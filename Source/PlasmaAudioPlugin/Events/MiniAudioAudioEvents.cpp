#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <Foundation/Threading/Lock.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResource.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResourceLoader.h>
#include <PlasmaAudioPlugin/Events/MiniAudioAudioEvents.h>
#include <PlasmaAudioPlugin/Mixer/AudioMixerService.h>
#include <PlasmaAudioPlugin/Mixer/MiniAudioBusGraph.h>
#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>
#include <PlasmaAudioPlugin/Streaming/AudioStreamService.h>

// ---------------------------------------------------------------------------------------------
// plMaVoiceDataSource
// ---------------------------------------------------------------------------------------------

namespace
{
  ma_result VoiceSourceRead(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 uiFrameCount, ma_uint64* pFramesRead)
  {
    auto* pSource = static_cast<plMaVoiceDataSource*>(pDataSource);

    // the first read happens while the engine mixes the chunk the sound started in, and the engine time is that chunk's first frame
    if (!pSource->m_bLeadResolved)
    {
      const ma_uint64 uiNow = ma_engine_get_time_in_pcm_frames(pSource->m_pEngine);
      if (pSource->m_uiStartFrame > uiNow)
      {
        pSource->m_uiLeadFramesRemaining = static_cast<ma_uint64>(static_cast<double>(pSource->m_uiStartFrame - uiNow) * pSource->m_fSourceFramesPerEngineFrame + 0.5);
      }
      pSource->m_bLeadResolved = true;
    }

    // The sound applies its fader to the frames of this read after the read returns, so a fader set here takes effect
    // from the engine frame at which the data begins. A fade the main thread sets later replaces it, starting from the
    // fader's current volume.
    if (!pSource->m_bStartFadeApplied)
    {
      pSource->m_bStartFadeApplied = true;

      if (pSource->m_uiStartFadeFrames > 0 && pSource->m_pStartFader != nullptr)
      {
        const ma_uint64 uiNow = ma_engine_get_time_in_pcm_frames(pSource->m_pEngine);
        const ma_uint64 uiFadeStart = plMath::Max(pSource->m_uiStartFrame, uiNow);
        ma_fader_set_fade_ex(pSource->m_pStartFader, 0.0f, pSource->m_fStartFadeVolume, pSource->m_uiStartFadeFrames, static_cast<ma_int64>(uiFadeStart - uiNow));
      }
    }

    ma_uint64 uiRead = 0;

    if (pSource->m_uiLeadFramesRemaining > 0)
    {
      uiRead = plMath::Min(pSource->m_uiLeadFramesRemaining, uiFrameCount);
      if (pFramesOut != nullptr)
      {
        ma_silence_pcm_frames(pFramesOut, uiRead, pSource->m_eFormat, pSource->m_uiChannels);
      }
      pSource->m_uiLeadFramesRemaining -= uiRead;
    }

    if (uiRead < uiFrameCount && !pSource->m_bPadded)
    {
      void* pInnerOut = (pFramesOut != nullptr) ? ma_offset_pcm_frames_ptr(pFramesOut, uiRead, pSource->m_eFormat, pSource->m_uiChannels) : nullptr;

      ma_uint64 uiInnerRead = 0;
      const ma_result res = ma_data_source_read_pcm_frames(pSource->m_pInner, pInnerOut, uiFrameCount - uiRead, &uiInnerRead);
      uiRead += uiInnerRead;

      if (res != MA_SUCCESS && res != MA_AT_END)
      {
        *pFramesRead = uiRead;
        return res;
      }

      // the inner source is done: append the silent frame
      if (uiRead < uiFrameCount && (res == MA_AT_END || uiInnerRead == 0))
      {
        if (pFramesOut != nullptr)
        {
          ma_silence_pcm_frames(ma_offset_pcm_frames_ptr(pFramesOut, uiRead, pSource->m_eFormat, pSource->m_uiChannels), 1, pSource->m_eFormat, pSource->m_uiChannels);
        }

        pSource->m_bPadded = true;
        ++uiRead;
      }
    }

    *pFramesRead = uiRead;
    return uiRead > 0 ? MA_SUCCESS : MA_AT_END;
  }

  ma_result VoiceSourceSeek(ma_data_source* pDataSource, ma_uint64 uiFrameIndex)
  {
    auto* pSource = static_cast<plMaVoiceDataSource*>(pDataSource);
    pSource->m_uiLeadFramesRemaining = 0;
    pSource->m_bLeadResolved = true;
    pSource->m_bPadded = false;
    return ma_data_source_seek_to_pcm_frame(pSource->m_pInner, uiFrameIndex);
  }

  ma_result VoiceSourceGetDataFormat(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t uiChannelMapCap)
  {
    return ma_data_source_get_data_format(static_cast<plMaVoiceDataSource*>(pDataSource)->m_pInner, pFormat, pChannels, pSampleRate, pChannelMap, uiChannelMapCap);
  }

  ma_result VoiceSourceGetCursor(ma_data_source* pDataSource, ma_uint64* pCursor)
  {
    return ma_data_source_get_cursor_in_pcm_frames(static_cast<plMaVoiceDataSource*>(pDataSource)->m_pInner, pCursor);
  }

  ma_result VoiceSourceGetLength(ma_data_source* pDataSource, ma_uint64* pLength)
  {
    return ma_data_source_get_length_in_pcm_frames(static_cast<plMaVoiceDataSource*>(pDataSource)->m_pInner, pLength);
  }

  ma_data_source_vtable s_VoiceSourceVTable = {VoiceSourceRead, VoiceSourceSeek, VoiceSourceGetDataFormat, VoiceSourceGetCursor, VoiceSourceGetLength, nullptr, 0};

  ma_result InitVoiceDataSource(ma_data_source* pInner, ma_engine* pEngine, ma_uint64 uiStartFrame, plMaVoiceDataSource* pSource)
  {
    pSource->m_pInner = pInner;
    pSource->m_pEngine = pEngine;
    pSource->m_uiStartFrame = uiStartFrame;
    pSource->m_fSourceFramesPerEngineFrame = 1.0;
    pSource->m_uiLeadFramesRemaining = 0;
    pSource->m_pStartFader = nullptr;
    pSource->m_uiStartFadeFrames = 0;
    pSource->m_fStartFadeVolume = 1.0f;
    pSource->m_bLeadResolved = false;
    pSource->m_bStartFadeApplied = false;
    pSource->m_bPadded = false;

    const ma_result formatResult = ma_data_source_get_data_format(pInner, &pSource->m_eFormat, &pSource->m_uiChannels, nullptr, nullptr, 0);
    if (formatResult != MA_SUCCESS)
      return formatResult;

    ma_data_source_config cfg = ma_data_source_config_init();
    cfg.vtable = &s_VoiceSourceVTable;
    return ma_data_source_init(&cfg, &pSource->m_Base);
  }
} // namespace

// ---------------------------------------------------------------------------------------------
// plPlasmaAudioEventInstance
// ---------------------------------------------------------------------------------------------

plPlasmaAudioEventInstance::~plPlasmaAudioEventInstance()
{
  Reset();
}

void plPlasmaAudioEventInstance::Reset()
{
  for (auto& voice : m_Voices)
  {
    StopVoice(voice);
  }

  if (m_pEventData != nullptr && m_pEventData->m_pInstance == this)
  {
    m_pEventData->m_pInstance = nullptr;
  }

  if (m_pResource != nullptr)
  {
    plResourceManager::EndAcquireResource(const_cast<plAudioEventResource*>(m_pResource));
    m_pResource = nullptr;
  }

  // Clear() keeps the capacity; the clip sources stay allocated and are assigned to voices again
  m_Voices.Clear();
  m_Instruments.Clear();
  m_ParameterValues.Clear();
  m_ParameterNameHashes.Clear();
  m_ParameterLocal.Clear();
  m_uiClipSourcesAssigned = 0;

  m_pEntity = nullptr;
  m_pEventData = nullptr;
  m_uiEventGroupHash = 0;
  m_pStreams = nullptr;
  m_pVoiceManager = nullptr;
  m_pMixer = nullptr;
  m_pEventLog = nullptr;
  m_vListenerPos = plVec3::MakeZero();
  m_uiSerial = 0;

  m_hResource.Invalidate();
  m_pBusGraph = nullptr;
  m_pEngine = nullptr;
  m_pGlobalParameters = nullptr;

  m_uiStartFrame = 0;
  m_uiTimelineOriginFrame = 0;
  m_uiSampleRate = 48000;
  m_uiRegionLoopInstrument = plInvalidIndex;

  m_Elapsed = plTime::MakeZero();
  m_bStopping = false;
  m_fCurrentVolumeMultiplier = 1.0f;
  m_fCurrentPitchMultiplier = 1.0f;
  m_fAudibility = 1.0f;
  m_fDistance = 0.0f;
  m_uiBus = 0;
}

plResult plPlasmaAudioEventInstance::Start(const plAudioEventResourceHandle& hEvent, plPlasmaAudioEntityData* pEntity, plPlasmaAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine, const plHashTable<plUInt64, float>* pGlobalParameters, plRandom& ref_rng)
{
  // the caller loaded the resource and holds the resource manager mutex, so it cannot be unloaded in between; the
  // decoders read straight out of the resource's sample buffers until the instance ends or the resource is unloaded
  plAudioEventResource* pResource = plResourceManager::BeginAcquireResource(hEvent, plResourceAcquireMode::PointerOnly);

  if (pResource->GetLoadingState() != plResourceState::Loaded)
  {
    plResourceManager::EndAcquireResource(pResource);
    return PL_FAILURE;
  }

  m_hResource = hEvent;
  m_pResource = pResource;
  m_pEntity = pEntity;
  m_pEventData = pEventData;
  m_pBusGraph = pBusGraph;
  m_pEngine = pEngine;
  m_pGlobalParameters = pGlobalParameters;

  if (m_pEventData != nullptr)
  {
    m_pEventData->m_pInstance = this;
  }

  m_uiSampleRate = plMath::Max<ma_uint32>(ma_engine_get_sample_rate(m_pEngine), 1u);
  m_uiStartFrame = ma_engine_get_time_in_pcm_frames(m_pEngine);
  m_uiTimelineOriginFrame = m_uiStartFrame;

  const auto& desc = m_pResource->GetDescriptor();

  m_uiBus = (m_pBusGraph != nullptr && m_pBusGraph->IsBuilt()) ? m_pBusGraph->ResolveBus(desc.m_sOutputBus) : 0;

  // logged before the first voices are scheduled, so the entries about them follow the start
  if (m_pEventLog != nullptr)
  {
    plStringBuilder sDetail;
    if (m_pEntity != nullptr)
    {
      sDetail.SetFormat("entity #{}", m_pEntity->m_uiEntityId);
    }
    Log(plAudioLogEventType::Start, sDetail);
  }

  m_ParameterValues.SetCount(desc.m_Parameters.GetCount());
  m_ParameterNameHashes.SetCount(desc.m_Parameters.GetCount());
  m_ParameterLocal.SetCount(desc.m_Parameters.GetCount());
  for (plUInt32 i = 0; i < desc.m_Parameters.GetCount(); ++i)
  {
    m_ParameterValues[i] = desc.m_Parameters[i].m_fDefault;
    m_ParameterNameHashes[i] = plHashingUtils::StringHash(desc.m_Parameters[i].m_sName.GetView());
    m_ParameterLocal[i] = false;
  }

  ApplyGlobalParameters();

  // voices are never relocated after this point: miniaudio nodes reference them by address
  m_Voices.SetCount(desc.m_Instruments.GetCount() * VoiceSlotsPerInstrument);
  m_Instruments.SetCount(desc.m_Instruments.GetCount());

  const bool bRegionLooping = IsRegionLooping();
  plUInt32 uiInstrumentsInRegion = 0;

  for (plUInt32 i = 0; i < desc.m_Instruments.GetCount(); ++i)
  {
    const auto& instrumentDesc = desc.m_Instruments[i];

    // switch filter is evaluated when the event starts, so a switch change affects the next trigger
    const bool bSwitchAllows = m_pEntity == nullptr || m_pEntity->MatchesSwitchFilter(instrumentDesc.m_sSwitchState);

    // instruments behind the loop region are never reached while the region loops
    const bool bBehindRegion = bRegionLooping && instrumentDesc.m_StartDelay >= desc.m_LoopEnd;

    const bool bSkip = instrumentDesc.m_Playlist.IsEmpty() || instrumentDesc.m_bMute || !bSwitchAllows || bBehindRegion;

    m_Instruments[i].m_uiNextTriggerFrame = m_uiStartFrame + ToFrames(instrumentDesc.m_StartDelay);
    m_Instruments[i].m_bPendingTrigger = !bSkip;
    m_Instruments[i].m_bDone = bSkip;

    if (!bSkip && bRegionLooping && instrumentDesc.m_StartDelay >= desc.m_LoopStart)
    {
      ++uiInstrumentsInRegion;
      m_uiRegionLoopInstrument = i;
    }
  }

  // a region holding exactly one single-entry instrument that starts at the region start loops natively on its
  // inner source (loop point = region length), which is gapless and needs no re-arming; StartVoice falls back to
  // re-arming when the sample is shorter than the region
  if (uiInstrumentsInRegion == 1)
  {
    const auto& instrumentDesc = desc.m_Instruments[m_uiRegionLoopInstrument];
    const bool bRetriggerGaps = instrumentDesc.m_bLoop && (instrumentDesc.m_MinRetriggerDelay > plTime::MakeZero() || instrumentDesc.m_MaxRetriggerDelay > plTime::MakeZero());

    if (instrumentDesc.m_Playlist.GetCount() != 1 || instrumentDesc.m_StartDelay != desc.m_LoopStart || bRetriggerGaps)
    {
      m_uiRegionLoopInstrument = plInvalidIndex;
    }
  }
  else
  {
    m_uiRegionLoopInstrument = plInvalidIndex;
  }

  // every instrument whose start frame lies inside the look-ahead window is scheduled right away with its exact frame
  const ma_uint64 uiHorizon = m_uiStartFrame + plPlasmaAudioEventManager::GetLookAheadFrames(m_pEngine);
  for (plUInt32 i = 0; i < desc.m_Instruments.GetCount(); ++i)
  {
    if (m_Instruments[i].m_bPendingTrigger && m_Instruments[i].m_uiNextTriggerFrame <= uiHorizon)
    {
      StartVoice(i, 0, ref_rng, m_Instruments[i].m_uiNextTriggerFrame);
    }
  }

  return PL_SUCCESS;
}

bool plPlasmaAudioEventInstance::HasLoopingInstrument() const
{
  if (m_pResource == nullptr)
    return false;

  for (const auto& instrument : m_pResource->GetDescriptor().m_Instruments)
  {
    if (instrument.m_bLoop && !instrument.m_bMute && !instrument.m_Playlist.IsEmpty())
      return true;
  }

  return false;
}

bool plPlasmaAudioEventInstance::IsRegionLooping() const
{
  if (m_pResource == nullptr || m_bStopping || m_pEventData == nullptr || !m_pEventData->m_bLooping)
    return false;

  const auto& desc = m_pResource->GetDescriptor();
  return desc.m_bLoopRegion && desc.m_LoopEnd > desc.m_LoopStart;
}

ma_uint64 plPlasmaAudioEventInstance::ToFrames(plTime duration) const
{
  const double fSeconds = duration.GetSeconds();
  if (fSeconds <= 0.0)
    return 0;

  return static_cast<ma_uint64>(fSeconds * m_uiSampleRate + 0.5);
}

ma_uint64 plPlasmaAudioEventInstance::RollRetriggerGap(const plAudioEventInstrumentDesc& instrument, plRandom& ref_rng, double fMinSeconds) const
{
  const double fMin = instrument.m_MinRetriggerDelay.GetSeconds();
  const double fMax = plMath::Max(fMin, instrument.m_MaxRetriggerDelay.GetSeconds());
  const double fGap = plMath::Max(fMinSeconds, ref_rng.DoubleMinMax(fMin, fMax));
  return ToFrames(plTime::MakeFromSeconds(fGap));
}

ma_uint64 plPlasmaAudioEventInstance::EstimateVoiceEndFrame(const Voice& voice, ma_uint64 uiNow) const
{
  if (!voice.m_bActive || voice.m_bNativeLoop || voice.m_uiLengthSourceFrames == 0 || voice.m_uiSourceSampleRate == 0)
    return UnknownFrame;

  const double fRateRatio = static_cast<double>(m_uiSampleRate) / static_cast<double>(voice.m_uiSourceSampleRate);
  const float fPitchNow = voice.m_fBasePitch * m_fCurrentPitchMultiplier;
  const double fPitch = plMath::Max(static_cast<double>(fPitchNow), 0.01);

  // unchanged pitch (or not yet playing): exact frame arithmetic from the start frame; a virtual voice has no cursor
  // and uses the same arithmetic with the current pitch
  if (fPitchNow == voice.m_fStartPitch || uiNow <= voice.m_uiStartFrame || voice.m_pInner == nullptr)
  {
    return voice.m_uiStartFrame + static_cast<ma_uint64>(static_cast<double>(voice.m_uiLengthSourceFrames) * fRateRatio / fPitch + 0.5);
  }

  ma_uint64 uiCursor = 0;
  if (voice.m_pInner == nullptr || ma_data_source_get_cursor_in_pcm_frames(voice.m_pInner, &uiCursor) != MA_SUCCESS)
    return UnknownFrame;

  const ma_uint64 uiRemaining = (uiCursor < voice.m_uiLengthSourceFrames) ? (voice.m_uiLengthSourceFrames - uiCursor) : 0;
  return uiNow + static_cast<ma_uint64>(static_cast<double>(uiRemaining) * fRateRatio / fPitch + 0.5);
}

void plPlasmaAudioEventInstance::ApplyGlobalParameters()
{
  if (m_pGlobalParameters == nullptr || m_pGlobalParameters->IsEmpty() || m_pResource == nullptr)
    return;

  const auto& desc = m_pResource->GetDescriptor();

  for (plUInt32 i = 0; i < m_ParameterValues.GetCount(); ++i)
  {
    if (m_ParameterLocal[i])
      continue;

    float fValue = 0.0f;
    if (m_pGlobalParameters->TryGetValue(m_ParameterNameHashes[i], fValue))
    {
      m_ParameterValues[i] = plMath::Clamp(fValue, desc.m_Parameters[i].m_fMin, desc.m_Parameters[i].m_fMax);
    }
  }
}

plUInt32 plPlasmaAudioEventInstance::PickPlaylistEntry(const plAudioEventInstrumentDesc& instrument, InstrumentState& state, plRandom& ref_rng)
{
  const auto& playlist = instrument.m_Playlist;

  if (playlist.IsEmpty())
    return plInvalidIndex;

  if (instrument.m_ePlaylistMode == plAudioEventPlaylistMode::Sequential)
  {
    const plUInt32 uiIndex = state.m_uiCursor % playlist.GetCount();
    ++state.m_uiCursor;
    return uiIndex;
  }

  // weighted random (Shuffle re-rolls once when it hits the previous entry)
  float fTotalWeight = 0.0f;
  for (const auto& entry : playlist)
    fTotalWeight += plMath::Max(entry.m_fWeight, 0.0f);

  if (fTotalWeight <= 0.0f)
    return 0;

  for (plUInt32 uiAttempt = 0; uiAttempt < 2; ++uiAttempt)
  {
    float fPick = static_cast<float>(ref_rng.DoubleZeroToOneExclusive()) * fTotalWeight;
    plUInt32 uiPickedIndex = playlist.GetCount() - 1;

    for (plUInt32 i = 0; i < playlist.GetCount(); ++i)
    {
      fPick -= plMath::Max(playlist[i].m_fWeight, 0.0f);
      if (fPick <= 0.0f)
      {
        uiPickedIndex = i;
        break;
      }
    }

    if (instrument.m_ePlaylistMode == plAudioEventPlaylistMode::Shuffle && playlist.GetCount() > 1 && uiPickedIndex == state.m_uiLastEntry && uiAttempt == 0)
      continue; // re-roll once to avoid immediate repetition

    state.m_uiLastEntry = static_cast<plUInt16>(uiPickedIndex);
    return uiPickedIndex;
  }

  return 0;
}

plResult plPlasmaAudioEventInstance::InitClipSource(Voice& voice, const plAudioClipResourceHandle& hClip, bool& out_bNotLoaded)
{
  out_bNotLoaded = false;

  // PointerOnly never blocks the audio thread; the event resource preloads its clips
  plAudioClipResource* pClip = plResourceManager::BeginAcquireResource(hClip, plResourceAcquireMode::PointerOnly);

  const plResourceState state = pClip->GetLoadingState();
  if (state != plResourceState::Loaded)
  {
    plResourceManager::EndAcquireResource(pClip);

    if (state == plResourceState::LoadedResourceMissing)
      return PL_FAILURE;

    plResourceManager::PreloadResource(hClip);
    out_bNotLoaded = true;
    return PL_FAILURE;
  }

  // A language switch reloads the loaded clips, but not those whose load was in flight: such a clip finished loading
  // in the previous language and is reloaded before it plays. Clips with a single language sound the same in every
  // language. When the clip cannot be reloaded (created from memory) it plays as it is.
  if (pClip->GetLanguageGeneration() != plAudioClipResourceLoader::GetLanguageGeneration() && pClip->GetHeader().m_Languages.GetCount() > 1 &&
      plResourceManager::ReloadResource(hClip, true))
  {
    plResourceManager::EndAcquireResource(pClip);
    plResourceManager::PreloadResource(hClip);
    out_bNotLoaded = true;
    return PL_FAILURE;
  }

  if (voice.m_pClipSource == nullptr)
  {
    if (m_uiClipSourcesAssigned == m_ClipSources.GetCount())
    {
      m_ClipSources.PushBack(PL_DEFAULT_NEW(plAudioClipVoiceSource));
    }

    voice.m_pClipSource = m_ClipSources[m_uiClipSourcesAssigned++].Borrow();
  }

  if (pClip->CreateVoiceSource(*voice.m_pClipSource, m_pStreams).Failed())
  {
    plResourceManager::EndAcquireResource(pClip);
    return PL_FAILURE;
  }

  // the voice source reads the clip's memory: the handle keeps the resource alive and the acquisition lasts until StopVoice
  voice.m_hClip = hClip;
  voice.m_pClip = pClip;
  voice.m_bClipSourceValid = true;
  voice.m_pInner = voice.m_pClipSource->m_pSource;
  voice.m_uiSourceSampleRate = pClip->GetSampleRate();
  voice.m_uiLengthSourceFrames = pClip->GetFrames();
  return PL_SUCCESS;
}

float plPlasmaAudioEventInstance::GetOcclusionCutoff() const
{
  const float fOcclusion = (m_pEntity != nullptr) ? plMath::Clamp(m_pEntity->m_fCurrentOcclusion, 0.0f, 1.0f) : 0.0f;

  // exponential sweep from fully open (22 kHz) down to muffled (250 Hz)
  return 22000.0f * plMath::Pow(250.0f / 22000.0f, fOcclusion);
}

void plPlasmaAudioEventInstance::StartVoice(plUInt32 uiInstrument, plUInt32 uiSlot, plRandom& ref_rng, ma_uint64 uiStartFrame)
{
  const auto& desc = m_pResource->GetDescriptor();
  const auto& instrumentDesc = desc.m_Instruments[uiInstrument];
  auto& state = m_Instruments[uiInstrument];
  auto& voice = GetVoice(uiInstrument, uiSlot);

  state.m_bPendingTrigger = false;
  state.m_bGapRolled = false;

  const plUInt32 uiEntry = PickPlaylistEntry(instrumentDesc, state, ref_rng);
  const plAudioClipResourceHandle& hClip = (uiEntry != plInvalidIndex) ? m_pResource->GetClip(uiInstrument, uiEntry) : plAudioClipResourceHandle();
  const plUInt16 uiSample = (uiEntry != plInvalidIndex) ? instrumentDesc.m_Playlist[uiEntry].m_uiSample : plAudioEventSilenceSample;

  bool bSilence = !hClip.IsValid() && (uiSample == plAudioEventSilenceSample || uiSample >= m_pResource->GetNumSamples());

  if (hClip.IsValid())
  {
    // PointerOnly never blocks the audio thread; the event resource preloads its clips
    plAudioClipResource* pClip = plResourceManager::BeginAcquireResource(hClip, plResourceAcquireMode::PointerOnly);
    const plResourceState clipState = pClip->GetLoadingState();
    plResourceManager::EndAcquireResource(pClip);

    if (clipState == plResourceState::LoadedResourceMissing)
    {
      bSilence = true;

      if (!state.m_bClipMissingLogged)
      {
        state.m_bClipMissingLogged = true;
        Log(plAudioLogEventType::ClipMissing, hClip.GetResourceID());
      }
    }
    else if (clipState != plResourceState::Loaded)
    {
      if (!state.m_bLoadWaitLogged)
      {
        state.m_bLoadWaitLogged = true;
        Log(plAudioLogEventType::ClipLoadWait, hClip.GetResourceID());
      }

      // the clip is still loading: try again one look-ahead span later
      plResourceManager::PreloadResource(hClip);
      state.m_uiNextTriggerFrame = uiStartFrame + plPlasmaAudioEventManager::GetLookAheadFrames(m_pEngine);
      state.m_bPendingTrigger = true;
      return;
    }
  }

  auto FinishAsSilence = [&]()
  {
    // silence entry: for looping instruments schedule the next re-trigger, otherwise the instrument is done
    if (instrumentDesc.m_bLoop && !m_bStopping)
    {
      state.m_uiNextTriggerFrame = uiStartFrame + RollRetriggerGap(instrumentDesc, ref_rng, 0.05);
      state.m_bPendingTrigger = true;
    }
    else
    {
      state.m_bDone = true;
    }
  };

  if (bSilence)
  {
    FinishAsSilence();
    return;
  }

  voice.m_uiInstrument = static_cast<plUInt8>(uiInstrument);
  voice.m_uiEntry = uiEntry;
  voice.m_uiLastFlipFrame = UnknownFrame;

  // randomized base volume / pitch
  const float fVolDb = instrumentDesc.m_fVolumeDb + static_cast<float>(ref_rng.DoubleMinMax(-instrumentDesc.m_fVolumeRandDb, instrumentDesc.m_fVolumeRandDb));
  const float fPitchSt = instrumentDesc.m_fPitchSemitones + static_cast<float>(ref_rng.DoubleMinMax(-instrumentDesc.m_fPitchRandSemitones, instrumentDesc.m_fPitchRandSemitones));
  voice.m_fBaseVolume = plAudioMixerUtils::DbToLinear(fVolDb);
  voice.m_fBasePitch = plAudioMixerUtils::SemitonesToPitch(fPitchSt);

  ApplyModulation(); // current volume / pitch multipliers for the admission and the new voice

  // the follow-up voice of a looping instrument inherits the state of the voice it follows and is not logged
  const bool bFollowUp = GetVoice(uiInstrument, (uiSlot + 1) % VoiceSlotsPerInstrument).m_bActive;

  if (!AdmitVoice(uiInstrument, uiSlot, voice.m_fBaseVolume))
  {
    // a Kill event plays real or not at all, as when one of its voices is virtualized
    if (desc.m_eVirtualMode == plAudioEventVirtualMode::Kill)
    {
      Log(plAudioLogEventType::Killed, "not admitted at start");
      Stop(true);
      return;
    }

    if (!bFollowUp && m_pEventLog != nullptr)
    {
      plStringBuilder sDetail;
      sDetail.SetFormat("audibility {}", plArgF(ComputeAudibility(voice.m_fBaseVolume), 3));
      Log(plAudioLogEventType::CulledAtStart, sDetail);
    }

    // a virtual voice: length, sample rate and loop points come from the clip header or the probed embedded sample
    if (hClip.IsValid())
    {
      plAudioClipResource* pClip = plResourceManager::BeginAcquireResource(hClip, plResourceAcquireMode::PointerOnly);
      voice.m_uiLengthSourceFrames = pClip->GetFrames();
      voice.m_uiSourceSampleRate = pClip->GetSampleRate();
      DecideNativeLoop(voice, pClip);
      plResourceManager::EndAcquireResource(pClip);
    }
    else
    {
      plUInt64 uiFrames = 0;
      plUInt32 uiSampleRate = 0;
      m_pResource->GetSampleLength(uiSample, uiFrames, uiSampleRate);

      if (uiSampleRate == 0)
      {
        state.m_bDone = true;
        return;
      }

      voice.m_uiLengthSourceFrames = uiFrames;
      voice.m_uiSourceSampleRate = uiSampleRate;
      DecideNativeLoop(voice, nullptr);
    }

    voice.m_hClip = hClip;
    voice.m_uiStartFrame = uiStartFrame;
    voice.m_fStartPitch = voice.m_fBasePitch * m_fCurrentPitchMultiplier;
    voice.m_bVirtual = true;
    voice.m_bActive = true;
    state.m_uiCurrentSlot = static_cast<plUInt8>(uiSlot);
    state.m_bLoadWaitLogged = false;
    return;
  }

  // the voice counts as real from its admission on, so the StopVoice of a failed build below un-counts it again
  voice.m_bVirtual = false;
  voice.m_bActive = true;

  // authored fade-in from the frame the voice's data begins (applies to every voice start, including loop re-triggers)
  // the fader is a unity envelope multiplied with the sound volume, so it fades to 1, not to the voice volume
  switch (BuildVoiceChain(voice, uiStartFrame, 0, true, ToFrames(desc.m_FadeIn), 1.0f))
  {
    case BuildResult::Success:
      break;

    case BuildResult::NotLoaded:
      // the clip is still loading (or reloads for a language switch): try again one look-ahead span later
      if (!state.m_bLoadWaitLogged)
      {
        state.m_bLoadWaitLogged = true;
        Log(plAudioLogEventType::ClipLoadWait, hClip.GetResourceID());
      }

      StopVoice(voice);
      state.m_uiNextTriggerFrame = uiStartFrame + plPlasmaAudioEventManager::GetLookAheadFrames(m_pEngine);
      state.m_bPendingTrigger = true;
      return;

    case BuildResult::ClipFailed:
      // a clip that cannot play (unsupported codec, streamed without stream service) is treated as silence
      if (!state.m_bClipMissingLogged)
      {
        state.m_bClipMissingLogged = true;
        Log(plAudioLogEventType::ClipMissing, hClip.GetResourceID());
      }

      StopVoice(voice);
      FinishAsSilence();
      return;

    case BuildResult::Failed:
      StopVoice(voice);
      state.m_bDone = true;
      return;
  }

  voice.m_uiStartFrame = uiStartFrame;
  voice.m_fStartPitch = voice.m_fBasePitch * m_fCurrentPitchMultiplier;
  voice.m_bActive = true;
  state.m_uiCurrentSlot = static_cast<plUInt8>(uiSlot);
  state.m_bLoadWaitLogged = false;
}

void plPlasmaAudioEventInstance::DecideNativeLoop(Voice& voice, const plAudioClipResource* pClip)
{
  const auto& desc = m_pResource->GetDescriptor();
  const auto& instrumentDesc = desc.m_Instruments[voice.m_uiInstrument];

  // gapless looping for single-entry looping instruments without retrigger gaps
  const bool bGaplessLoop = instrumentDesc.m_bLoop && instrumentDesc.m_Playlist.GetCount() == 1 &&
                            instrumentDesc.m_MinRetriggerDelay <= plTime::MakeZero() && instrumentDesc.m_MaxRetriggerDelay <= plTime::MakeZero();

  voice.m_bNativeLoop = false;
  voice.m_bLoopPoints = false;

  // the sole instrument of a loop region loops the region window on its inner source (in source frames)
  if (voice.m_uiInstrument == m_uiRegionLoopInstrument && IsRegionLooping() && voice.m_uiSourceSampleRate > 0)
  {
    const ma_uint64 uiRegionSourceFrames = static_cast<ma_uint64>((desc.m_LoopEnd - desc.m_LoopStart).GetSeconds() * voice.m_uiSourceSampleRate + 0.5);

    if (uiRegionSourceFrames > 0 && voice.m_uiLengthSourceFrames >= uiRegionSourceFrames)
    {
      voice.m_bNativeLoop = true;
      voice.m_bLoopPoints = true;
      voice.m_uiLoopStart = 0;
      voice.m_uiLoopEnd = uiRegionSourceFrames;
    }
    else
    {
      // sample shorter than the region: the region is re-armed by frame arithmetic in Update
      m_uiRegionLoopInstrument = plInvalidIndex;
    }
  }

  if (!voice.m_bNativeLoop && bGaplessLoop)
  {
    // a clip with authored loop points plays its intro once and then repeats [loop start, loop end)
    plUInt64 uiLoopStart = 0;
    plUInt64 uiLoopEnd = 0;
    if (pClip != nullptr && pClip->GetLoopPoints(uiLoopStart, uiLoopEnd) && uiLoopEnd > uiLoopStart)
    {
      voice.m_bLoopPoints = true;
      voice.m_uiLoopStart = uiLoopStart;
      voice.m_uiLoopEnd = uiLoopEnd;
    }

    voice.m_bNativeLoop = true;
  }
}

plPlasmaAudioEventInstance::BuildResult plPlasmaAudioEventInstance::BuildVoiceChain(Voice& voice, ma_uint64 uiDataStartFrame, ma_uint64 uiSourceCursor, bool bFreshStart, ma_uint64 uiStartFadeFrames, float fStartFadeVolume)
{
  const auto& desc = m_pResource->GetDescriptor();
  const auto& instrumentDesc = desc.m_Instruments[voice.m_uiInstrument];
  const plAudioClipResourceHandle& hClip = m_pResource->GetClip(voice.m_uiInstrument, voice.m_uiEntry);

  if (hClip.IsValid())
  {
    bool bNotLoaded = false;
    if (InitClipSource(voice, hClip, bNotLoaded).Failed())
      return bNotLoaded ? BuildResult::NotLoaded : BuildResult::ClipFailed;
  }
  else
  {
    const plDataBuffer& sampleData = m_pResource->GetSampleData(instrumentDesc.m_Playlist[voice.m_uiEntry].m_uiSample);

    if (ma_decoder_init_memory(sampleData.GetData(), sampleData.GetCount(), nullptr, &voice.m_Decoder) != MA_SUCCESS)
      return BuildResult::Failed;

    voice.m_bDecoderValid = true;
    voice.m_pInner = &voice.m_Decoder;

    voice.m_uiSourceSampleRate = voice.m_Decoder.outputSampleRate;
    if (ma_decoder_get_length_in_pcm_frames(&voice.m_Decoder, &voice.m_uiLengthSourceFrames) != MA_SUCCESS)
    {
      voice.m_uiLengthSourceFrames = 0;
    }
  }

  if (bFreshStart)
  {
    DecideNativeLoop(voice, voice.m_bClipSourceValid ? voice.m_pClip : nullptr);
  }

  if (voice.m_bNativeLoop)
  {
    if (voice.m_bLoopPoints)
    {
      ma_data_source_set_loop_point_in_pcm_frames(voice.m_pInner, voice.m_uiLoopStart, voice.m_uiLoopEnd);
    }

    ma_data_source_set_looping(voice.m_pInner, true);
  }

  // a realized voice continues where its timeline is; the data source's lead silence then ends exactly at uiDataStartFrame
  if (uiSourceCursor > 0 && ma_data_source_seek_to_pcm_frame(voice.m_pInner, uiSourceCursor) != MA_SUCCESS)
  {
    ReleaseVoiceChain(voice);
    return BuildResult::Failed;
  }

  if (InitVoiceDataSource(voice.m_pInner, m_pEngine, uiDataStartFrame, &voice.m_Source) != MA_SUCCESS)
  {
    ReleaseVoiceChain(voice);
    return BuildResult::Failed;
  }
  voice.m_bSourceValid = true;

  if (ma_sound_init_from_data_source(m_pEngine, &voice.m_Source, 0, nullptr, &voice.m_Sound) != MA_SUCCESS)
  {
    ReleaseVoiceChain(voice);
    return BuildResult::Failed;
  }
  voice.m_bSoundValid = true;

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
  // The nodes live inside the voice and allocate their heaps through the engine's allocation callbacks (the
  // "PlasmaAudio/miniaudio" allocator for the backend's engine); the same callbacks uninitialize them.
  ma_node* pCurrent = reinterpret_cast<ma_node*>(&voice.m_Sound);

  const ma_lpf_node_config lpfCfg = ma_lpf_node_config_init(ma_engine_get_channels(m_pEngine), ma_engine_get_sample_rate(m_pEngine), GetOcclusionCutoff(), 2);
  if (ma_lpf_node_init(ma_engine_get_node_graph(m_pEngine), &lpfCfg, &m_pEngine->allocationCallbacks, &voice.m_OcclusionLpf) == MA_SUCCESS)
  {
    voice.m_bOcclusionLpfValid = true;
    ma_node_attach_output_bus(pCurrent, 0, reinterpret_cast<ma_node*>(&voice.m_OcclusionLpf), 0);
    pCurrent = reinterpret_cast<ma_node*>(&voice.m_OcclusionLpf);
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
    ma_splitter_node_config splitCfg = ma_splitter_node_config_init(ma_engine_get_channels(m_pEngine));
    splitCfg.outputBusCount = 2;

    if (ma_splitter_node_init(ma_engine_get_node_graph(m_pEngine), &splitCfg, &m_pEngine->allocationCallbacks, &voice.m_EnvSplitter) == MA_SUCCESS)
    {
      voice.m_bEnvSplitterValid = true;
      ma_node* pSplitter = reinterpret_cast<ma_node*>(&voice.m_EnvSplitter);
      ma_node_attach_output_bus(pCurrent, 0, pSplitter, 0);
      ma_node_attach_output_bus(pSplitter, 0, pTarget, 0); // dry
      ma_node_set_output_bus_volume(pSplitter, 1, 0.0f);   // wet starts silent; Update() routes and levels it
      pCurrent = nullptr;                                  // chain fully wired through splitter
    }
  }

  if (pCurrent != nullptr)
  {
    ma_node_attach_output_bus(pCurrent, 0, pTarget, 0);
  }

  const float fPitchNow = voice.m_fBasePitch * m_fCurrentPitchMultiplier;
  ma_sound_set_volume(&voice.m_Sound, voice.m_fBaseVolume * m_fCurrentVolumeMultiplier);
  ma_sound_set_pitch(&voice.m_Sound, fPitchNow);

  // sample-accurate start: the sound runs from the next mixed chunk on and its data source emits silence up to
  // uiDataStartFrame (a frame at or before the engine time starts with the next mixed chunk)
  voice.m_Source.m_fSourceFramesPerEngineFrame = static_cast<double>(plMath::Max(fPitchNow, 0.01f)) * static_cast<double>(voice.m_uiSourceSampleRate) / static_cast<double>(m_uiSampleRate);

  // the start fade is set up before the sound starts; the source's first read applies it (see plMaVoiceDataSource)
  voice.m_Source.m_pStartFader = &voice.m_Sound.engineNode.fader;
  voice.m_Source.m_uiStartFadeFrames = uiStartFadeFrames;
  voice.m_Source.m_fStartFadeVolume = fStartFadeVolume;

  if (ma_sound_start(&voice.m_Sound) != MA_SUCCESS)
  {
    ReleaseVoiceChain(voice);
    return BuildResult::Failed;
  }

  return BuildResult::Success;
}

bool plPlasmaAudioEventInstance::AdmitVoice(plUInt32 uiInstrument, plUInt32 uiSlot, float fBaseVolume)
{
  // the follow-up voice of an instrument takes over the state of the voice it follows
  const Voice& other = GetVoice(uiInstrument, (uiSlot + 1) % VoiceSlotsPerInstrument);
  if (other.m_bActive)
  {
    if (!other.m_bVirtual && m_pVoiceManager != nullptr)
    {
      m_pVoiceManager->NoteRealVoiceStarted();
    }

    return !other.m_bVirtual;
  }

  if (m_pVoiceManager == nullptr)
    return true;

  return m_pVoiceManager->AdmitNewVoice(GetPriority(), ComputeAudibility(fBaseVolume));
}

float plPlasmaAudioEventInstance::ComputeAudibility(float fBaseVolume) const
{
  const auto& desc = m_pResource->GetDescriptor();

  float fAttenuation = 1.0f;

  if (desc.m_bSpatialize && m_pEntity != nullptr && !m_pEntity->m_bGlobalEntity)
  {
    const float fDistance = (m_pEntity->m_LastTransform.m_vPosition - m_vListenerPos).GetLength();

    // beyond the max distance the voice counts as inaudible, so it is virtual whatever the budget
    if (fDistance > desc.m_fMaxDistance)
      return 0.0f;

    // miniaudio's default inverse distance model
    const float fMin = plMath::Max(desc.m_fMinDistance, 0.0001f);
    const float fMax = plMath::Max(desc.m_fMaxDistance, fMin);
    const float fClamped = plMath::Clamp(fDistance, fMin, fMax);
    fAttenuation = fMin / (fMin + desc.m_fRolloff * (fClamped - fMin));
  }

  const float fOcclusion = (m_pEntity != nullptr) ? plMath::Clamp(m_pEntity->m_fCurrentOcclusion, 0.0f, 1.0f) : 0.0f;
  const float fBusGain = (m_pMixer != nullptr) ? m_pMixer->GetEffectiveBusGain(m_uiBus) : 1.0f;

  return fAttenuation * m_fCurrentVolumeMultiplier * fBaseVolume * fBusGain * (1.0f - 0.5f * fOcclusion);
}

bool plPlasmaAudioEventInstance::IsVoiceLooping(plUInt32 uiVoice) const
{
  if (m_pResource == nullptr)
    return false;

  return m_pResource->GetDescriptor().m_Instruments[m_Voices[uiVoice].m_uiInstrument].m_bLoop;
}

bool plPlasmaAudioEventInstance::CanVoiceFlip(plUInt32 uiVoice, ma_uint64 uiNow, ma_uint64 uiDwellFrames) const
{
  const ma_uint64 uiLastFlip = m_Voices[uiVoice].m_uiLastFlipFrame;
  return uiLastFlip == UnknownFrame || uiNow >= uiLastFlip + uiDwellFrames;
}

plUInt8 plPlasmaAudioEventInstance::GetPriority() const
{
  return (m_pResource != nullptr) ? m_pResource->GetDescriptor().m_uiPriority : 0;
}

void plPlasmaAudioEventInstance::VirtualizeVoice(plUInt32 uiVoice)
{
  Voice& voice = m_Voices[uiVoice];
  if (!voice.m_bActive || voice.m_bVirtual || m_bStopping || m_pResource == nullptr)
    return;

  if (m_pResource->GetDescriptor().m_eVirtualMode == plAudioEventVirtualMode::Kill)
  {
    Log(plAudioLogEventType::Killed, "virtualised");
    Stop(true);
    return;
  }

  const ma_uint64 uiNow = ma_engine_get_time_in_pcm_frames(m_pEngine);
  const ma_uint64 uiFadeFrames = ToFrames(VirtualFadeDuration);

  voice.m_bVirtual = true;
  voice.m_uiLastFlipFrame = uiNow;

  if (m_pEventLog != nullptr)
  {
    plStringBuilder sDetail;
    sDetail.SetFormat("audibility {}", plArgF(m_fAudibility, 3));
    Log(plAudioLogEventType::Virtualised, sDetail);
  }

  if (m_pVoiceManager != nullptr)
  {
    m_pVoiceManager->NoteRealVoiceEnded();
  }

  // A voice that has not produced audio yet is cut. Otherwise it fades to silence from its current volume and keeps
  // running silently until the first Update after the fade end tears the chain down.
  //
  // The fade has no absolute start frame: the sound picks it up at the start of the next chunk it is mixed in. The
  // device thread may already have mixed the sound for the chunk that begins at uiNow (the engine time advances only
  // once a chunk is mixed), and a fade anchored at uiNow would then start one chunk in the past and end right away.
  // The fade therefore ends within one look-ahead span (at least two device periods) after uiNow plus its length. No
  // stop time is set: miniaudio stops a sound for the whole processing chunk that reaches its stop time, which would
  // cut the fade.
  if (voice.m_uiStartFrame > uiNow || !voice.m_bSoundValid || uiFadeFrames == 0)
  {
    ReleaseVoiceChain(voice);
    return;
  }

  ma_sound_set_fade_in_pcm_frames(&voice.m_Sound, -1.0f, 0.0f, uiFadeFrames);
  voice.m_uiTeardownFrame = uiNow + plPlasmaAudioEventManager::GetLookAheadFrames(m_pEngine) + uiFadeFrames;
  voice.m_bTeardownPending = true;
}

bool plPlasmaAudioEventInstance::RealizeVoice(plUInt32 uiVoice)
{
  Voice& voice = m_Voices[uiVoice];
  if (!voice.m_bActive || !voice.m_bVirtual || m_bStopping || m_pResource == nullptr)
    return false;

  const auto& desc = m_pResource->GetDescriptor();
  const ma_uint64 uiNow = ma_engine_get_time_in_pcm_frames(m_pEngine);

  // a voice that has not started yet keeps its start frame; otherwise its data resumes one look-ahead span ahead,
  // which the engine has not mixed yet
  const ma_uint64 uiResume = (voice.m_uiStartFrame >= uiNow) ? voice.m_uiStartFrame : uiNow + plPlasmaAudioEventManager::GetLookAheadFrames(m_pEngine);
  const bool bRestart = desc.m_eVirtualMode == plAudioEventVirtualMode::Restart;

  ma_uint64 uiCursor = 0;

  if (!bRestart && uiResume > voice.m_uiStartFrame && voice.m_uiSourceSampleRate > 0)
  {
    const float fPitchNow = voice.m_fBasePitch * m_fCurrentPitchMultiplier;
    const double fSourceFramesPerEngineFrame = static_cast<double>(plMath::Max(fPitchNow, 0.01f)) * static_cast<double>(voice.m_uiSourceSampleRate) / static_cast<double>(m_uiSampleRate);
    uiCursor = static_cast<ma_uint64>(static_cast<double>(uiResume - voice.m_uiStartFrame) * fSourceFramesPerEngineFrame + 0.5);

    if (voice.m_bNativeLoop)
    {
      if (voice.m_bLoopPoints && voice.m_uiLoopEnd > voice.m_uiLoopStart)
      {
        if (uiCursor >= voice.m_uiLoopEnd)
        {
          uiCursor = voice.m_uiLoopStart + (uiCursor - voice.m_uiLoopStart) % (voice.m_uiLoopEnd - voice.m_uiLoopStart);
        }
      }
      else if (voice.m_uiLengthSourceFrames > 0)
      {
        uiCursor %= voice.m_uiLengthSourceFrames;
      }
    }
    else if (voice.m_uiLengthSourceFrames > 0 && uiCursor >= voice.m_uiLengthSourceFrames)
    {
      // the voice runs out before it could be heard; Update ends it
      return false;
    }
  }

  if (!HasStreamFor(voice))
  {
    // Every stream is taken, possibly by voices that still play their virtualization fade-out: the voice stays virtual
    // and the voice manager tries again in its next Update. A fading voice releases its stream at its teardown frame
    // whatever the ranking does, so a voice waiting for such a stream is realized within a few updates.
    return false;
  }

  // the chain of a voice that is still fading out after its virtualization is replaced
  if (voice.m_bTeardownPending)
  {
    ReleaseVoiceChain(voice);
  }

  // A restarted voice fades in like a new voice; a voice resumed in the middle of its data fades in from the resume
  // frame, where the data source's lead silence ends. Both fades start where the data begins (see plMaVoiceDataSource).
  ma_uint64 uiFadeFrames = 0;
  float fFadeVolume = 1.0f;
  if (bRestart)
  {
    uiFadeFrames = ToFrames(desc.m_FadeIn);
  }
  else if (uiResume > voice.m_uiStartFrame)
  {
    uiFadeFrames = ToFrames(VirtualFadeDuration);
  }

  if (BuildVoiceChain(voice, uiResume, uiCursor, false, uiFadeFrames, fFadeVolume) != BuildResult::Success)
    return false;

  if (bRestart)
  {
    voice.m_uiStartFrame = uiResume;
    voice.m_fStartPitch = voice.m_fBasePitch * m_fCurrentPitchMultiplier;
  }

  voice.m_bVirtual = false;
  voice.m_uiLastFlipFrame = uiNow;

  if (m_pEventLog != nullptr)
  {
    plStringBuilder sDetail;
    sDetail.SetFormat("audibility {}", plArgF(m_fAudibility, 3));
    Log(plAudioLogEventType::Realised, sDetail);
  }

  return true;
}

bool plPlasmaAudioEventInstance::HasStreamFor(const Voice& voice) const
{
  if (!voice.m_hClip.IsValid() || m_pStreams == nullptr || !m_pStreams->IsRunning())
    return true;

  // a voice still fading out hands its own stream to the rebuilt chain
  if (voice.m_bClipSourceValid && voice.m_pClipSource->m_pStream != nullptr)
    return true;

  plAudioClipResource* pClip = plResourceManager::BeginAcquireResource(voice.m_hClip, plResourceAcquireMode::PointerOnly);
  const bool bStreamed = pClip->GetLoadingState() == plResourceState::Loaded && pClip->GetLoadMode() == plAudioClipLoadMode::Streamed;
  plResourceManager::EndAcquireResource(pClip);

  return !bStreamed || m_pStreams->GetActiveStreamCount() < m_pStreams->GetMaxStreams();
}

void plPlasmaAudioEventInstance::KillVirtualVoice(plUInt32 uiVoice)
{
  Voice& voice = m_Voices[uiVoice];
  if (!voice.m_bActive || !voice.m_bVirtual)
    return;

  Log(plAudioLogEventType::Killed, "virtual voice budget");

  auto& state = m_Instruments[voice.m_uiInstrument];
  StopVoice(voice);
  state.m_bPendingTrigger = false;
  state.m_bDone = true;
}

void plPlasmaAudioEventInstance::StopVoice(Voice& voice)
{
  if (voice.m_bActive && !voice.m_bVirtual && m_pVoiceManager != nullptr)
  {
    m_pVoiceManager->NoteRealVoiceEnded();
  }

  ReleaseVoiceChain(voice);

  voice.m_bActive = false;
  voice.m_bVirtual = false;
  voice.m_hClip.Invalidate();
  voice.m_bNativeLoop = false;
  voice.m_bLoopPoints = false;
  voice.m_uiLengthSourceFrames = 0;
}

void plPlasmaAudioEventInstance::ReleaseVoiceChain(Voice& voice)
{
  // a sound that was initialized but failed to start is uninitialized as well
  if (voice.m_bSoundValid)
  {
    if (voice.m_bActive)
    {
      ma_sound_stop(&voice.m_Sound);
    }
    ma_sound_uninit(&voice.m_Sound);
    voice.m_bSoundValid = false;
  }

  if (voice.m_bEnvSplitterValid)
  {
    ma_splitter_node_uninit(&voice.m_EnvSplitter, &m_pEngine->allocationCallbacks);
    voice.m_bEnvSplitterValid = false;
  }

  if (voice.m_bOcclusionLpfValid)
  {
    ma_lpf_node_uninit(&voice.m_OcclusionLpf, &m_pEngine->allocationCallbacks);
    voice.m_bOcclusionLpfValid = false;
  }

  voice.m_bTeardownPending = false;

  if (voice.m_bSourceValid)
  {
    ma_data_source_uninit(&voice.m_Source);
    voice.m_bSourceValid = false;
  }

  if (voice.m_bDecoderValid)
  {
    ma_decoder_uninit(&voice.m_Decoder);
    voice.m_bDecoderValid = false;
  }

  if (voice.m_bClipSourceValid)
  {
    voice.m_pClipSource->Uninit();
    plResourceManager::EndAcquireResource(voice.m_pClip);
    voice.m_pClip = nullptr;
    voice.m_bClipSourceValid = false;
  }

  voice.m_pInner = nullptr;
}

void plPlasmaAudioEventInstance::OnClipUnloading(const plAudioClipResource* pClip)
{
  const ma_uint64 uiNow = (m_pEngine != nullptr) ? ma_engine_get_time_in_pcm_frames(m_pEngine) : 0;

  for (plUInt32 i = 0; i < m_Instruments.GetCount(); ++i)
  {
    auto& state = m_Instruments[i];
    bool bCurrentStopped = false;

    for (plUInt32 uiSlot = 0; uiSlot < VoiceSlotsPerInstrument; ++uiSlot)
    {
      Voice& voice = GetVoice(i, uiSlot);

      // a virtual voice holds only the clip handle
      const bool bReadsClip = (voice.m_bClipSourceValid && voice.m_pClip == pClip) || (voice.m_bActive && voice.m_bVirtual && voice.m_hClip == pClip);
      if (!bReadsClip)
        continue;

      bCurrentStopped |= voice.m_bActive && uiSlot == state.m_uiCurrentSlot;
      StopVoice(voice);
    }

    if (bCurrentStopped && !m_bStopping && !state.m_bPendingTrigger && m_pResource != nullptr && m_pResource->GetDescriptor().m_Instruments[i].m_bLoop)
    {
      state.m_uiNextTriggerFrame = uiNow;
      state.m_bPendingTrigger = true;
      state.m_bGapRolled = false;
    }
  }
}

void plPlasmaAudioEventInstance::OnEventUnloading()
{
  for (auto& voice : m_Voices)
  {
    StopVoice(voice);
  }

  for (auto& state : m_Instruments)
  {
    state.m_bPendingTrigger = false;
    state.m_bDone = true;
  }

  if (m_pResource != nullptr)
  {
    plResourceManager::EndAcquireResource(const_cast<plAudioEventResource*>(m_pResource));
    m_pResource = nullptr;
  }
}

void plPlasmaAudioEventInstance::Stop(bool bImmediate)
{
  if (m_bStopping && !bImmediate)
    return;

  m_bStopping = true;

  const plTime fadeOut = (m_pResource != nullptr) ? m_pResource->GetDescriptor().m_StopFadeOut : plTime::MakeZero();
  const ma_uint64 uiNow = (m_pEngine != nullptr) ? ma_engine_get_time_in_pcm_frames(m_pEngine) : 0;

  for (auto& voice : m_Voices)
  {
    if (!voice.m_bActive)
      continue;

    // voices scheduled inside the look-ahead window that have not produced audio yet are dropped, as are virtual voices
    if (bImmediate || fadeOut <= plTime::MakeZero() || voice.m_uiStartFrame > uiNow || voice.m_bVirtual)
    {
      StopVoice(voice);
    }
    else
    {
      ma_data_source_set_looping(voice.m_pInner, false);
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

void plPlasmaAudioEventInstance::SetParameter(plStringView sName, float fValue)
{
  if (m_pResource == nullptr)
    return;

  const auto& desc = m_pResource->GetDescriptor();
  const plUInt32 uiParam = desc.FindParameterByName(sName);

  if (uiParam == plInvalidIndex || uiParam >= m_ParameterValues.GetCount())
    return;

  m_ParameterValues[uiParam] = plMath::Clamp(fValue, desc.m_Parameters[uiParam].m_fMin, desc.m_Parameters[uiParam].m_fMax);
  m_ParameterLocal[uiParam] = true;
}

void plPlasmaAudioEventInstance::ApplyModulation()
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

  if (m_pEntity != nullptr)
  {
    // obstruction scales the direct signal down
    fVolume *= 1.0f - 0.85f * plMath::Clamp(m_pEntity->m_fCurrentObstruction, 0.0f, 1.0f);

    // per-entity multipliers (legacy component volume / pitch)
    fVolume *= plMath::Max(m_pEntity->m_fVolumeMultiplier, 0.0f);
    fPitch *= plMath::Clamp(m_pEntity->m_fPitchMultiplier, 0.01f, 10.0f);
  }

  m_fCurrentVolumeMultiplier = fVolume;
  m_fCurrentPitchMultiplier = fPitch;
}

plUInt32 plPlasmaAudioEventInstance::GetActiveVoiceCount() const
{
  plUInt32 uiCount = 0;
  for (const auto& voice : m_Voices)
  {
    if (voice.m_bActive)
      ++uiCount;
  }
  return uiCount;
}

plUInt32 plPlasmaAudioEventInstance::GetRealVoiceCount() const
{
  plUInt32 uiCount = 0;
  for (const auto& voice : m_Voices)
  {
    if (voice.m_bActive && !voice.m_bVirtual)
      ++uiCount;
  }
  return uiCount;
}

plUInt32 plPlasmaAudioEventInstance::GetVirtualVoiceCount() const
{
  plUInt32 uiCount = 0;
  for (const auto& voice : m_Voices)
  {
    if (voice.m_bActive && voice.m_bVirtual)
      ++uiCount;
  }
  return uiCount;
}

plStringView plPlasmaAudioEventInstance::GetDisplayName() const
{
  if (m_pResource == nullptr)
    return "<unloaded>";

  return !m_pResource->GetResourceDescription().IsEmpty() ? m_pResource->GetResourceDescription().GetView() : m_pResource->GetResourceID().GetView();
}

void plPlasmaAudioEventInstance::Log(plAudioLogEventType::Enum type, plStringView sDetail) const
{
  if (m_pEventLog != nullptr)
  {
    m_pEventLog->Add(type, GetDisplayName(), sDetail);
  }
}

void plPlasmaAudioEventInstance::AppendSnapshotVoices(plDynamicArray<plAudioVoiceSnapshotEntry>& inout_voices) const
{
  const plStringView sEvent = GetDisplayName();
  plStringView sBus = "Master";
  plUInt8 uiPriority = 0;

  if (m_pResource != nullptr)
  {
    uiPriority = m_pResource->GetDescriptor().m_uiPriority;

    if (m_pBusGraph != nullptr && m_pBusGraph->IsBuilt() && m_uiBus < m_pBusGraph->GetDesc().m_Buses.GetCount())
    {
      sBus = m_pBusGraph->GetDesc().m_Buses[m_uiBus].m_sName;
    }
    else if (!m_pResource->GetDescriptor().m_sOutputBus.IsEmpty())
    {
      sBus = m_pResource->GetDescriptor().m_sOutputBus;
    }
  }

  const float fOcclusion = (m_pEntity != nullptr) ? plMath::Clamp(m_pEntity->m_fCurrentOcclusion, 0.0f, 1.0f) : 0.0f;

  for (plUInt32 uiVoice = 0; uiVoice < m_Voices.GetCount(); ++uiVoice)
  {
    const Voice& voice = m_Voices[uiVoice];
    if (!voice.m_bActive)
      continue;

    plAudioVoiceSnapshotEntry& entry = inout_voices.ExpandAndGetRef();
    entry.m_uiId = static_cast<plUInt32>((m_uiSerial << 8) | (uiVoice & 0xFFu));
    entry.m_sEvent = sEvent;
    entry.m_uiEntityId = (m_pEntity != nullptr) ? static_cast<plUInt64>(m_pEntity->m_uiEntityId) : 0;
    entry.m_uiState = m_bStopping ? plAudioVoiceSnapshotEntry::Stopping : (voice.m_bVirtual ? plAudioVoiceSnapshotEntry::Virtual : plAudioVoiceSnapshotEntry::Real);
    entry.m_uiPriority = uiPriority;
    entry.m_uiCodec = plAudioVoiceSnapshotEntry::Embedded;
    entry.m_uiLoadMode = plAudioVoiceSnapshotEntry::Embedded;
    entry.m_fAudibility = m_fAudibility;
    entry.m_fDistance = m_fDistance;
    entry.m_fOcclusion = fOcclusion;
    entry.m_fVolume = m_fCurrentVolumeMultiplier * voice.m_fBaseVolume;
    entry.m_fAge = static_cast<float>(m_Elapsed.GetSeconds());
    entry.m_sBus = sBus;

    // a virtual voice released its clip acquisition but keeps the handle; PointerOnly never blocks
    if (voice.m_pClip != nullptr)
    {
      entry.m_uiCodec = voice.m_pClip->GetCodec();
      entry.m_uiLoadMode = voice.m_pClip->GetLoadMode();
    }
    else if (voice.m_hClip.IsValid())
    {
      plAudioClipResource* pClip = plResourceManager::BeginAcquireResource(voice.m_hClip, plResourceAcquireMode::PointerOnly);
      if (pClip->GetLoadingState() == plResourceState::Loaded)
      {
        entry.m_uiCodec = pClip->GetCodec();
        entry.m_uiLoadMode = pClip->GetLoadMode();
      }
      plResourceManager::EndAcquireResource(pClip);
    }
  }
}

bool plPlasmaAudioEventInstance::Update(plTime delta, const plVec3& vListenerPos, plRandom& ref_rng)
{
  if (m_pResource == nullptr)
    return false;

  m_Elapsed += delta;
  m_vListenerPos = vListenerPos;

  const auto& desc = m_pResource->GetDescriptor();

  const ma_uint64 uiNow = ma_engine_get_time_in_pcm_frames(m_pEngine);
  const ma_uint64 uiHorizon = uiNow + plPlasmaAudioEventManager::GetLookAheadFrames(m_pEngine);

  // smooth occlusion state on the entity (shared by all instances on it; cheap to repeat)
  if (m_pEntity != nullptr)
  {
    const float fSmooth = plMath::Min(1.0f, static_cast<float>(delta.GetSeconds()) * 8.0f);
    m_pEntity->m_fCurrentObstruction = plMath::Lerp(m_pEntity->m_fCurrentObstruction, m_pEntity->m_fTargetObstruction, fSmooth);
    m_pEntity->m_fCurrentOcclusion = plMath::Lerp(m_pEntity->m_fCurrentOcclusion, m_pEntity->m_fTargetOcclusion, fSmooth);
  }

  // parameter precedence: global parameters, then instance values from SetParameter, then the built-in Distance
  ApplyGlobalParameters();

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

  // the instance lives while anything plays or is scheduled; looping events with m_bLooping
  // event data are kept alive until an explicit stop even if momentarily silent
  const bool bHoldOpen = !m_bStopping && m_pEventData != nullptr && m_pEventData->m_bLooping;

  for (plUInt32 i = 0; i < m_Instruments.GetCount(); ++i)
  {
    auto& state = m_Instruments[i];
    const auto& instrumentDesc = desc.m_Instruments[i];

    for (plUInt32 uiSlot = 0; uiSlot < VoiceSlotsPerInstrument; ++uiSlot)
    {
      auto& voice = GetVoice(i, uiSlot);

      if (!voice.m_bActive)
        continue;

      bool bEnded = false;
      bool bFadedOut = false;
      bool bStopped = false;

      if (voice.m_bVirtual)
      {
        // The fade-out of a virtualized voice has ended and the voice was mixed silently since, so the chain can go. The
        // bus the voice feeds holds back the last frame it received until it gets input again; that frame is silent then.
        if (voice.m_bTeardownPending && uiNow > voice.m_uiTeardownFrame)
        {
          ReleaseVoiceChain(voice);
        }

        // a virtual voice runs out at its estimated end frame
        const ma_uint64 uiVirtualEnd = EstimateVoiceEndFrame(voice, uiNow);
        bEnded = uiVirtualEnd != UnknownFrame && uiVirtualEnd <= uiNow;
        bStopped = m_bStopping;
      }
      else
      {
        bEnded = ma_sound_at_end(&voice.m_Sound) != 0;
        bFadedOut = m_bStopping && ma_sound_get_current_fade_volume(&voice.m_Sound) <= 0.0f;
        bStopped = m_bStopping && ma_sound_is_playing(&voice.m_Sound) == 0;
      }

      if (bEnded || bFadedOut || bStopped)
      {
        const bool bWasCurrent = uiSlot == state.m_uiCurrentSlot;

        // the frame the voice ran out of data; a voice that ended earlier than estimated ended by now at the latest
        const ma_uint64 uiEstimatedEnd = EstimateVoiceEndFrame(voice, uiNow);
        const ma_uint64 uiEndFrame = (uiEstimatedEnd != UnknownFrame) ? plMath::Min(uiEstimatedEnd, uiNow) : uiNow;

        StopVoice(voice);

        if (!bWasCurrent || state.m_bPendingTrigger)
        {
          // a follow-up voice is already scheduled
        }
        else if (!m_bStopping && instrumentDesc.m_bLoop)
        {
          // the follow-up was not scheduled ahead (unknown length, a retrigger gap longer than the look-ahead, or the
          // voice ended earlier than estimated): the gap counts from the voice's end frame
          if (!state.m_bGapRolled)
          {
            state.m_uiRetriggerGapFrames = RollRetriggerGap(instrumentDesc, ref_rng, 0.0);
            state.m_bGapRolled = true;
          }

          state.m_uiNextTriggerFrame = uiEndFrame + state.m_uiRetriggerGapFrames;
          state.m_bPendingTrigger = true;
        }
        else
        {
          state.m_bDone = true;
        }
      }
      else if (voice.m_bVirtual)
      {
        bAnyActive = true;
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
        if (voice.m_bOcclusionLpfValid)
        {
          const ma_lpf_config lpfCfg = ma_lpf_config_init(ma_format_f32, ma_engine_get_channels(m_pEngine), ma_engine_get_sample_rate(m_pEngine), fOcclusionCutoff, 2);
          ma_lpf_node_reinit(&lpfCfg, &voice.m_OcclusionLpf);
        }

        // environment send
        if (voice.m_bEnvSplitterValid && m_pBusGraph != nullptr)
        {
          ma_node* pSplitter = reinterpret_cast<ma_node*>(&voice.m_EnvSplitter);

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

    // looping instruments schedule their follow-up voice before the current one ends: once
    // (estimated end + gap) enters the look-ahead window it is started on the other voice slot
    // with that exact frame, so gapless and gapped re-triggers are sample-accurate
    if (!m_bStopping && instrumentDesc.m_bLoop && !state.m_bPendingTrigger)
    {
      const Voice& current = GetVoice(i, state.m_uiCurrentSlot);

      if (current.m_bActive && !current.m_bNativeLoop)
      {
        if (!state.m_bGapRolled)
        {
          state.m_uiRetriggerGapFrames = RollRetriggerGap(instrumentDesc, ref_rng, 0.0);
          state.m_bGapRolled = true;
        }

        const ma_uint64 uiEnd = EstimateVoiceEndFrame(current, uiNow);
        if (uiEnd != UnknownFrame && uiEnd + state.m_uiRetriggerGapFrames <= uiHorizon)
        {
          state.m_uiNextTriggerFrame = uiEnd + state.m_uiRetriggerGapFrames;
          state.m_bPendingTrigger = true;
        }
      }
    }

    if (StartPendingTrigger(i, uiHorizon, ref_rng))
      bAnyActive = true;

    if (state.m_bPendingTrigger && !m_bStopping)
      bAnyPending = true;
  }

  // authored loop region: a looping event repeats its [start, end] window instead of the whole timeline.
  // Each pass moves the timeline origin by the region length once the region end enters the look-ahead
  // window, and instruments inside the window are re-armed at origin + StartDelay, i.e. at
  // region end + (StartDelay - LoopStart) of the previous pass. The native region loop instrument keeps
  // looping on its inner source and is skipped here (its voice never reports an end frame).
  if (IsRegionLooping())
  {
    const ma_uint64 uiRegionStart = ToFrames(desc.m_LoopStart);
    const ma_uint64 uiRegionEnd = ToFrames(desc.m_LoopEnd);

    if (uiRegionEnd > uiRegionStart && m_uiTimelineOriginFrame + uiRegionEnd <= uiHorizon)
    {
      const ma_uint64 uiRegionLength = uiRegionEnd - uiRegionStart;

      // advance by whole passes; passes missed during a stall are skipped rather than replayed
      const ma_uint64 uiPasses = (uiHorizon - (m_uiTimelineOriginFrame + uiRegionEnd)) / uiRegionLength + 1;
      m_uiTimelineOriginFrame += uiPasses * uiRegionLength;

      for (plUInt32 i = 0; i < m_Instruments.GetCount(); ++i)
      {
        const auto& instrumentDesc = desc.m_Instruments[i];
        auto& state = m_Instruments[i];

        if (instrumentDesc.m_bMute || instrumentDesc.m_Playlist.IsEmpty() || state.m_bPendingTrigger)
          continue;

        if (instrumentDesc.m_StartDelay < desc.m_LoopStart || instrumentDesc.m_StartDelay >= desc.m_LoopEnd)
          continue;

        if (m_pEntity != nullptr && !m_pEntity->MatchesSwitchFilter(instrumentDesc.m_sSwitchState))
          continue;

        const ma_uint64 uiTrigger = m_uiTimelineOriginFrame + ToFrames(instrumentDesc.m_StartDelay);

        // an instrument still sounding at its re-arm frame (or sustaining itself as a looping instrument) skips this pass
        bool bBusy = false;
        for (plUInt32 uiSlot = 0; uiSlot < VoiceSlotsPerInstrument; ++uiSlot)
        {
          const Voice& voice = GetVoice(i, uiSlot);
          if (voice.m_bActive && (instrumentDesc.m_bLoop || EstimateVoiceEndFrame(voice, uiNow) > uiTrigger))
            bBusy = true;
        }

        if (bBusy)
          continue;

        state.m_uiNextTriggerFrame = uiTrigger;
        state.m_bPendingTrigger = true;
        state.m_bDone = false;

        if (StartPendingTrigger(i, uiHorizon, ref_rng))
          bAnyActive = true;

        if (state.m_bPendingTrigger)
          bAnyPending = true;
      }
    }
  }

  // audibility for the voice ranking, from the loudest voice of the instance
  float fMaxBaseVolume = 0.0f;
  bool bAnyVoice = false;
  for (const auto& voice : m_Voices)
  {
    if (voice.m_bActive)
    {
      fMaxBaseVolume = plMath::Max(fMaxBaseVolume, voice.m_fBaseVolume);
      bAnyVoice = true;
    }
  }

  m_fDistance = (m_pEntity != nullptr) ? (m_pEntity->m_LastTransform.m_vPosition - vListenerPos).GetLength() : 0.0f;
  m_fAudibility = ComputeAudibility(bAnyVoice ? fMaxBaseVolume : 1.0f);

  return bAnyActive || bAnyPending || bHoldOpen;
}

bool plPlasmaAudioEventInstance::StartPendingTrigger(plUInt32 uiInstrument, ma_uint64 uiHorizon, plRandom& ref_rng)
{
  auto& state = m_Instruments[uiInstrument];

  if (!state.m_bPendingTrigger || m_bStopping || state.m_uiNextTriggerFrame > uiHorizon)
    return false;

  // prefer the slot that does not hold the current voice; with both slots busy the trigger waits
  // for the next Update (a start frame in the past starts with the next mixed period)
  const plUInt32 uiOther = (state.m_uiCurrentSlot + 1) % VoiceSlotsPerInstrument;
  plUInt32 uiFreeSlot = plInvalidIndex;

  if (!GetVoice(uiInstrument, uiOther).m_bActive)
    uiFreeSlot = uiOther;
  else if (!GetVoice(uiInstrument, state.m_uiCurrentSlot).m_bActive)
    uiFreeSlot = state.m_uiCurrentSlot;

  if (uiFreeSlot == plInvalidIndex)
    return false;

  StartVoice(uiInstrument, uiFreeSlot, ref_rng, state.m_uiNextTriggerFrame);
  return GetVoice(uiInstrument, uiFreeSlot).m_bActive;
}

// ---------------------------------------------------------------------------------------------
// plPlasmaAudioEventManager
// ---------------------------------------------------------------------------------------------

namespace
{
  /// look-ahead override in engine frames; 0 selects the automatic value
  plAtomicInteger64 s_iLookAheadFramesOverride;
} // namespace

void plPlasmaAudioEventManager::SetLookAheadFrames(ma_uint64 uiFrames)
{
  s_iLookAheadFramesOverride.Set(static_cast<plInt64>(uiFrames));
}

ma_uint64 plPlasmaAudioEventManager::GetLookAheadFrames(ma_engine* pEngine)
{
  const plInt64 iOverride = s_iLookAheadFramesOverride;
  if (iOverride > 0)
    return static_cast<ma_uint64>(iOverride);

  ma_uint32 uiPeriod = 0;
  if (pEngine != nullptr)
  {
    if (const ma_device* pDevice = ma_engine_get_device(pEngine))
    {
      uiPeriod = pDevice->playback.internalPeriodSizeInFrames;
    }
  }

  if (uiPeriod == 0)
    uiPeriod = 480;

  // Triggers are committed from the audio thread (Audio.FPS, 16.7 ms at the default 60), so the window must span
  // several ticks or a late tick schedules a voice in the past. 50 ms covers three default ticks plus jitter;
  // zero-delay starts are unaffected because they are scheduled at "now".
  const ma_uint32 uiSampleRate = (pEngine != nullptr) ? ma_engine_get_sample_rate(pEngine) : 48000;
  const ma_uint64 uiMinFrames = uiSampleRate / 20;

  return plMath::Max<ma_uint64>(2 * static_cast<ma_uint64>(uiPeriod), uiMinFrames);
}

plPlasmaAudioEventManager::plPlasmaAudioEventManager()
{
  m_ResourceEventSubscription = plResourceManager::GetResourceEvents().AddEventHandler(plMakeDelegate(&plPlasmaAudioEventManager::OnResourceEvent, this));
}

plPlasmaAudioEventManager::~plPlasmaAudioEventManager()
{
  plResourceManager::GetResourceEvents().RemoveEventHandler(m_ResourceEventSubscription);

  PL_LOCK(plResourceManager::GetMutex());
  m_Instances.Clear();
  m_FreeInstances.Clear();
}

plUniquePtr<plPlasmaAudioEventInstance> plPlasmaAudioEventManager::AcquireInstance()
{
  if (m_FreeInstances.IsEmpty())
    return PL_DEFAULT_NEW(plPlasmaAudioEventInstance);

  plUniquePtr<plPlasmaAudioEventInstance> pInstance = std::move(m_FreeInstances.PeekBack());
  m_FreeInstances.PopBack();
  return pInstance;
}

void plPlasmaAudioEventManager::RecycleInstance(plUniquePtr<plPlasmaAudioEventInstance>&& pInstance)
{
  if (pInstance == nullptr)
    return;

  m_VoiceManager.ForgetInstance(pInstance.Borrow());
  pInstance->Reset();

  if (m_FreeInstances.GetCount() < MaxPooledInstances)
  {
    m_FreeInstances.PushBack(std::move(pInstance));
  }
  else
  {
    pInstance.Clear();
  }
}

void plPlasmaAudioEventManager::OnResourceEvent(const plResourceEvent& e)
{
  if (e.m_Type != plResourceEvent::Type::ResourceContentUnloading)
    return;

  const plRTTI* pType = e.m_pResource->GetDynamicRTTI();
  const bool bClip = pType->IsDerivedFrom<plAudioClipResource>();

  if (!bClip && !pType->IsDerivedFrom<plAudioEventResource>())
    return;

  // already held by the resource manager while it broadcasts; taken again in case an unload path does not hold it
  PL_LOCK(plResourceManager::GetMutex());

  for (auto& pInstance : m_Instances)
  {
    if (bClip)
    {
      pInstance->OnClipUnloading(static_cast<const plAudioClipResource*>(e.m_pResource));
    }
    else if (pInstance->GetResource() == e.m_pResource)
    {
      pInstance->OnEventUnloading();
    }
  }
}

plResult plPlasmaAudioEventManager::ActivateEvent(plStringView sEventResourceId, plPlasmaAudioEntityData* pEntity, plPlasmaAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine)
{
  const plAudioEventResourceHandle hEvent = plResourceManager::LoadResource<plAudioEventResource>(sEventResourceId);
  if (!hEvent.IsValid())
    return PL_FAILURE;

  return ActivateEventInternal(hEvent, sEventResourceId, pEntity, pEventData, pBusGraph, pEngine);
}

plResult plPlasmaAudioEventManager::ActivateLegacySound(const plMiniAudioSoundResourceHandle& hSound, plPlasmaAudioEntityData* pEntity, plPlasmaAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine)
{
  if (!hSound.IsValid())
    return PL_FAILURE;

  const plString sSoundId = hSound.GetResourceID();

  plAudioEventResourceHandle hEvent;
  if (!m_LegacyEvents.TryGetValue(sSoundId, hEvent))
  {
    plResourceLock<plMiniAudioSoundResource> pSound(hSound, plResourceAcquireMode::BlockTillLoaded_NeverFail);
    if (pSound.GetAcquireResult() != plResourceAcquireResult::Final || pSound->GetNumVariants() == 0)
      return PL_FAILURE;

    plAudioEventResourceDescriptor res;
    plAudioEventDesc& desc = res.m_Desc;

    // the sound group maps onto a mixer bus of the same name (master when unknown)
    desc.m_sOutputBus = pSound->GetSoundGroup();
    desc.m_uiMaxInstances = 0; // unlimited, like the legacy one-voice-per-play behavior
    desc.m_eStealing = plAudioEventStealingMode::None;
    desc.m_bSpatialize = pSound->GetSpatialize();
    desc.m_fMinDistance = pSound->GetMinDistance();
    desc.m_fMaxDistance = pSound->GetMaxDistance();
    desc.m_fRolloff = pSound->GetRolloff();
    desc.m_fDopplerFactor = pSound->GetDopplerFactor();

    // the resource's random volume / pitch ranges become a centered value plus a +/- range
    const float fMinVolumeDb = plAudioMixerUtils::LinearToDb(pSound->GetMinVolume());
    const float fMaxVolumeDb = plAudioMixerUtils::LinearToDb(pSound->GetMaxVolume());
    const float fMinPitchSt = 12.0f * plMath::Log2(plMath::Max(pSound->GetMinPitch(), 0.01f));
    const float fMaxPitchSt = 12.0f * plMath::Log2(plMath::Max(pSound->GetMaxPitch(), 0.01f));

    plAudioEventInstrumentDesc& instrument = desc.m_Instruments.ExpandAndGetRef();
    instrument.m_sName = "Sound";
    instrument.m_ePlaylistMode = plAudioEventPlaylistMode::Random;
    instrument.m_bLoop = pSound->GetLoop();
    instrument.m_fVolumeDb = 0.5f * (fMinVolumeDb + fMaxVolumeDb);
    instrument.m_fVolumeRandDb = 0.5f * (fMaxVolumeDb - fMinVolumeDb);
    instrument.m_fPitchSemitones = 0.5f * (fMinPitchSt + fMaxPitchSt);
    instrument.m_fPitchRandSemitones = 0.5f * (fMaxPitchSt - fMinPitchSt);

    for (plUInt32 i = 0; i < pSound->GetNumVariants(); ++i)
    {
      plAudioEventPlaylistEntryDesc& entry = instrument.m_Playlist.ExpandAndGetRef();
      entry.m_uiSample = static_cast<plUInt16>(i);
      entry.m_fWeight = 1.0f;

      plStringBuilder sSampleName;
      sSampleName.SetFormat("{}#{}", sSoundId, i);
      desc.m_SampleNames.PushBack(sSampleName);
      res.m_SampleData.PushBack(pSound->GetVariantData(i));
    }

    plStringBuilder sEventId;
    sEventId.Set("Legacy:", sSoundId);

    // another world may already have created it
    hEvent = plResourceManager::GetExistingResource<plAudioEventResource>(sEventId);
    if (!hEvent.IsValid())
    {
      hEvent = plResourceManager::CreateResource<plAudioEventResource>(sEventId, std::move(res));
    }

    m_LegacyEvents.Insert(sSoundId, hEvent);
  }

  return ActivateEventInternal(hEvent, hEvent.GetResourceID(), pEntity, pEventData, pBusGraph, pEngine);
}

plResult plPlasmaAudioEventManager::ActivateEventInternal(const plAudioEventResourceHandle& hEvent, plStringView sEventResourceId, plPlasmaAudioEntityData* pEntity, plPlasmaAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine)
{
  if (!m_bRngInitialized)
  {
    m_Rng.InitializeFromCurrentTime();
    m_bRngInitialized = true;
  }

  const plUInt64 uiGroupHash = plHashHelper<plStringView>::Hash(sEventResourceId);

  // peek at the descriptor for polyphony rules
  plUInt8 uiMaxInstances = 8;
  plAudioEventStealingMode::Enum eStealing = plAudioEventStealingMode::Default;
  plTime cooldown = plTime::MakeZero();

  // loads the resource; this may wait for a loader thread, so it happens before the resource manager mutex is taken
  {
    plResourceLock<plAudioEventResource> pResource(hEvent, plResourceAcquireMode::BlockTillLoaded_NeverFail);
    if (pResource.GetAcquireResult() != plResourceAcquireResult::Final)
      return PL_FAILURE;
  }

  PL_LOCK(plResourceManager::GetMutex());

  // the name for log entries; the resource stays alive through hEvent and is not unloaded while the mutex is held
  plStringView sLogName = sEventResourceId;

  {
    plResourceLock<plAudioEventResource> pResource(hEvent, plResourceAcquireMode::PointerOnly);
    if (pResource->GetLoadingState() != plResourceState::Loaded)
      return PL_FAILURE; // unloaded again in the meantime

    uiMaxInstances = pResource->GetDescriptor().m_uiMaxInstances;
    eStealing = pResource->GetDescriptor().m_eStealing;
    cooldown = pResource->GetDescriptor().m_Cooldown;

    if (!pResource->GetResourceDescription().IsEmpty())
    {
      sLogName = pResource->GetResourceDescription().GetView();
    }
  }

  auto& group = m_EventGroups[uiGroupHash];

  // cooldown
  if (group.m_bHasStarted && cooldown > plTime::MakeZero() && (m_Now - group.m_LastStartTime) < cooldown)
  {
    plStringBuilder sDetail;
    sDetail.SetFormat("{} of {} ms", plArgF((m_Now - group.m_LastStartTime).GetMilliseconds(), 0), plArgF(cooldown.GetMilliseconds(), 0));
    m_EventLog.Add(plAudioLogEventType::CooldownRejected, sLogName, sDetail);
    return PL_SUCCESS; // silently swallowed, not an error
  }

  // polyphony
  if (uiMaxInstances > 0)
  {
    plHybridArray<plPlasmaAudioEventInstance*, 8> groupInstances;
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
        {
          plStringBuilder sDetail;
          sDetail.SetFormat("{} instances playing", groupInstances.GetCount());
          m_EventLog.Add(plAudioLogEventType::LimitRejected, sLogName, sDetail);
          return PL_SUCCESS; // new instance is not started
        }

        case plAudioEventStealingMode::Oldest:
        {
          plPlasmaAudioEventInstance* pOldest = groupInstances[0];
          for (auto* pInstance : groupInstances)
          {
            if (pInstance->GetAge() > pOldest->GetAge())
              pOldest = pInstance;
          }
          m_EventLog.Add(plAudioLogEventType::Stolen, pOldest->GetDisplayName(), "oldest");
          pOldest->Stop(false);
          break;
        }

        case plAudioEventStealingMode::Quietest:
        {
          plPlasmaAudioEventInstance* pQuietest = groupInstances[0];
          for (auto* pInstance : groupInstances)
          {
            if (pInstance->GetEffectiveVolume() < pQuietest->GetEffectiveVolume())
              pQuietest = pInstance;
          }
          m_EventLog.Add(plAudioLogEventType::Stolen, pQuietest->GetDisplayName(), "quietest");
          pQuietest->Stop(false);
          break;
        }
      }
    }
  }

  plUniquePtr<plPlasmaAudioEventInstance> pInstance = AcquireInstance();
  pInstance->m_uiEventGroupHash = uiGroupHash;

  pInstance->m_pStreams = m_pStreamService;
  pInstance->m_pVoiceManager = &m_VoiceManager;
  pInstance->m_pMixer = m_pMixerService;
  pInstance->m_pEventLog = &m_EventLog;
  pInstance->m_vListenerPos = m_vListenerPos;
  pInstance->m_uiSerial = m_uiNextInstanceSerial++;

  if (pInstance->Start(hEvent, pEntity, pEventData, pBusGraph, pEngine, &m_GlobalParameters, m_Rng).Failed())
  {
    RecycleInstance(std::move(pInstance));
    return PL_FAILURE;
  }

  group.m_LastStartTime = m_Now;
  group.m_bHasStarted = true;

  m_Instances.PushBack(std::move(pInstance));
  return PL_SUCCESS;
}

plResult plPlasmaAudioEventManager::StopEvent(const plPlasmaAudioEventData* pEventData, bool bImmediate)
{
  PL_LOCK(plResourceManager::GetMutex());

  for (auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEventData == pEventData)
    {
      m_EventLog.Add(plAudioLogEventType::Stop, pInstance->GetDisplayName(), bImmediate ? "immediate" : "fade out");
      pInstance->Stop(bImmediate);
      return PL_SUCCESS;
    }
  }

  return PL_FAILURE;
}

void plPlasmaAudioEventManager::DetachEventData(plPlasmaAudioEventData* pEventData)
{
  if (pEventData == nullptr)
    return;

  PL_LOCK(plResourceManager::GetMutex());

  pEventData->m_pInstance = nullptr;

  for (auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEventData != pEventData)
      continue;

    // Without the event data the instance no longer holds open or loops its region, so a one-shot plays to its
    // end and a stop already in progress keeps fading. A looping instrument would re-trigger forever with nothing
    // left to stop it, so such an instance fades out now.
    pInstance->m_pEventData = nullptr;

    if (pInstance->HasLoopingInstrument())
    {
      pInstance->Stop(false);
    }
  }
}

void plPlasmaAudioEventManager::StopAllOnEntity(const plPlasmaAudioEntityData* pEntity, bool bImmediate)
{
  PL_LOCK(plResourceManager::GetMutex());

  for (auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEntity == pEntity)
    {
      if (!pInstance->IsStopping() || bImmediate)
      {
        m_EventLog.Add(plAudioLogEventType::Stop, pInstance->GetDisplayName(), bImmediate ? "entity, immediate" : "entity, fade out");
      }

      pInstance->Stop(bImmediate);

      if (bImmediate)
      {
        // the entity data is about to be destroyed — sever the link
        pInstance->m_pEntity = nullptr;
      }
    }
  }
}

void plPlasmaAudioEventManager::StopAll(bool bImmediate)
{
  PL_LOCK(plResourceManager::GetMutex());

  // one entry for all instances
  if (!m_Instances.IsEmpty())
  {
    plStringBuilder sDetail;
    sDetail.SetFormat("{} instances, {}", m_Instances.GetCount(), bImmediate ? "immediate" : "fade out");
    m_EventLog.Add(plAudioLogEventType::Stop, "<all>", sDetail);
  }

  for (auto& pInstance : m_Instances)
  {
    pInstance->Stop(bImmediate);
  }

  if (bImmediate)
  {
    // first, so the recycling below has no candidates to drop
    m_VoiceManager.Reset();

    for (auto& pInstance : m_Instances)
    {
      RecycleInstance(std::move(pInstance));
    }

    m_Instances.Clear();
  }
}

void plPlasmaAudioEventManager::SetEventParameter(const plPlasmaAudioEntityData* pEntity, plStringView sParamName, float fValue)
{
  PL_LOCK(plResourceManager::GetMutex());

  for (auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEntity == pEntity)
    {
      pInstance->SetParameter(sParamName, fValue);
    }
  }
}

bool plPlasmaAudioEventManager::IsEventPlaying(const plPlasmaAudioEventData* pEventData) const
{
  PL_LOCK(plResourceManager::GetMutex());

  for (const auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEventData == pEventData)
      return true;
  }

  return false;
}

void plPlasmaAudioEventManager::Update(plTime delta, const plVec3& vListenerPos)
{
  PL_LOCK(plResourceManager::GetMutex());

  m_Now += delta;
  m_vListenerPos = vListenerPos;

  // The ranking of the last update stays in use while the instances update, so voices that start in their updates are
  // admitted against it; RecycleInstance drops the candidates of each finished instance.
  {
    PL_PROFILE_SCOPE("PlasmaAudio.Instances");

    for (plUInt32 i = 0; i < m_Instances.GetCount();)
    {
      if (!m_Instances[i]->Update(delta, vListenerPos, m_Rng))
      {
        plStringBuilder sDetail;
        sDetail.SetFormat("after {} s", plArgF(m_Instances[i]->GetAge().GetSeconds(), 2));
        m_EventLog.Add(plAudioLogEventType::Finished, m_Instances[i]->GetDisplayName(), sDetail);

        RecycleInstance(std::move(m_Instances[i]));
        m_Instances.RemoveAtAndSwap(i);
      }
      else
      {
        ++i;
      }
    }
  }

  ma_engine* pEngine = nullptr;
  for (const auto& pInstance : m_Instances)
  {
    if (pInstance->GetEngine() != nullptr)
    {
      pEngine = pInstance->GetEngine();
      break;
    }
  }

  if (pEngine != nullptr)
  {
    PL_PROFILE_SCOPE("PlasmaAudio.VoiceManager");
    m_VoiceManager.Update(m_Instances.GetArrayPtr(), ma_engine_get_time_in_pcm_frames(pEngine), plMath::Max<ma_uint32>(ma_engine_get_sample_rate(pEngine), 1u));
  }
  else
  {
    m_VoiceManager.Reset();
  }
}

plUInt32 plPlasmaAudioEventManager::GetRealVoiceCount() const
{
  PL_LOCK(plResourceManager::GetMutex());

  plUInt32 uiCount = 0;
  for (const auto& pInstance : m_Instances)
  {
    uiCount += pInstance->GetRealVoiceCount();
  }
  return uiCount;
}

plUInt32 plPlasmaAudioEventManager::GetVirtualVoiceCount() const
{
  PL_LOCK(plResourceManager::GetMutex());

  plUInt32 uiCount = 0;
  for (const auto& pInstance : m_Instances)
  {
    uiCount += pInstance->GetVirtualVoiceCount();
  }
  return uiCount;
}

void plPlasmaAudioEventManager::GetEventVoiceCounts(const plPlasmaAudioEventData* pEventData, plUInt32& out_uiReal, plUInt32& out_uiVirtual) const
{
  PL_LOCK(plResourceManager::GetMutex());

  out_uiReal = 0;
  out_uiVirtual = 0;

  for (const auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEventData == pEventData)
    {
      out_uiReal += pInstance->GetRealVoiceCount();
      out_uiVirtual += pInstance->GetVirtualVoiceCount();
    }
  }
}

plUInt64 plPlasmaAudioEventManager::GetVoiceFlipCount() const
{
  PL_LOCK(plResourceManager::GetMutex());
  return m_VoiceManager.GetFlipCount();
}

void plPlasmaAudioEventManager::FillSnapshot(plAudioProfilingSnapshot& out_snapshot) const
{
  PL_LOCK(plResourceManager::GetMutex());

  out_snapshot.m_uiMaxReal = plAudioVoiceManager::GetMaxRealVoices();
  out_snapshot.m_uiMaxVirtual = plAudioVoiceManager::GetMaxVirtualVoices();
  out_snapshot.m_uiInstances = m_Instances.GetCount();
  out_snapshot.m_uiRealVoices = 0;
  out_snapshot.m_uiVirtualVoices = 0;

  // Clear() keeps the capacity of the array
  out_snapshot.m_Voices.Clear();

  for (const auto& pInstance : m_Instances)
  {
    out_snapshot.m_uiRealVoices += pInstance->GetRealVoiceCount();
    out_snapshot.m_uiVirtualVoices += pInstance->GetVirtualVoiceCount();
    pInstance->AppendSnapshotVoices(out_snapshot.m_Voices);
  }
}
