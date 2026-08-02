#include <MiniAudioPlugin/MiniAudioPluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <MiniAudioPlugin/Events/MiniAudioAudioEvents.h>
#include <MiniAudioPlugin/Mixer/MiniAudioBusGraph.h>
#include <MiniAudioPlugin/MiniAudioAtlData.h>

// ---------------------------------------------------------------------------------------------
// plMiniAudioEventInstance
// ---------------------------------------------------------------------------------------------

plMiniAudioEventInstance::~plMiniAudioEventInstance()
{
  for (auto& voice : m_Voices)
  {
    StopVoice(voice);
  }
  m_Voices.Clear();

  if (m_pEventData != nullptr && m_pEventData->m_pInstance == this)
  {
    m_pEventData->m_pInstance = nullptr;
  }

  if (m_pResource != nullptr)
  {
    plResourceManager::EndAcquireResource(const_cast<plAudioEventResource*>(m_pResource));
    m_pResource = nullptr;
  }
}

plResult plMiniAudioEventInstance::Start(const plAudioEventResourceHandle& hEvent, plMiniAudioEntityData* pEntity, plMiniAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine, plRandom& ref_rng)
{
  // acquire and keep the resource alive for the lifetime of this instance — the decoders
  // read straight out of the resource's sample buffers
  plAudioEventResource* pResource = plResourceManager::BeginAcquireResource(hEvent, plResourceAcquireMode::BlockTillLoaded_NeverFail);

  if (pResource == nullptr || pResource->GetLoadingState() != plResourceState::Loaded)
  {
    if (pResource != nullptr)
    {
      plResourceManager::EndAcquireResource(pResource);
    }
    return PL_FAILURE;
  }

  m_pResource = pResource;
  m_pEntity = pEntity;
  m_pEventData = pEventData;
  m_pBusGraph = pBusGraph;
  m_pEngine = pEngine;

  if (m_pEventData != nullptr)
  {
    m_pEventData->m_pInstance = this;
  }

  const auto& desc = m_pResource->GetDescriptor();

  m_ParameterValues.SetCount(desc.m_Parameters.GetCount());
  for (plUInt32 i = 0; i < desc.m_Parameters.GetCount(); ++i)
  {
    m_ParameterValues[i] = desc.m_Parameters[i].m_fDefault;
  }

  m_Voices.SetCount(desc.m_Instruments.GetCount());
  m_Instruments.SetCount(desc.m_Instruments.GetCount());

  for (plUInt32 i = 0; i < desc.m_Instruments.GetCount(); ++i)
  {
    // switch filter is evaluated when the event starts, so a switch change affects the next trigger
    const bool bSwitchAllows = m_pEntity == nullptr || m_pEntity->MatchesSwitchFilter(desc.m_Instruments[i].m_sSwitchState);
    const bool bSkip = desc.m_Instruments[i].m_Playlist.IsEmpty() || desc.m_Instruments[i].m_bMute || !bSwitchAllows;

    m_Instruments[i].m_NextTriggerTime = desc.m_Instruments[i].m_StartDelay;
    m_Instruments[i].m_bPendingTrigger = !bSkip;
    m_Instruments[i].m_bDone = bSkip;
  }

  // start all instruments that have no delay right away, so one-shots have zero added latency
  for (plUInt32 i = 0; i < desc.m_Instruments.GetCount(); ++i)
  {
    if (m_Instruments[i].m_bPendingTrigger && m_Instruments[i].m_NextTriggerTime <= plTime::MakeZero())
    {
      StartVoice(i, ref_rng);
    }
  }

  return PL_SUCCESS;
}

plUInt16 plMiniAudioEventInstance::PickPlaylistEntry(const plAudioEventInstrumentDesc& instrument, InstrumentState& state, plRandom& ref_rng)
{
  const auto& playlist = instrument.m_Playlist;

  if (playlist.IsEmpty())
    return plAudioEventSilenceSample;

  if (instrument.m_ePlaylistMode == plAudioEventPlaylistMode::Sequential)
  {
    const plUInt32 uiIndex = state.m_uiCursor % playlist.GetCount();
    ++state.m_uiCursor;
    return playlist[uiIndex].m_uiSample;
  }

  // weighted random (Shuffle re-rolls once when it hits the previous entry)
  float fTotalWeight = 0.0f;
  for (const auto& entry : playlist)
    fTotalWeight += plMath::Max(entry.m_fWeight, 0.0f);

  if (fTotalWeight <= 0.0f)
    return playlist[0].m_uiSample;

  for (plUInt32 uiAttempt = 0; uiAttempt < 2; ++uiAttempt)
  {
    float fPick = static_cast<float>(ref_rng.DoubleZeroToOneExclusive()) * fTotalWeight;
    plUInt16 uiPicked = playlist.PeekBack().m_uiSample;
    plUInt32 uiPickedIndex = playlist.GetCount() - 1;

    for (plUInt32 i = 0; i < playlist.GetCount(); ++i)
    {
      fPick -= plMath::Max(playlist[i].m_fWeight, 0.0f);
      if (fPick <= 0.0f)
      {
        uiPicked = playlist[i].m_uiSample;
        uiPickedIndex = i;
        break;
      }
    }

    if (instrument.m_ePlaylistMode == plAudioEventPlaylistMode::Shuffle && playlist.GetCount() > 1 && uiPickedIndex == state.m_uiLastEntry && uiAttempt == 0)
      continue; // re-roll once to avoid immediate repetition

    state.m_uiLastEntry = static_cast<plUInt16>(uiPickedIndex);
    return uiPicked;
  }

  return playlist[0].m_uiSample;
}

float plMiniAudioEventInstance::GetOcclusionCutoff() const
{
  const float fOcclusion = (m_pEntity != nullptr) ? plMath::Clamp(m_pEntity->m_fCurrentOcclusion, 0.0f, 1.0f) : 0.0f;

  // exponential sweep from fully open (22 kHz) down to muffled (250 Hz)
  return 22000.0f * plMath::Pow(250.0f / 22000.0f, fOcclusion);
}

void plMiniAudioEventInstance::StartVoice(plUInt32 uiInstrument, plRandom& ref_rng)
{
  const auto& desc = m_pResource->GetDescriptor();
  const auto& instrumentDesc = desc.m_Instruments[uiInstrument];
  auto& state = m_Instruments[uiInstrument];
  auto& voice = m_Voices[uiInstrument];

  state.m_bPendingTrigger = false;

  const plUInt16 uiSample = PickPlaylistEntry(instrumentDesc, state, ref_rng);

  if (uiSample == plAudioEventSilenceSample || uiSample >= m_pResource->GetNumSamples())
  {
    // silence entry: for looping instruments schedule the next re-trigger, otherwise the instrument is done
    if (instrumentDesc.m_bLoop && !m_bStopping)
    {
      const double fMin = instrumentDesc.m_MinRetriggerDelay.GetSeconds();
      const double fMax = plMath::Max(fMin, instrumentDesc.m_MaxRetriggerDelay.GetSeconds());
      state.m_NextTriggerTime = m_Elapsed + plTime::MakeFromSeconds(plMath::Max(0.05, ref_rng.DoubleMinMax(fMin, fMax)));
      state.m_bPendingTrigger = true;
    }
    else
    {
      state.m_bDone = true;
    }
    return;
  }

  const plDataBuffer& sampleData = m_pResource->GetSampleData(uiSample);

  if (ma_decoder_init_memory(sampleData.GetData(), sampleData.GetCount(), nullptr, &voice.m_Decoder) != MA_SUCCESS)
  {
    state.m_bDone = true;
    return;
  }
  voice.m_bDecoderValid = true;

  if (ma_sound_init_from_data_source(m_pEngine, &voice.m_Decoder, 0, nullptr, &voice.m_Sound) != MA_SUCCESS)
  {
    ma_decoder_uninit(&voice.m_Decoder);
    voice.m_bDecoderValid = false;
    state.m_bDone = true;
    return;
  }

  // gapless looping for single-entry looping instruments without retrigger gaps
  const bool bGaplessLoop = instrumentDesc.m_bLoop && instrumentDesc.m_Playlist.GetCount() == 1 &&
                            instrumentDesc.m_MinRetriggerDelay <= plTime::MakeZero() && instrumentDesc.m_MaxRetriggerDelay <= plTime::MakeZero();
  if (bGaplessLoop)
  {
    ma_data_source_set_looping(&voice.m_Decoder, true);
  }

  // spatializer
  ma_sound_set_spatialization_enabled(&voice.m_Sound, desc.m_bSpatialize && (m_pEntity == nullptr || !m_pEntity->m_bGlobalEntity));
  ma_sound_set_min_distance(&voice.m_Sound, desc.m_fMinDistance);
  ma_sound_set_rolloff(&voice.m_Sound, desc.m_fRolloff);
  ma_sound_set_doppler_factor(&voice.m_Sound, desc.m_fDopplerFactor);

  if (m_pEntity != nullptr)
  {
    const plVec3 vPos = m_pEntity->m_LastTransform.m_vPosition;
    const plVec3 vVel = m_pEntity->m_LastTransform.m_vVelocity;
    ma_sound_set_position(&voice.m_Sound, vPos.x, vPos.y, vPos.z);
    ma_sound_set_velocity(&voice.m_Sound, vVel.x, vVel.y, vVel.z);
  }

  // randomized base volume / pitch
  const float fVolDb = instrumentDesc.m_fVolumeDb + static_cast<float>(ref_rng.DoubleMinMax(-instrumentDesc.m_fVolumeRandDb, instrumentDesc.m_fVolumeRandDb));
  const float fPitchSt = instrumentDesc.m_fPitchSemitones + static_cast<float>(ref_rng.DoubleMinMax(-instrumentDesc.m_fPitchRandSemitones, instrumentDesc.m_fPitchRandSemitones));
  voice.m_fBaseVolume = plAudioMixerUtils::DbToLinear(fVolDb);
  voice.m_fBasePitch = plAudioMixerUtils::SemitonesToPitch(fPitchSt);

  // routing target: the event's bus (or the engine endpoint when no mixer graph is built)
  ma_node* pTarget = nullptr;
  if (m_pBusGraph != nullptr && m_pBusGraph->IsBuilt())
  {
    pTarget = reinterpret_cast<ma_node*>(m_pBusGraph->GetBusInput(m_pBusGraph->ResolveBus(desc.m_sOutputBus)));
  }
  if (pTarget == nullptr)
  {
    pTarget = ma_node_graph_get_endpoint(ma_engine_get_node_graph(m_pEngine));
  }

  // voice chain: sound -> occlusion LPF -> [env splitter] -> bus
  ma_node* pCurrent = reinterpret_cast<ma_node*>(&voice.m_Sound);

  voice.m_pOcclusionLpf = PL_DEFAULT_NEW(ma_lpf_node);
  const ma_lpf_node_config lpfCfg = ma_lpf_node_config_init(ma_engine_get_channels(m_pEngine), ma_engine_get_sample_rate(m_pEngine), GetOcclusionCutoff(), 2);
  if (ma_lpf_node_init(ma_engine_get_node_graph(m_pEngine), &lpfCfg, nullptr, voice.m_pOcclusionLpf) == MA_SUCCESS)
  {
    ma_node_attach_output_bus(pCurrent, 0, reinterpret_cast<ma_node*>(voice.m_pOcclusionLpf), 0);
    pCurrent = reinterpret_cast<ma_node*>(voice.m_pOcclusionLpf);
  }
  else
  {
    PL_DEFAULT_DELETE(voice.m_pOcclusionLpf);
  }

  // environment splitter only when the mixer actually has environment aux buses
  bool bCreateSplitter = false;
  if (m_pBusGraph != nullptr && m_pBusGraph->IsBuilt())
  {
    for (const auto& busDesc : m_pBusGraph->GetDesc().m_Buses)
    {
      if (busDesc.m_bIsAux && !busDesc.m_sEnvironmentName.IsEmpty())
      {
        bCreateSplitter = true;
        break;
      }
    }
  }

  if (bCreateSplitter)
  {
    voice.m_pEnvSplitter = PL_DEFAULT_NEW(ma_splitter_node);
    ma_splitter_node_config splitCfg = ma_splitter_node_config_init(ma_engine_get_channels(m_pEngine));
    splitCfg.outputBusCount = 2;

    if (ma_splitter_node_init(ma_engine_get_node_graph(m_pEngine), &splitCfg, nullptr, voice.m_pEnvSplitter) == MA_SUCCESS)
    {
      ma_node* pSplitter = reinterpret_cast<ma_node*>(voice.m_pEnvSplitter);
      ma_node_attach_output_bus(pCurrent, 0, pSplitter, 0);
      ma_node_attach_output_bus(pSplitter, 0, pTarget, 0); // dry
      ma_node_set_output_bus_volume(pSplitter, 1, 0.0f);   // wet starts silent; Update() routes and levels it
      pCurrent = nullptr;                                  // chain fully wired through splitter
    }
    else
    {
      PL_DEFAULT_DELETE(voice.m_pEnvSplitter);
    }
  }

  if (pCurrent != nullptr)
  {
    ma_node_attach_output_bus(pCurrent, 0, pTarget, 0);
  }

  ApplyModulation(); // sets initial volume/pitch on all voices, including this one
  ma_sound_set_volume(&voice.m_Sound, voice.m_fBaseVolume * m_fCurrentVolumeMultiplier);
  ma_sound_set_pitch(&voice.m_Sound, voice.m_fBasePitch * m_fCurrentPitchMultiplier);

  if (ma_sound_start(&voice.m_Sound) != MA_SUCCESS)
  {
    StopVoice(voice);
    state.m_bDone = true;
    return;
  }

  // authored fade-in (applies to every voice start, including loop re-triggers)
  if (desc.m_FadeIn > plTime::MakeZero())
  {
    ma_sound_set_fade_in_milliseconds(&voice.m_Sound, 0.0f, voice.m_fBaseVolume * m_fCurrentVolumeMultiplier, static_cast<ma_uint64>(desc.m_FadeIn.GetMilliseconds()));
  }

  voice.m_bActive = true;
}

void plMiniAudioEventInstance::StopVoice(Voice& voice)
{
  if (voice.m_bActive)
  {
    ma_sound_stop(&voice.m_Sound);
    ma_sound_uninit(&voice.m_Sound);
    voice.m_bActive = false;
  }

  if (voice.m_pEnvSplitter != nullptr)
  {
    ma_splitter_node_uninit(voice.m_pEnvSplitter, nullptr);
    PL_DEFAULT_DELETE(voice.m_pEnvSplitter);
  }

  if (voice.m_pOcclusionLpf != nullptr)
  {
    ma_lpf_node_uninit(voice.m_pOcclusionLpf, nullptr);
    PL_DEFAULT_DELETE(voice.m_pOcclusionLpf);
  }

  if (voice.m_bDecoderValid)
  {
    ma_decoder_uninit(&voice.m_Decoder);
    voice.m_bDecoderValid = false;
  }
}

void plMiniAudioEventInstance::Stop(bool bImmediate)
{
  if (m_bStopping && !bImmediate)
    return;

  m_bStopping = true;

  const plTime fadeOut = (m_pResource != nullptr) ? m_pResource->GetDescriptor().m_StopFadeOut : plTime::MakeZero();

  for (auto& voice : m_Voices)
  {
    if (!voice.m_bActive)
      continue;

    if (bImmediate || fadeOut <= plTime::MakeZero())
    {
      StopVoice(voice);
    }
    else
    {
      ma_data_source_set_looping(&voice.m_Decoder, false);
      ma_sound_stop_with_fade_in_milliseconds(&voice.m_Sound, static_cast<ma_uint64>(fadeOut.GetMilliseconds()));
    }
  }

  // no further re-triggers
  for (auto& state : m_Instruments)
  {
    state.m_bPendingTrigger = false;
    state.m_bDone = true;
  }
}

void plMiniAudioEventInstance::SetParameter(plStringView sName, float fValue)
{
  if (m_pResource == nullptr)
    return;

  const auto& desc = m_pResource->GetDescriptor();
  const plUInt32 uiParam = desc.FindParameterByName(sName);

  if (uiParam == plInvalidIndex || uiParam >= m_ParameterValues.GetCount())
    return;

  m_ParameterValues[uiParam] = plMath::Clamp(fValue, desc.m_Parameters[uiParam].m_fMin, desc.m_Parameters[uiParam].m_fMax);
}

void plMiniAudioEventInstance::ApplyModulation()
{
  const auto& desc = m_pResource->GetDescriptor();

  float fVolume = 1.0f;
  float fPitch = 1.0f;

  for (const auto& automation : desc.m_Automations)
  {
    if (automation.m_uiParameter >= m_ParameterValues.GetCount() || automation.m_Curve.IsEmpty())
      continue;

    const float fParamValue = m_ParameterValues[automation.m_uiParameter];
    const float fCurveValue = static_cast<float>(automation.m_Curve.Evaluate(fParamValue));

    switch (automation.m_eTarget)
    {
      case plAudioEventAutomationTarget::EventVolume:
        fVolume *= plMath::Max(fCurveValue, 0.0f);
        break;
      case plAudioEventAutomationTarget::EventPitch:
        fPitch *= plMath::Clamp(fCurveValue, 0.01f, 10.0f);
        break;
    }
  }

  // obstruction scales the direct signal down
  if (m_pEntity != nullptr)
  {
    fVolume *= 1.0f - 0.85f * plMath::Clamp(m_pEntity->m_fCurrentObstruction, 0.0f, 1.0f);
  }

  m_fCurrentVolumeMultiplier = fVolume;
  m_fCurrentPitchMultiplier = fPitch;
}

plUInt32 plMiniAudioEventInstance::GetActiveVoiceCount() const
{
  plUInt32 uiCount = 0;
  for (const auto& voice : m_Voices)
  {
    if (voice.m_bActive)
      ++uiCount;
  }
  return uiCount;
}

bool plMiniAudioEventInstance::Update(plTime delta, const plVec3& vListenerPos, plRandom& ref_rng)
{
  if (m_pResource == nullptr)
    return false;

  m_Elapsed += delta;

  const auto& desc = m_pResource->GetDescriptor();

  // smooth occlusion state on the entity (shared by all instances on it; cheap to repeat)
  if (m_pEntity != nullptr)
  {
    const float fSmooth = plMath::Min(1.0f, static_cast<float>(delta.GetSeconds()) * 8.0f);
    m_pEntity->m_fCurrentObstruction = plMath::Lerp(m_pEntity->m_fCurrentObstruction, m_pEntity->m_fTargetObstruction, fSmooth);
    m_pEntity->m_fCurrentOcclusion = plMath::Lerp(m_pEntity->m_fCurrentOcclusion, m_pEntity->m_fTargetOcclusion, fSmooth);
  }

  // built-in Distance parameter
  if (m_pEntity != nullptr && !m_pEntity->m_bGlobalEntity)
  {
    const plUInt32 uiDistanceParam = desc.FindParameterByName("Distance");
    if (uiDistanceParam != plInvalidIndex && uiDistanceParam < m_ParameterValues.GetCount())
    {
      const float fDistance = (m_pEntity->m_LastTransform.m_vPosition - vListenerPos).GetLength();
      m_ParameterValues[uiDistanceParam] = plMath::Clamp(fDistance, desc.m_Parameters[uiDistanceParam].m_fMin, desc.m_Parameters[uiDistanceParam].m_fMax);
    }
  }

  ApplyModulation();

  const float fOcclusionCutoff = GetOcclusionCutoff();
  const auto* pDominantEnv = (m_pEntity != nullptr) ? m_pEntity->GetDominantEnvironment() : nullptr;

  bool bAnyActive = false;
  bool bAnyPending = false;

  for (plUInt32 i = 0; i < m_Voices.GetCount(); ++i)
  {
    auto& voice = m_Voices[i];
    auto& state = m_Instruments[i];
    const auto& instrumentDesc = desc.m_Instruments[i];

    if (voice.m_bActive)
    {
      const bool bEnded = ma_sound_at_end(&voice.m_Sound) != 0;
      const bool bFadedOut = m_bStopping && ma_sound_get_current_fade_volume(&voice.m_Sound) <= 0.0f;
      const bool bStopped = m_bStopping && ma_sound_is_playing(&voice.m_Sound) == 0;

      if (bEnded || bFadedOut || bStopped)
      {
        StopVoice(voice);

        if (!m_bStopping && instrumentDesc.m_bLoop)
        {
          const double fMin = instrumentDesc.m_MinRetriggerDelay.GetSeconds();
          const double fMax = plMath::Max(fMin, instrumentDesc.m_MaxRetriggerDelay.GetSeconds());
          state.m_NextTriggerTime = m_Elapsed + plTime::MakeFromSeconds(ref_rng.DoubleMinMax(fMin, fMax));
          state.m_bPendingTrigger = true;
        }
        else
        {
          state.m_bDone = true;
        }
      }
      else
      {
        bAnyActive = true;

        // follow the entity
        if (m_pEntity != nullptr && !m_pEntity->m_bGlobalEntity)
        {
          const plVec3 vPos = m_pEntity->m_LastTransform.m_vPosition;
          const plVec3 vVel = m_pEntity->m_LastTransform.m_vVelocity;
          ma_sound_set_position(&voice.m_Sound, vPos.x, vPos.y, vPos.z);
          ma_sound_set_velocity(&voice.m_Sound, vVel.x, vVel.y, vVel.z);
        }

        if (!m_bStopping)
        {
          ma_sound_set_volume(&voice.m_Sound, voice.m_fBaseVolume * m_fCurrentVolumeMultiplier);
          ma_sound_set_pitch(&voice.m_Sound, voice.m_fBasePitch * m_fCurrentPitchMultiplier);
        }

        // occlusion low-pass
        if (voice.m_pOcclusionLpf != nullptr)
        {
          const ma_lpf_config lpfCfg = ma_lpf_config_init(ma_format_f32, ma_engine_get_channels(m_pEngine), ma_engine_get_sample_rate(m_pEngine), fOcclusionCutoff, 2);
          ma_lpf_node_reinit(&lpfCfg, voice.m_pOcclusionLpf);
        }

        // environment send
        if (voice.m_pEnvSplitter != nullptr && m_pBusGraph != nullptr)
        {
          ma_node* pSplitter = reinterpret_cast<ma_node*>(voice.m_pEnvSplitter);

          if (pDominantEnv != nullptr)
          {
            if (ma_sound_group* pAux = m_pBusGraph->GetEnvironmentAuxBus(pDominantEnv->m_sName))
            {
              ma_node_attach_output_bus(pSplitter, 1, reinterpret_cast<ma_node*>(pAux), 0);
              ma_node_set_output_bus_volume(pSplitter, 1, plMath::Clamp(pDominantEnv->m_fAmount, 0.0f, 1.0f));
            }
          }
          else
          {
            ma_node_set_output_bus_volume(pSplitter, 1, 0.0f);
          }
        }
      }
    }
    else if (state.m_bPendingTrigger && !m_bStopping)
    {
      if (m_Elapsed >= state.m_NextTriggerTime)
      {
        StartVoice(i, ref_rng);
        if (voice.m_bActive)
          bAnyActive = true;
      }

      if (state.m_bPendingTrigger)
        bAnyPending = true;
    }
  }

  // the instance lives while anything plays or is scheduled; looping events with m_bLooping
  // event data are kept alive until an explicit stop even if momentarily silent
  const bool bHoldOpen = !m_bStopping && m_pEventData != nullptr && m_pEventData->m_bLooping;

  // authored loop region: a looping event repeats its [start, end] window instead of the
  // whole timeline, so instruments inside the window are re-armed when the window ends
  if (bHoldOpen && desc.m_bLoopRegion && desc.m_LoopEnd > desc.m_LoopStart && m_Elapsed >= desc.m_LoopEnd)
  {
    m_Elapsed = desc.m_LoopStart;

    for (plUInt32 i = 0; i < m_Instruments.GetCount(); ++i)
    {
      const auto& instrumentDesc = desc.m_Instruments[i];

      if (instrumentDesc.m_bMute || instrumentDesc.m_Playlist.IsEmpty())
        continue;

      if (m_pEntity != nullptr && !m_pEntity->MatchesSwitchFilter(instrumentDesc.m_sSwitchState))
        continue;

      // re-arm instruments that start inside the loop window
      if (instrumentDesc.m_StartDelay >= desc.m_LoopStart && instrumentDesc.m_StartDelay < desc.m_LoopEnd && !m_Voices[i].m_bActive)
      {
        m_Instruments[i].m_NextTriggerTime = instrumentDesc.m_StartDelay;
        m_Instruments[i].m_bPendingTrigger = true;
        m_Instruments[i].m_bDone = false;
      }
    }

    return true;
  }

  return bAnyActive || bAnyPending || bHoldOpen;
}

// ---------------------------------------------------------------------------------------------
// plMiniAudioEventManager
// ---------------------------------------------------------------------------------------------

plResult plMiniAudioEventManager::ActivateEvent(plStringView sEventResourceId, plMiniAudioEntityData* pEntity, plMiniAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine)
{
  if (!m_bRngInitialized)
  {
    m_Rng.InitializeFromCurrentTime();
    m_bRngInitialized = true;
  }

  const plAudioEventResourceHandle hEvent = plResourceManager::LoadResource<plAudioEventResource>(sEventResourceId);
  if (!hEvent.IsValid())
    return PL_FAILURE;

  const plUInt64 uiGroupHash = plHashHelper<plStringView>::Hash(sEventResourceId);

  // peek at the descriptor for polyphony rules
  plUInt8 uiMaxInstances = 8;
  plAudioEventStealingMode::Enum eStealing = plAudioEventStealingMode::Default;
  plTime cooldown = plTime::MakeZero();

  {
    plResourceLock<plAudioEventResource> pResource(hEvent, plResourceAcquireMode::BlockTillLoaded_NeverFail);
    if (pResource.GetAcquireResult() != plResourceAcquireResult::Final)
      return PL_FAILURE;

    uiMaxInstances = pResource->GetDescriptor().m_uiMaxInstances;
    eStealing = pResource->GetDescriptor().m_eStealing;
    cooldown = pResource->GetDescriptor().m_Cooldown;
  }

  auto& group = m_EventGroups[uiGroupHash];

  // cooldown
  if (group.m_bHasStarted && cooldown > plTime::MakeZero() && (m_Now - group.m_LastStartTime) < cooldown)
    return PL_SUCCESS; // silently swallowed, not an error

  // polyphony
  if (uiMaxInstances > 0)
  {
    plHybridArray<plMiniAudioEventInstance*, 8> groupInstances;
    for (auto& pInstance : m_Instances)
    {
      if (pInstance->m_uiEventGroupHash == uiGroupHash && !pInstance->IsStopping())
        groupInstances.PushBack(pInstance.Borrow());
    }

    if (groupInstances.GetCount() >= uiMaxInstances)
    {
      switch (eStealing)
      {
        case plAudioEventStealingMode::None:
          return PL_SUCCESS; // new instance is not started

        case plAudioEventStealingMode::Oldest:
        {
          plMiniAudioEventInstance* pOldest = groupInstances[0];
          for (auto* pInstance : groupInstances)
          {
            if (pInstance->GetAge() > pOldest->GetAge())
              pOldest = pInstance;
          }
          pOldest->Stop(false);
          break;
        }

        case plAudioEventStealingMode::Quietest:
        {
          plMiniAudioEventInstance* pQuietest = groupInstances[0];
          for (auto* pInstance : groupInstances)
          {
            if (pInstance->GetEffectiveVolume() < pQuietest->GetEffectiveVolume())
              pQuietest = pInstance;
          }
          pQuietest->Stop(false);
          break;
        }
      }
    }
  }

  plUniquePtr<plMiniAudioEventInstance> pInstance = PL_DEFAULT_NEW(plMiniAudioEventInstance);
  pInstance->m_uiEventGroupHash = uiGroupHash;

  if (pInstance->Start(hEvent, pEntity, pEventData, pBusGraph, pEngine, m_Rng).Failed())
  {
    return PL_FAILURE;
  }

  group.m_LastStartTime = m_Now;
  group.m_bHasStarted = true;

  m_Instances.PushBack(std::move(pInstance));
  return PL_SUCCESS;
}

plResult plMiniAudioEventManager::StopEvent(const plMiniAudioEventData* pEventData, bool bImmediate)
{
  for (auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEventData == pEventData)
    {
      pInstance->Stop(bImmediate);
      return PL_SUCCESS;
    }
  }

  return PL_FAILURE;
}

void plMiniAudioEventManager::StopAllOnEntity(const plMiniAudioEntityData* pEntity, bool bImmediate)
{
  for (auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEntity == pEntity)
    {
      pInstance->Stop(bImmediate);

      if (bImmediate)
      {
        // the entity data is about to be destroyed — sever the link
        pInstance->m_pEntity = nullptr;
      }
    }
  }
}

void plMiniAudioEventManager::StopAll(bool bImmediate)
{
  for (auto& pInstance : m_Instances)
  {
    pInstance->Stop(bImmediate);
  }

  if (bImmediate)
  {
    m_Instances.Clear();
  }
}

void plMiniAudioEventManager::SetEventParameter(const plMiniAudioEntityData* pEntity, plStringView sParamName, float fValue)
{
  for (auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEntity == pEntity)
    {
      pInstance->SetParameter(sParamName, fValue);
    }
  }
}

bool plMiniAudioEventManager::IsEventPlaying(const plMiniAudioEventData* pEventData) const
{
  for (const auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEventData == pEventData)
      return true;
  }

  return false;
}

void plMiniAudioEventManager::Update(plTime delta, const plVec3& vListenerPos)
{
  m_Now += delta;

  for (plUInt32 i = 0; i < m_Instances.GetCount();)
  {
    if (!m_Instances[i]->Update(delta, vListenerPos, m_Rng))
    {
      m_Instances.RemoveAtAndSwap(i);
    }
    else
    {
      ++i;
    }
  }
}

plUInt32 plMiniAudioEventManager::GetActiveVoiceCount() const
{
  plUInt32 uiCount = 0;
  for (const auto& pInstance : m_Instances)
  {
    uiCount += pInstance->GetActiveVoiceCount();
  }
  return uiCount;
}
