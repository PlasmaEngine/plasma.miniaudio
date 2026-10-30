#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <AudioSystem/Core/AudioMiddleware.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <Foundation/Threading/Lock.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResource.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResourceLoader.h>
#include <PlasmaAudioPlugin/Events/MiniAudioAudioEvents.h>
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
      pSource->m_fEnginePosition = static_cast<double>(uiNow);
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

    const double fSourcePerEngine = plMath::Max(pSource->m_fSourceFramesPerEngineFrame, 1e-6);

    if (uiRead < uiFrameCount && !pSource->m_bPadded)
    {
      void* pInnerOut = (pFramesOut != nullptr) ? ma_offset_pcm_frames_ptr(pFramesOut, uiRead, pSource->m_eFormat, pSource->m_uiChannels) : nullptr;

      // an end frame limits the read to the source frames before it
      const plInt64 iEndFrame = pSource->m_iEndFrame;
      const double fInnerStart = pSource->m_fEnginePosition + static_cast<double>(uiRead) / fSourcePerEngine;
      ma_uint64 uiWanted = uiFrameCount - uiRead;
      bool bEndReached = false;

      if (iEndFrame > 0)
      {
        const double fLeft = plMath::Max((static_cast<double>(iEndFrame) - fInnerStart) * fSourcePerEngine, 0.0);
        const ma_uint64 uiLeft = static_cast<ma_uint64>(fLeft + 0.5);
        if (uiLeft <= uiWanted)
        {
          uiWanted = uiLeft;
          bEndReached = true;
        }
      }

      ma_uint64 uiCursorBefore = 0;
      const bool bCursorBefore = ma_data_source_get_cursor_in_pcm_frames(pSource->m_pInner, &uiCursorBefore) == MA_SUCCESS;

      ma_uint64 uiInnerRead = 0;
      const ma_result res = (uiWanted > 0) ? ma_data_source_read_pcm_frames(pSource->m_pInner, pInnerOut, uiWanted, &uiInnerRead) : MA_AT_END;

      // the frames within the end fade fall to silence at the end frame
      const plInt64 iEndFade = pSource->m_iEndFadeFrames;
      if (iEndFrame > 0 && iEndFade > 0 && pInnerOut != nullptr)
      {
        const double fFadeStart = static_cast<double>(iEndFrame - iEndFade);
        const double fFirst = plMath::Max((fFadeStart - fInnerStart) * fSourcePerEngine, 0.0);

        for (ma_uint64 i = static_cast<ma_uint64>(fFirst); i < uiInnerRead; ++i)
        {
          const double fFrame = fInnerStart + static_cast<double>(i) / fSourcePerEngine;
          const float fGain = static_cast<float>(plMath::Clamp((static_cast<double>(iEndFrame) - fFrame) / static_cast<double>(iEndFade), 0.0, 1.0));
          ma_apply_volume_factor_pcm_frames(ma_offset_pcm_frames_ptr(pInnerOut, i, pSource->m_eFormat, pSource->m_uiChannels), 1, pSource->m_eFormat, pSource->m_uiChannels, fGain);
        }
      }

      uiRead += uiInnerRead;

      // Publish the position for the audio thread. Frames returned without the cursor advancing are the silence a
      // starved stream plays; a cursor that moved backwards looped, which says nothing about starvation.
      ma_uint64 uiCursorAfter = 0;
      if (ma_data_source_get_cursor_in_pcm_frames(pSource->m_pInner, &uiCursorAfter) == MA_SUCCESS)
      {
        pSource->m_iPublishedCursor = static_cast<plInt64>(uiCursorAfter);

        if (bCursorBefore && uiCursorAfter >= uiCursorBefore && uiInnerRead > uiCursorAfter - uiCursorBefore)
        {
          pSource->m_iStarvedFrames.Add(static_cast<plInt64>(uiInnerRead - (uiCursorAfter - uiCursorBefore)));
        }
      }

      if (res != MA_SUCCESS && res != MA_AT_END)
      {
        *pFramesRead = uiRead;
        return res;
      }

      // the inner source is done or the end frame came: append the silent frame
      if (uiRead < uiFrameCount && (res == MA_AT_END || uiInnerRead == 0 || bEndReached))
      {
        if (pFramesOut != nullptr)
        {
          ma_silence_pcm_frames(ma_offset_pcm_frames_ptr(pFramesOut, uiRead, pSource->m_eFormat, pSource->m_uiChannels), 1, pSource->m_eFormat, pSource->m_uiChannels);
        }

        pSource->m_bPadded = true;
        ++uiRead;
      }
    }

    pSource->m_fEnginePosition += static_cast<double>(uiRead) / fSourcePerEngine;

    *pFramesRead = uiRead;
    return uiRead > 0 ? MA_SUCCESS : MA_AT_END;
  }

  ma_result VoiceSourceSeek(ma_data_source* pDataSource, ma_uint64 uiFrameIndex)
  {
    auto* pSource = static_cast<plMaVoiceDataSource*>(pDataSource);
    pSource->m_uiLeadFramesRemaining = 0;
    pSource->m_bLeadResolved = true;
    pSource->m_bPadded = false;
    pSource->m_iPublishedCursor = static_cast<plInt64>(uiFrameIndex);
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
    pSource->m_iStarvedFrames = 0;
    pSource->m_fEnginePosition = 0.0;
    pSource->m_iEndFrame = 0;
    pSource->m_iEndFadeFrames = 0;

    // the inner source may already have been sought to where a realized voice resumes
    ma_uint64 uiCursor = 0;
    ma_data_source_get_cursor_in_pcm_frames(pInner, &uiCursor);
    pSource->m_iPublishedCursor = static_cast<plInt64>(uiCursor);

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
  m_ParameterTargets.Clear();
  m_ParameterNameHashes.Clear();
  m_ParameterLocal.Clear();
  m_AutomationValues.Clear();
  m_fAutomationCutoff = 22000.0f;
  m_uiReleasedSustains = 0;
  m_fMarkerPosition = 0.0;
  m_uiClipSourcesAssigned = 0;

  m_pEntity = nullptr;
  m_pEventData = nullptr;
  m_uiEventGroupHash = 0;
  m_pStreams = nullptr;
  m_pVoiceManager = nullptr;
  m_pEventLog = nullptr;
  m_vListenerPos = plVec3::MakeZero();
  m_uiSerial = 0;
  m_ChildRequests.Clear();
  m_uiParentSerial = 0;
  m_uiParentInstrument = 0;
  m_uiNestingDepth = 0;
  m_uiLiveChildren = 0;
  m_uiRequestedOriginFrame = 0;
  m_bRequestedLimit = false;
  m_RequestedLength = plTime::MakeZero();
  m_uiStopFrame = 0;

  m_Music.Reset();
  m_uiMusicStateParameter = plInvalidIndex;
  m_MusicPlays.Clear();
  m_MusicCuts.Clear();
  m_MusicMessages.Clear();

  m_hResource.Invalidate();
  m_pBusGraph = nullptr;
  m_pEngine = nullptr;
  m_pGlobalParameters = nullptr;

  m_uiStartFrame = 0;
  m_uiTimelineOriginFrame = 0;
  m_uiSampleRate = 48000;
  m_uiRegionLoopInstrument = plInvalidIndex;
  m_bSeeded = false;

  m_Elapsed = plTime::MakeZero();
  m_bStopping = false;
  m_fCurrentVolumeMultiplier = 1.0f;
  m_fCurrentSpatialGain = 1.0f;
  m_fCurrentPitchMultiplier = 1.0f;
  m_fAudibility = 1.0f;
  m_fDistance = 0.0f;
  m_uiBus = 0;
}

plResult plPlasmaAudioEventInstance::Start(const plAudioEventResourceHandle& hEvent, plPlasmaAudioEntityData* pEntity, plPlasmaAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine, const plHashTable<plUInt64, float>* pGlobalParameters, plRandom& ref_sharedRng)
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

  plTime startOffset = plTime::MakeZero();

  if (m_pEventData != nullptr)
  {
    m_pEventData->m_pInstance = this;

    // a seeded instance draws its playlist picks and randomization from its own generator, so they repeat
    if (m_pEventData->m_bSeeded)
    {
      m_SeededRng.Initialize(m_pEventData->m_uiSeed);
      m_bSeeded = true;
    }

    startOffset = m_pEventData->m_StartOffset;
  }

  plRandom& ref_rng = m_bSeeded ? m_SeededRng : ref_sharedRng;

  m_uiSampleRate = plMath::Max<ma_uint32>(ma_engine_get_sample_rate(m_pEngine), 1u);
  m_uiStartFrame = ma_engine_get_time_in_pcm_frames(m_pEngine);
  m_uiTimelineOriginFrame = m_uiStartFrame - plMath::Min(ToFrames(startOffset), m_uiStartFrame);

  // a nested event starts where its parent's playlist entry does: a frame ahead lets its first voices start there, a
  // frame already passed starts it like a start offset
  if (m_uiRequestedOriginFrame != 0)
  {
    m_uiTimelineOriginFrame = m_uiRequestedOriginFrame;
    if (m_uiRequestedOriginFrame < m_uiStartFrame)
    {
      startOffset = plTime::MakeFromSeconds(static_cast<double>(m_uiStartFrame - m_uiRequestedOriginFrame) / m_uiSampleRate);
    }
  }

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
  m_ParameterTargets.SetCount(desc.m_Parameters.GetCount());
  m_ParameterNameHashes.SetCount(desc.m_Parameters.GetCount());
  m_ParameterLocal.SetCount(desc.m_Parameters.GetCount());
  for (plUInt32 i = 0; i < desc.m_Parameters.GetCount(); ++i)
  {
    m_ParameterValues[i] = desc.m_Parameters[i].m_fDefault;
    m_ParameterTargets[i] = desc.m_Parameters[i].m_fDefault;
    m_ParameterNameHashes[i] = plHashingUtils::StringHash(desc.m_Parameters[i].m_sName.GetView());
    m_ParameterLocal[i] = false;
  }

  m_AutomationValues.SetCount(desc.m_Automations.GetCount());
  m_fAutomationCutoff = 22000.0f;

  // an instance starts at the global values; only later changes seek
  ApplyGlobalParameters();
  m_ParameterValues = m_ParameterTargets;

  // the first voices start with the distance gain they will have
  m_fCurrentSpatialGain = ComputeSpatialGain();

  // voices are never relocated after this point: miniaudio nodes reference them by address
  m_Voices.SetCount(desc.m_Instruments.GetCount() * VoiceSlotsPerInstrument);
  m_Instruments.SetCount(desc.m_Instruments.GetCount());

  const bool bRegionLooping = IsRegionLooping();
  plUInt32 uiInstrumentsInRegion = 0;

  // sustain points the start offset lies behind are passed; markers there are not sent
  m_uiReleasedSustains = 0;
  while (m_uiReleasedSustains < desc.m_SustainPoints.GetCount() && desc.m_SustainPoints[m_uiReleasedSustains] < startOffset)
  {
    ++m_uiReleasedSustains;
  }

  m_fMarkerPosition = startOffset.GetSeconds();

  for (plUInt32 i = 0; i < desc.m_Instruments.GetCount(); ++i)
  {
    const auto& instrumentDesc = desc.m_Instruments[i];

    plUInt8 uiSustainIndex = 0;
    while (uiSustainIndex < desc.m_SustainPoints.GetCount() && desc.m_SustainPoints[uiSustainIndex] <= instrumentDesc.m_StartDelay)
    {
      ++uiSustainIndex;
    }

    m_Instruments[i].m_uiSustainIndex = uiSustainIndex;

    // switch filter is evaluated when the event starts, so a switch change affects the next trigger
    const bool bSwitchAllows = m_pEntity == nullptr || m_pEntity->MatchesSwitchFilter(instrumentDesc.m_sSwitchState);

    // instruments behind the loop region are never reached while the region loops
    const bool bBehindRegion = bRegionLooping && instrumentDesc.m_StartDelay >= desc.m_LoopEnd;

    const bool bSkip = instrumentDesc.m_Playlist.IsEmpty() || instrumentDesc.m_bMute || !bSwitchAllows || bBehindRegion;

    m_Instruments[i].m_uiNextTriggerFrame = m_uiTimelineOriginFrame + ToFrames(instrumentDesc.m_StartDelay);
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

  // a nested event of a non-looping instrument plays for the length the instrument gives it, or one pass (measured in
  // Update while its clips still load)
  m_uiStopFrame = 0;
  if (m_bRequestedLimit && m_RequestedLength.IsPositive())
    m_uiStopFrame = m_uiTimelineOriginFrame + plMath::Max<ma_uint64>(ToFrames(m_RequestedLength), 1);

  const ma_uint64 uiHorizon = m_uiStartFrame + plPlasmaAudioEventManager::GetLookAheadFrames(m_pEngine);

  // music: the music player starts the stems, not the instruments' start delays; the first segment starts right away
  if (desc.m_Music.IsMusic())
  {
    for (InstrumentState& state : m_Instruments)
    {
      state.m_bPendingTrigger = false;
      state.m_bDone = true;
    }

    m_uiMusicStateParameter = desc.FindParameterByName(plAudioMusicDesc::StateParameter);
    const float fState = (m_uiMusicStateParameter < m_ParameterTargets.GetCount()) ? m_ParameterTargets[m_uiMusicStateParameter] : 0.0f;

    m_Music.Start(&desc.m_Music, m_uiStartFrame, m_uiSampleRate, static_cast<plUInt16>(plMath::Max(plMath::Round(fState), 0.0f)));
    UpdateMusic(m_uiStartFrame, uiHorizon, ref_rng);
  }

  // every instrument whose start frame lies inside the look-ahead window is scheduled right away with its exact frame
  for (plUInt32 i = 0; i < desc.m_Instruments.GetCount(); ++i)
  {
    if (m_Instruments[i].m_bPendingTrigger && m_Instruments[i].m_uiNextTriggerFrame <= uiHorizon && !IsWaitingForKeyOff(i))
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

double plPlasmaAudioEventInstance::GetSourceFramesPerEngineFrame(const Voice& voice) const
{
  const double fPitch = plMath::Max(static_cast<double>(voice.m_fBasePitch * m_fCurrentPitchMultiplier), 0.01);
  return fPitch * static_cast<double>(voice.m_uiSourceSampleRate) / static_cast<double>(m_uiSampleRate);
}

void plPlasmaAudioEventInstance::AdvanceVirtualCursor(Voice& voice, ma_uint64 uiNow) const
{
  // the timeline does not move before the voice's start frame
  const ma_uint64 uiFrom = plMath::Max(voice.m_uiVirtualCursorFrame, voice.m_uiStartFrame);
  if (uiNow > uiFrom && voice.m_uiSourceSampleRate > 0)
  {
    voice.m_fVirtualCursor += static_cast<double>(uiNow - uiFrom) * GetSourceFramesPerEngineFrame(voice);
  }

  voice.m_uiVirtualCursorFrame = plMath::Max(voice.m_uiVirtualCursorFrame, uiNow);
}

ma_uint64 plPlasmaAudioEventInstance::EstimateVoiceEndFrame(const Voice& voice, ma_uint64 uiNow) const
{
  const ma_uint64 uiDataEnd = EstimateVoiceDataEndFrame(voice, uiNow);

  // a music voice's data ends at its end frame at the latest
  if (!voice.m_bActive || voice.m_uiEndFrame == 0)
    return uiDataEnd;

  return (uiDataEnd == UnknownFrame) ? voice.m_uiEndFrame : plMath::Min(uiDataEnd, voice.m_uiEndFrame);
}

ma_uint64 plPlasmaAudioEventInstance::EstimateVoiceDataEndFrame(const Voice& voice, ma_uint64 uiNow) const
{
  if (!voice.m_bActive || voice.m_bNativeLoop || voice.m_uiLengthSourceFrames == 0 || voice.m_uiSourceSampleRate == 0)
    return UnknownFrame;

  const double fFramesPerEngineFrame = GetSourceFramesPerEngineFrame(voice);
  const double fLength = static_cast<double>(voice.m_uiLengthSourceFrames);

  auto EndFrom = [&](ma_uint64 uiFrame, double fSourceFramesLeft)
  {
    return uiFrame + static_cast<ma_uint64>(plMath::Max(fSourceFramesLeft, 0.0) / fFramesPerEngineFrame + 0.5);
  };

  // not playing yet: the whole length at the current pitch
  if (uiNow <= voice.m_uiStartFrame)
    return EndFrom(voice.m_uiStartFrame, fLength);

  // virtual: the cursor Update advances, carried on to now
  if (voice.m_bVirtual)
  {
    const ma_uint64 uiFrom = plMath::Max(voice.m_uiVirtualCursorFrame, voice.m_uiStartFrame);
    const double fCursor = voice.m_fVirtualCursor + ((uiNow > uiFrom) ? static_cast<double>(uiNow - uiFrom) * fFramesPerEngineFrame : 0.0);
    return EndFrom(uiNow, fLength - fCursor);
  }

  if (!voice.m_bSourceValid)
    return EndFrom(voice.m_uiStartFrame, fLength);

  // Unchanged pitch: exact frame arithmetic from the start frame, delayed by the silence a starved stream played (the
  // stream resumes where it stopped). Otherwise the cursor the device thread published, at the current pitch.
  const double fStarved = static_cast<double>(static_cast<plInt64>(voice.m_Source.m_iStarvedFrames));
  if (voice.m_fBasePitch * m_fCurrentPitchMultiplier == voice.m_fStartPitch)
    return EndFrom(voice.m_uiStartFrame, fLength + fStarved);

  const double fCursor = static_cast<double>(static_cast<plInt64>(voice.m_Source.m_iPublishedCursor));
  return EndFrom(uiNow, fLength - fCursor);
}

float plPlasmaAudioEventInstance::ComputeSpatialGain() const
{
  if (m_pResource == nullptr || m_pEntity == nullptr)
    return 1.0f;

  const auto& desc = m_pResource->GetDescriptor();
  float fGain = desc.GetOcclusionGain(m_pEntity->m_fCurrentOcclusion);

  float fDistance = 0.0f;
  if (GetListenerDistance(fDistance))
  {
    fGain *= desc.GetDistanceGain(fDistance);
  }

  return fGain;
}

bool plPlasmaAudioEventInstance::GetListenerDistance(float& out_fDistance) const
{
  if (m_pEntity == nullptr)
    return false;

  if (m_pEntity->m_bGlobalEntity)
  {
    out_fDistance = m_pEntity->m_fAuditionDistance;
    return m_pEntity->m_fAuditionDistance >= 0.0f;
  }

  out_fDistance = (m_pEntity->m_LastTransform.m_vPosition - m_vListenerPos).GetLength();
  return true;
}

bool plPlasmaAudioEventInstance::GetDebugEmitter(plAudioDebugEmitter& out_emitter) const
{
  if (m_pResource == nullptr || m_pEntity == nullptr || m_pEntity->m_bGlobalEntity)
    return false;

  const auto& desc = m_pResource->GetDescriptor();
  if (!desc.m_bSpatialize)
    return false;

  out_emitter.m_sEvent = GetDisplayName();
  out_emitter.m_vPosition = m_pEntity->m_LastTransform.m_vPosition;
  out_emitter.m_vForward = m_pEntity->m_LastTransform.m_vForward;
  out_emitter.m_vListener = m_vListenerPos;
  out_emitter.m_fMinDistance = desc.m_fMinDistance;
  out_emitter.m_fMaxDistance = desc.m_fMaxDistance;
  out_emitter.m_fConeInnerAngle = desc.m_fConeInnerAngle;
  out_emitter.m_fConeOuterAngle = desc.m_fConeOuterAngle;
  out_emitter.m_fDistance = (m_pEntity->m_LastTransform.m_vPosition - m_vListenerPos).GetLength();
  out_emitter.m_fGain = m_fCurrentSpatialGain * ComputeConeGain();
  out_emitter.m_fOcclusion = plMath::Clamp(m_pEntity->m_fCurrentOcclusion, 0.0f, 1.0f);
  out_emitter.m_fAudibility = m_fAudibility;
  out_emitter.m_bReal = GetRealVoiceCount() > 0;

  out_emitter.m_EnvironmentNames.Clear();
  out_emitter.m_EnvironmentAmounts.Clear();
  for (const auto& env : m_pEntity->m_Environments)
  {
    if (env.m_fAmount > 0.0001f)
    {
      out_emitter.m_EnvironmentNames.PushBack(env.m_sName);
      out_emitter.m_EnvironmentAmounts.PushBack(env.m_fAmount);
    }
  }

  return true;
}

void plPlasmaAudioEventInstance::UpdateSpatializer(Voice& ref_voice) const
{
  if (ref_voice.m_pSpatializerNode == nullptr || m_pEntity == nullptr || m_pResource == nullptr)
    return;

  plAudioSpatialVoiceState state;
  state.m_vEmitterPosition = m_pEntity->m_LastTransform.m_vPosition;
  state.m_vEmitterForward = m_pEntity->m_LastTransform.m_vForward;
  state.m_vListenerPosition = m_vListenerPos;
  state.m_fSpread = m_pResource->GetDescriptor().m_fSpread;

  // the listener miniaudio would spatialize against: the closest one
  const ma_uint32 uiListener = ma_engine_find_closest_listener(m_pEngine, m_vListenerPos.x, m_vListenerPos.y, m_vListenerPos.z);
  const ma_vec3f vForward = ma_engine_listener_get_direction(m_pEngine, uiListener);
  const ma_vec3f vUp = ma_engine_listener_get_world_up(m_pEngine, uiListener);
  state.m_vListenerForward.Set(vForward.x, vForward.y, vForward.z);
  state.m_vListenerUp.Set(vUp.x, vUp.y, vUp.z);

  ref_voice.m_pSpatializer->UpdateVoice(ref_voice.m_pSpatializerNode, state);
}

float plPlasmaAudioEventInstance::ComputeConeGain() const
{
  if (m_pResource == nullptr || m_pEntity == nullptr || m_pEntity->m_bGlobalEntity)
    return 1.0f;

  const auto& desc = m_pResource->GetDescriptor();
  if (!desc.m_bSpatialize || (desc.m_fConeInnerAngle >= 360.0f && desc.m_fConeOuterAngle >= 360.0f))
    return 1.0f;

  plVec3 vForward = m_pEntity->m_LastTransform.m_vForward;
  plVec3 vToListener = m_vListenerPos - m_pEntity->m_LastTransform.m_vPosition;
  if (vForward.NormalizeIfNotZero(plVec3::MakeZero()).Failed() || vToListener.NormalizeIfNotZero(plVec3::MakeZero()).Failed())
    return 1.0f;

  return desc.GetConeGain(plMath::ACos(plMath::Clamp(vForward.Dot(vToListener), -1.0f, 1.0f)).GetDegree());
}

void plPlasmaAudioEventInstance::SelectListener(plArrayPtr<const plVec3> listeners)
{
  if (listeners.IsEmpty())
    return;

  if (m_pEntity == nullptr || m_pEntity->m_bGlobalEntity)
  {
    m_vListenerPos = listeners[0];
    return;
  }

  const plVec3 vEntity = m_pEntity->m_LastTransform.m_vPosition;
  float fClosest = plMath::MaxValue<float>();

  for (const plVec3& vListener : listeners)
  {
    const float fDistanceSq = (vEntity - vListener).GetLengthSquared();
    if (fDistanceSq < fClosest)
    {
      fClosest = fDistanceSq;
      m_vListenerPos = vListener;
    }
  }
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
      SetParameterTarget(i, fValue);
    }
  }
}

void plPlasmaAudioEventInstance::SetParameterTarget(plUInt32 uiParam, float fValue)
{
  const plAudioEventParameterDesc& parameter = m_pResource->GetDescriptor().m_Parameters[uiParam];

  m_ParameterTargets[uiParam] = plMath::Clamp(fValue, parameter.m_fMin, parameter.m_fMax);

  if (parameter.m_fSeekSpeed <= 0.0f)
    m_ParameterValues[uiParam] = m_ParameterTargets[uiParam];
}

void plPlasmaAudioEventInstance::SeekParameters(plTime delta)
{
  const auto& desc = m_pResource->GetDescriptor();
  const float fSeconds = static_cast<float>(delta.GetSeconds());

  for (plUInt32 i = 0; i < m_ParameterValues.GetCount() && i < desc.m_Parameters.GetCount(); ++i)
  {
    const float fStep = desc.m_Parameters[i].m_fSeekSpeed * fSeconds;
    const float fDiff = m_ParameterTargets[i] - m_ParameterValues[i];

    if (desc.m_Parameters[i].m_fSeekSpeed <= 0.0f || plMath::Abs(fDiff) <= fStep)
      m_ParameterValues[i] = m_ParameterTargets[i];
    else
      m_ParameterValues[i] += plMath::Sign(fDiff) * fStep;
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

    if (m_pDeferredLoads != nullptr)
    {
      m_pDeferredLoads->Preload(hClip);
    }
    out_bNotLoaded = true;
    return PL_FAILURE;
  }

  // A language switch reloads the loaded clips, but not those whose load was in flight: such a clip finished loading
  // in the previous language and is reloaded before it plays. Clips with a single language sound the same in every
  // language. When the clip cannot be reloaded (created from memory) it plays as it is.
  if (pClip->GetLanguageGeneration() != plAudioClipResourceLoader::GetLanguageGeneration() && pClip->GetHeader().m_Languages.GetCount() > 1 &&
      pClip->GetBaseResourceFlags().IsSet(plResourceFlags::IsReloadable) && m_pDeferredLoads != nullptr)
  {
    plResourceManager::EndAcquireResource(pClip);
    m_pDeferredLoads->Reload(hClip);
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

  if (m_pResource == nullptr)
    return 22000.0f;

  return plMath::Min(m_pResource->GetDescriptor().GetOcclusionCutoff(fOcclusion), m_fAutomationCutoff);
}

void plPlasmaAudioEventInstance::StartVoice(plUInt32 uiInstrument, plUInt32 uiSlot, plRandom& ref_rng, ma_uint64 uiStartFrame, ma_uint64 uiOffsetFrames, ma_uint64 uiEndFrame)
{
  const auto& desc = m_pResource->GetDescriptor();
  const auto& instrumentDesc = desc.m_Instruments[uiInstrument];
  auto& state = m_Instruments[uiInstrument];
  auto& voice = GetVoice(uiInstrument, uiSlot);

  state.m_bPendingTrigger = false;
  state.m_bGapRolled = false;

  const plUInt32 uiEntry = PickPlaylistEntry(instrumentDesc, state, ref_rng);

  // a nested event: the manager starts it as a child instance after this update; the instrument waits for it to end
  if (uiEntry != plInvalidIndex && !instrumentDesc.m_Playlist[uiEntry].m_sEvent.IsEmpty())
  {
    ChildRequest& request = m_ChildRequests.ExpandAndGetRef();
    request.m_hEvent = m_pResource->GetNestedEvent(uiInstrument, uiEntry);
    request.m_uiOriginFrame = uiStartFrame;
    request.m_uiInstrument = static_cast<plUInt8>(uiInstrument);
    request.m_bLimited = !instrumentDesc.m_bLoop;
    request.m_Length = instrumentDesc.m_NestedLength;

    state.m_bChildPlaying = true;
    return;
  }

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
      if (m_pDeferredLoads != nullptr)
      {
        m_pDeferredLoads->Preload(hClip);
      }
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

  // a music voice may start inside its data and end at a frame; its timeline starts the offset before its start frame
  voice.m_uiOffsetFrames = plMath::Min(uiOffsetFrames, uiStartFrame);
  voice.m_uiEndFrame = uiEndFrame;
  voice.m_uiEndFadeFrames = 0;
  const ma_uint64 uiTimelineStart = uiStartFrame - voice.m_uiOffsetFrames;

  // randomized base volume / pitch
  const float fVolDb = instrumentDesc.m_fVolumeDb + static_cast<float>(ref_rng.DoubleMinMax(-instrumentDesc.m_fVolumeRandDb, instrumentDesc.m_fVolumeRandDb));
  const float fPitchSt = instrumentDesc.m_fPitchSemitones + static_cast<float>(ref_rng.DoubleMinMax(-instrumentDesc.m_fPitchRandSemitones, instrumentDesc.m_fPitchRandSemitones));
  voice.m_fBaseVolume = plAudioMixerUtils::DbToLinear(fVolDb);
  voice.m_fBasePitch = plAudioMixerUtils::SemitonesToPitch(fPitchSt);

  ApplyModulation(); // current volume / pitch multipliers for the admission and the new voice

  // the follow-up voice of a looping instrument inherits the state of the voice it follows and is not logged
  const bool bFollowUp = GetVoice(uiInstrument, (uiSlot + 1) % VoiceSlotsPerInstrument).m_bActive;

  const bool bAdmitted = AdmitVoice(uiInstrument, uiSlot, GetVoiceVolume(voice));

  // a start offset puts the start frames of the instruments it skips before the instance's start: such a voice begins
  // as a virtual voice whose timeline started at that frame and is made real right away, mid-sample
  const bool bBeforeStart = uiStartFrame < m_uiStartFrame;

  if (!bAdmitted || bBeforeStart)
  {
    // a Kill event plays real or not at all, as when one of its voices is virtualized
    if (!bAdmitted && desc.m_eVirtualMode == plAudioEventVirtualMode::Kill)
    {
      Log(plAudioLogEventType::Killed, "not admitted at start");
      Stop(true);
      return;
    }

    if (!bAdmitted && !bFollowUp && m_pEventLog != nullptr)
    {
      plStringBuilder sDetail;
      sDetail.SetFormat("audibility {}", plArgF(ComputeAudibility(GetVoiceVolume(voice)), 3));
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
    voice.m_uiStartFrame = uiTimelineStart;
    voice.m_fStartPitch = voice.m_fBasePitch * m_fCurrentPitchMultiplier;
    voice.m_fVirtualCursor = 0.0;
    voice.m_uiVirtualCursorFrame = uiTimelineStart;
    voice.m_bVirtual = true;
    voice.m_bActive = true;
    state.m_uiCurrentSlot = static_cast<plUInt8>(uiSlot);
    state.m_bLoadWaitLogged = false;

    // a voice that ran out before the start offset stays virtual and Update ends it
    if (bAdmitted)
    {
      RealizeVoice(uiInstrument * VoiceSlotsPerInstrument + uiSlot);
    }
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

  voice.m_uiStartFrame = uiTimelineStart;
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

  // a music voice that starts at a later position of its data starts there
  if (bFreshStart && voice.m_uiOffsetFrames > 0 && voice.m_uiSourceSampleRate > 0)
  {
    const double fSourcePerEngine = static_cast<double>(voice.m_fBasePitch * m_fCurrentPitchMultiplier) * voice.m_uiSourceSampleRate / m_uiSampleRate;
    uiSourceCursor = static_cast<ma_uint64>(static_cast<double>(voice.m_uiOffsetFrames) * fSourcePerEngine + 0.5);

    if (voice.m_uiLengthSourceFrames > 0 && uiSourceCursor >= voice.m_uiLengthSourceFrames)
    {
      if (!voice.m_bNativeLoop)
      {
        ReleaseVoiceChain(voice);
        return BuildResult::ClipFailed;
      }

      uiSourceCursor %= voice.m_uiLengthSourceFrames;
    }
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

  // the end of a music voice's data carries over to every chain the voice gets
  voice.m_Source.m_iEndFadeFrames = static_cast<plInt64>(voice.m_uiEndFadeFrames);
  voice.m_Source.m_iEndFrame = static_cast<plInt64>(voice.m_uiEndFrame);

  if (ma_sound_init_from_data_source(m_pEngine, &voice.m_Source, 0, nullptr, &voice.m_Sound) != MA_SUCCESS)
  {
    ReleaseVoiceChain(voice);
    return BuildResult::Failed;
  }
  voice.m_bSoundValid = true;

  // spatializer
  ma_sound_set_spatialization_enabled(&voice.m_Sound, desc.m_bSpatialize && (m_pEntity == nullptr || !m_pEntity->m_bGlobalEntity));
  ma_sound_set_doppler_factor(&voice.m_Sound, desc.m_fDopplerFactor);

  // the distance gain is the event's attenuation curve (ComputeSpatialGain, in the voice volume), so miniaudio only
  // pans, applies the Doppler shift and the cone
  ma_sound_set_attenuation_model(&voice.m_Sound, ma_attenuation_model_none);
  ma_sound_set_cone(&voice.m_Sound, plAngle::MakeFromDegree(plMath::Clamp(desc.m_fConeInnerAngle, 0.0f, 360.0f)).GetRadian(),
    plAngle::MakeFromDegree(plMath::Clamp(plMath::Max(desc.m_fConeOuterAngle, desc.m_fConeInnerAngle), 0.0f, 360.0f)).GetRadian(),
    plMath::Pow(10.0f, desc.m_fConeOuterGainDb / 20.0f));

  // spread: the smallest channel gain of the panning; miniaudio's default 0.2 pans a point, 1 is heard everywhere
  voice.m_Sound.engineNode.spatializer.minSpatializationChannelGain = plMath::Lerp(0.2f, 1.0f, plMath::Clamp(desc.m_fSpread / 360.0f, 0.0f, 1.0f));

  if (m_pEntity != nullptr)
  {
    const plVec3 vPos = m_pEntity->m_LastTransform.m_vPosition;
    const plVec3 vVel = m_pEntity->m_LastTransform.m_vVelocity;
    const plVec3 vDir = m_pEntity->m_LastTransform.m_vForward;
    ma_sound_set_position(&voice.m_Sound, vPos.x, vPos.y, vPos.z);
    ma_sound_set_velocity(&voice.m_Sound, vVel.x, vVel.y, vVel.z);
    ma_sound_set_direction(&voice.m_Sound, vDir.x, vDir.y, vDir.z);
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

  // an active spatializer places spatialized voices instead of miniaudio's panning
  plAudioSpatializer* pSpatializer = plAudioSpatializer::GetActive();
  if (pSpatializer != nullptr && desc.m_bSpatialize && m_pEntity != nullptr && !m_pEntity->m_bGlobalEntity)
  {
    if (ma_node* pNode = pSpatializer->CreateVoiceNode(m_pEngine))
    {
      voice.m_pSpatializerNode = pNode;
      voice.m_pSpatializer = pSpatializer;
      ma_sound_set_spatialization_enabled(&voice.m_Sound, MA_FALSE);
      ma_node_attach_output_bus(pCurrent, 0, pNode, 0);
      pCurrent = pNode;
      UpdateSpatializer(voice);
    }
  }

  // the occlusion filter's cutoff follows the occlusion through atomics the device thread applies (plMaFilterNode)
  if (plMiniAudioDsp::InitFilterNode(m_pEngine, &voice.m_OcclusionFilter, plMaFilterNode::Type::LowPass, 2, GetOcclusionCutoff(), 2, 0.0f, 1.0f, &m_pEngine->allocationCallbacks).Succeeded())
  {
    voice.m_bOcclusionLpfValid = true;
    ma_node_attach_output_bus(pCurrent, 0, reinterpret_cast<ma_node*>(&voice.m_OcclusionFilter), 0);
    pCurrent = reinterpret_cast<ma_node*>(&voice.m_OcclusionFilter);
  }

  // send splitter only when the mixer has environment aux buses or the event automates a send to an aux bus it has
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

    for (const auto& automation : desc.m_Automations)
    {
      if (automation.m_eTarget == plAudioEventAutomationTarget::SendLevel && m_pBusGraph->GetAuxBus(automation.m_sBus) != nullptr)
      {
        bCreateSplitter = true;
        break;
      }
    }
  }

  if (bCreateSplitter)
  {
    ma_splitter_node_config splitCfg = ma_splitter_node_config_init(ma_engine_get_channels(m_pEngine));
    splitCfg.outputBusCount = 1 + MaxEnvironmentSends;

    if (ma_splitter_node_init(ma_engine_get_node_graph(m_pEngine), &splitCfg, &m_pEngine->allocationCallbacks, &voice.m_EnvSplitter) == MA_SUCCESS)
    {
      voice.m_bEnvSplitterValid = true;
      ma_node* pSplitter = reinterpret_cast<ma_node*>(&voice.m_EnvSplitter);
      ma_node_attach_output_bus(pCurrent, 0, pSplitter, 0);
      ma_node_attach_output_bus(pSplitter, 0, pTarget, 0); // dry

      // the wet outputs start silent; Update() routes and levels them
      for (plUInt32 uiSend = 0; uiSend < MaxEnvironmentSends; ++uiSend)
      {
        ma_node_set_output_bus_volume(pSplitter, 1 + uiSend, 0.0f);
      }

      pCurrent = nullptr; // chain fully wired through splitter
    }
  }

  if (pCurrent != nullptr)
  {
    ma_node_attach_output_bus(pCurrent, 0, pTarget, 0);
  }

  const float fPitchNow = voice.m_fBasePitch * m_fCurrentPitchMultiplier;
  ma_sound_set_volume(&voice.m_Sound, GetVoiceVolume(voice) * m_fCurrentVolumeMultiplier * m_fCurrentSpatialGain);
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
  // the gain the voice is heard with: the distance curve (0 at and beyond the max distance, so the voice is virtual
  // whatever the budget), occlusion, the cone, the instance's modulation and the mixer
  const float fBusGain = (m_pBusGraph != nullptr) ? m_pBusGraph->GetEffectiveBusGain(m_uiBus) : 1.0f;

  return ComputeSpatialGain() * ComputeConeGain() * m_fCurrentVolumeMultiplier * fBaseVolume * fBusGain;
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

  // the timeline continues from where the device thread last read the data (or from the start, when it has not begun)
  voice.m_fVirtualCursor = (voice.m_bSourceValid && voice.m_uiStartFrame <= uiNow) ? static_cast<double>(static_cast<plInt64>(voice.m_Source.m_iPublishedCursor)) : 0.0;
  voice.m_uiVirtualCursorFrame = plMath::Max(uiNow, voice.m_uiStartFrame);

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
    // the cursor integrated over the pitch history up to now, carried on to the resume frame at the current pitch
    AdvanceVirtualCursor(voice, uiNow);

    const ma_uint64 uiCursorFrom = plMath::Max(uiNow, voice.m_uiStartFrame);
    double fCursor = voice.m_fVirtualCursor;
    if (uiResume > uiCursorFrom)
    {
      fCursor += static_cast<double>(uiResume - uiCursorFrom) * GetSourceFramesPerEngineFrame(voice);
    }
    uiCursor = static_cast<ma_uint64>(fCursor + 0.5);

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
  voice.m_uiEndFrame = 0;
  voice.m_uiEndFadeFrames = 0;
  voice.m_uiOffsetFrames = 0;
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

  if (voice.m_pSpatializerNode != nullptr)
  {
    voice.m_pSpatializer->DestroyVoiceNode(voice.m_pSpatializerNode);
    voice.m_pSpatializerNode = nullptr;
    voice.m_pSpatializer = nullptr;
  }

  for (ma_sound_group*& pAux : voice.m_EnvAux)
  {
    pAux = nullptr;
  }

  if (voice.m_bOcclusionLpfValid)
  {
    plMiniAudioDsp::UninitFilterNode(&voice.m_OcclusionFilter);
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

  // the music player reads the resource's desc
  m_Music.Reset();

  if (m_pResource != nullptr)
  {
    plResourceManager::EndAcquireResource(const_cast<plAudioEventResource*>(m_pResource));
    m_pResource = nullptr;
  }
}

namespace
{
  /// The last node of a voice's chain, whose output 0 goes to the bus: sound -> [occlusion LPF] -> [env splitter].
  template <typename VOICE>
  ma_node* GetVoiceTail(VOICE& ref_voice)
  {
    if (ref_voice.m_bEnvSplitterValid)
      return reinterpret_cast<ma_node*>(&ref_voice.m_EnvSplitter);

    if (ref_voice.m_bOcclusionLpfValid)
      return reinterpret_cast<ma_node*>(&ref_voice.m_OcclusionFilter);

    if (ref_voice.m_pSpatializerNode != nullptr)
      return ref_voice.m_pSpatializerNode;

    if (ref_voice.m_bSoundValid)
      return reinterpret_cast<ma_node*>(&ref_voice.m_Sound);

    return nullptr;
  }
} // namespace

void plPlasmaAudioEventInstance::DetachFromBusGraph()
{
  for (Voice& voice : m_Voices)
  {
    ma_node* pTail = GetVoiceTail(voice);
    if (pTail == nullptr)
      continue;

    ma_node_detach_output_bus(pTail, 0);

    if (voice.m_bEnvSplitterValid)
    {
      for (plUInt32 uiSend = 0; uiSend < MaxEnvironmentSends; ++uiSend)
      {
        ma_node_detach_output_bus(pTail, 1 + uiSend);
        voice.m_EnvAux[uiSend] = nullptr;
      }
    }
  }
}

void plPlasmaAudioEventInstance::ReattachToBusGraph()
{
  if (m_pBusGraph == nullptr || m_pEngine == nullptr)
    return;

  // the output bus may have another index in the rebuilt graph
  if (m_pBusGraph->IsBuilt() && m_pResource != nullptr)
  {
    m_uiBus = m_pBusGraph->ResolveBus(m_pResource->GetDescriptor().m_sOutputBus);
  }
  else
  {
    m_uiBus = 0;
  }

  ma_node* pTarget = m_pBusGraph->IsBuilt() ? reinterpret_cast<ma_node*>(m_pBusGraph->GetBusInput(m_uiBus)) : nullptr;
  if (pTarget == nullptr)
  {
    pTarget = ma_node_graph_get_endpoint(ma_engine_get_node_graph(m_pEngine));
  }

  for (Voice& voice : m_Voices)
  {
    if (ma_node* pTail = GetVoiceTail(voice))
    {
      ma_node_attach_output_bus(pTail, 0, pTarget, 0);
    }
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
    if (bImmediate || !voice.m_bSoundValid || fadeOut <= plTime::MakeZero() || voice.m_uiStartFrame > uiNow || voice.m_bVirtual)
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

  SetParameterTarget(uiParam, fValue);
  m_ParameterLocal[uiParam] = true;
}

void plPlasmaAudioEventInstance::ResetParameter(plStringView sName)
{
  if (m_pResource == nullptr)
    return;

  const auto& desc = m_pResource->GetDescriptor();
  const plUInt32 uiParam = desc.FindParameterByName(sName);

  if (uiParam == plInvalidIndex || uiParam >= m_ParameterValues.GetCount())
    return;

  // the default now; a global parameter of the same name takes over below and in every Update
  SetParameterTarget(uiParam, desc.m_Parameters[uiParam].m_fDefault);
  m_ParameterLocal[uiParam] = false;
  ApplyGlobalParameters();
}

void plPlasmaAudioEventInstance::KeyOff()
{
  if (m_pResource == nullptr || m_pEngine == nullptr)
    return;

  const auto& desc = m_pResource->GetDescriptor();
  if (m_uiReleasedSustains >= desc.m_SustainPoints.GetCount())
    return;

  // the timeline went on past the sustain point while held: the part behind it resumes from the next schedulable frame
  const ma_uint64 uiSustainFrame = m_uiTimelineOriginFrame + ToFrames(desc.m_SustainPoints[m_uiReleasedSustains]);
  const ma_uint64 uiResumeFrame = ma_engine_get_time_in_pcm_frames(m_pEngine) + plPlasmaAudioEventManager::GetLookAheadFrames(m_pEngine);

  if (uiResumeFrame > uiSustainFrame)
  {
    const ma_uint64 uiHeldFrames = uiResumeFrame - uiSustainFrame;
    m_uiTimelineOriginFrame += uiHeldFrames;

    for (plUInt32 i = 0; i < m_Instruments.GetCount(); ++i)
    {
      if (IsWaitingForKeyOff(i) && m_Instruments[i].m_bPendingTrigger)
      {
        m_Instruments[i].m_uiNextTriggerFrame += uiHeldFrames;
      }
    }
  }

  ++m_uiReleasedSustains;
  Log(plAudioLogEventType::KeyOff);
}

plTime plPlasmaAudioEventInstance::GetTimelineLength() const
{
  if (m_pResource == nullptr)
    return plTime::MakeZero();

  const auto& desc = m_pResource->GetDescriptor();
  double fEnd = 0.0;

  for (plUInt32 i = 0; i < desc.m_Instruments.GetCount(); ++i)
  {
    const auto& instrumentDesc = desc.m_Instruments[i];
    if (instrumentDesc.m_bMute)
      continue;

    double fLongest = 0.0;
    for (plUInt32 uiEntry = 0; uiEntry < instrumentDesc.m_Playlist.GetCount(); ++uiEntry)
    {
      plUInt64 uiFrames = 0;
      plUInt32 uiSampleRate = 0;

      if (const plAudioClipResourceHandle& hClip = m_pResource->GetClip(i, uiEntry); hClip.IsValid())
      {
        // PointerOnly does not wait; a clip that is not loaded yet counts as zero
        plAudioClipResource* pClip = plResourceManager::BeginAcquireResource(hClip, plResourceAcquireMode::PointerOnly);
        if (pClip->GetLoadingState() == plResourceState::Loaded)
        {
          uiFrames = pClip->GetFrames();
          uiSampleRate = pClip->GetSampleRate();
        }
        plResourceManager::EndAcquireResource(pClip);
      }
      else if (instrumentDesc.m_Playlist[uiEntry].m_sEvent.IsEmpty())
      {
        m_pResource->GetSampleLength(instrumentDesc.m_Playlist[uiEntry].m_uiSample, uiFrames, uiSampleRate);
      }

      if (uiSampleRate > 0)
        fLongest = plMath::Max(fLongest, static_cast<double>(uiFrames) / uiSampleRate);
    }

    if (fLongest > 0.0)
      fEnd = plMath::Max(fEnd, instrumentDesc.m_StartDelay.GetSeconds() + fLongest);
  }

  return plTime::MakeFromSeconds(fEnd);
}

void plPlasmaAudioEventInstance::GetSustainState(plUInt32& out_uiSustainsLeft, bool& out_bHolding) const
{
  out_uiSustainsLeft = 0;
  out_bHolding = false;

  if (m_pResource == nullptr || m_pEngine == nullptr)
    return;

  const auto& desc = m_pResource->GetDescriptor();
  if (m_uiReleasedSustains >= desc.m_SustainPoints.GetCount())
    return;

  out_uiSustainsLeft = desc.m_SustainPoints.GetCount() - m_uiReleasedSustains;

  // GetTimelinePosition stops at the sustain point; the engine time tells whether the playhead got there
  const ma_uint64 uiSustainFrame = m_uiTimelineOriginFrame + ToFrames(desc.m_SustainPoints[m_uiReleasedSustains]);
  out_bHolding = ma_engine_get_time_in_pcm_frames(m_pEngine) >= uiSustainFrame;
}

void plPlasmaAudioEventInstance::OnChildFinished(plUInt32 uiInstrument, plRandom& ref_rng)
{
  if (m_pResource == nullptr || uiInstrument >= m_Instruments.GetCount())
    return;

  InstrumentState& state = m_Instruments[uiInstrument];
  const auto& instrumentDesc = m_pResource->GetDescriptor().m_Instruments[uiInstrument];

  state.m_bChildPlaying = false;

  if (instrumentDesc.m_bLoop && !m_bStopping && m_pEngine != nullptr)
  {
    const ma_uint64 uiNow = ma_engine_get_time_in_pcm_frames(m_pEngine);
    state.m_uiNextTriggerFrame = uiNow + plPlasmaAudioEventManager::GetLookAheadFrames(m_pEngine) + RollRetriggerGap(instrumentDesc, ref_rng, 0.0);
    state.m_bPendingTrigger = true;
  }
  else
  {
    state.m_bDone = true;
  }
}

void plPlasmaAudioEventInstance::PassMarkers()
{
  const auto& desc = m_pResource->GetDescriptor();
  if (desc.m_Markers.IsEmpty() || m_pEventData == nullptr)
    return;

  const double fPosition = GetTimelinePosition();

  auto passRange = [&](double fFrom, double fTo)
  {
    for (const plAudioEventMarkerDesc& marker : desc.m_Markers)
    {
      const double fTime = marker.m_Time.GetSeconds();
      if (fTime >= fFrom && fTime < fTo)
      {
        m_pEventData->m_PassedMarkers.PushBack(marker.m_sName);
        Log(plAudioLogEventType::Marker, marker.m_sName);
      }
    }
  };

  if (fPosition >= m_fMarkerPosition)
  {
    passRange(m_fMarkerPosition, fPosition);
    m_fMarkerPosition = fPosition;
  }
  else if (IsRegionLooping())
  {
    // the region wrapped: the rest of the last pass, then the region start up to the position
    passRange(m_fMarkerPosition, desc.m_LoopEnd.GetSeconds());
    passRange(desc.m_LoopStart.GetSeconds(), fPosition);
    m_fMarkerPosition = fPosition;
  }

  // otherwise a key-off moved the timeline back by the look-ahead to resume behind the sustain point: the markers up
  // to the old position were passed already
}

void plPlasmaAudioEventInstance::ApplyModulation()
{
  const auto& desc = m_pResource->GetDescriptor();

  float fVolume = 1.0f;
  float fPitch = 1.0f;
  float fCutoff = 22000.0f;

  for (InstrumentState& state : m_Instruments)
  {
    state.m_fAutomationGain = 1.0f;
  }

  for (plUInt32 uiAutomation = 0; uiAutomation < desc.m_Automations.GetCount(); ++uiAutomation)
  {
    const auto& automation = desc.m_Automations[uiAutomation];

    if (automation.m_uiParameter >= m_ParameterValues.GetCount() || automation.m_Curve.IsEmpty())
      continue;

    const float fParamValue = m_ParameterValues[automation.m_uiParameter];
    const float fCurveValue = static_cast<float>(automation.m_Curve.Evaluate(fParamValue));

    if (uiAutomation < m_AutomationValues.GetCount())
      m_AutomationValues[uiAutomation] = fCurveValue;

    switch (automation.m_eTarget)
    {
      case plAudioEventAutomationTarget::EventVolume:
        fVolume *= plMath::Max(fCurveValue, 0.0f);
        break;
      case plAudioEventAutomationTarget::EventPitch:
        fPitch *= plMath::Clamp(fCurveValue, 0.01f, 10.0f);
        break;
      case plAudioEventAutomationTarget::InstrumentVolume:
        if (automation.m_uiInstrument < m_Instruments.GetCount())
          m_Instruments[automation.m_uiInstrument].m_fAutomationGain *= plMath::Max(fCurveValue, 0.0f);
        break;
      case plAudioEventAutomationTarget::FilterCutoff:
        fCutoff = plMath::Min(fCutoff, plMath::Clamp(fCurveValue, 20.0f, 22000.0f));
        break;
      case plAudioEventAutomationTarget::SendLevel:
        // read per voice by the send routing in Update
        break;
    }
  }

  m_fAutomationCutoff = fCutoff;

  if (m_pEntity != nullptr)
  {
    // obstruction scales the direct signal down
    fVolume *= 1.0f - 0.85f * plMath::Clamp(m_pEntity->m_fCurrentObstruction, 0.0f, 1.0f);

    // per-entity multipliers (legacy component volume / pitch)
    fVolume *= plMath::Max(m_pEntity->m_fVolumeMultiplier, 0.0f);
    fPitch *= plMath::Clamp(m_pEntity->m_fPitchMultiplier, 0.01f, 10.0f);
  }

  // the distance curve fades to silence toward the max distance, where the audibility reaches 0 and the voice is
  // virtualized; occlusion lowers the volume as configured
  m_fCurrentSpatialGain = ComputeSpatialGain();

  // instruments placed on a parameter range follow that parameter
  for (plUInt32 i = 0; i < m_Instruments.GetCount() && i < desc.m_Instruments.GetCount(); ++i)
  {
    const auto& instrumentDesc = desc.m_Instruments[i];
    const plUInt32 uiParam = instrumentDesc.m_uiRangeParameter;
    m_Instruments[i].m_fRangeGain = uiParam < m_ParameterValues.GetCount() ? instrumentDesc.GetRangeGain(m_ParameterValues[uiParam]) : 1.0f;
  }

  m_fCurrentVolumeMultiplier = fVolume;
  m_fCurrentPitchMultiplier = fPitch;
}

void plPlasmaAudioEventInstance::AddAutomationSends(plHybridArray<AuxSend, MaxEnvironmentSends>& inout_sends)
{
  const auto& desc = m_pResource->GetDescriptor();

  for (plUInt32 uiAutomation = 0; uiAutomation < desc.m_Automations.GetCount() && uiAutomation < m_AutomationValues.GetCount(); ++uiAutomation)
  {
    const auto& automation = desc.m_Automations[uiAutomation];

    if (automation.m_eTarget != plAudioEventAutomationTarget::SendLevel)
      continue;

    ma_sound_group* pAux = m_pBusGraph->GetAuxBus(automation.m_sBus);
    const float fLevel = plMath::Clamp(m_AutomationValues[uiAutomation], 0.0f, 1.0f);

    if (pAux == nullptr || fLevel <= 0.0001f)
      continue;

    bool bMerged = false;
    for (AuxSend& send : inout_sends)
    {
      if (send.m_pAux == pAux)
      {
        send.m_fLevel = plMath::Min(send.m_fLevel + fLevel, 1.0f);
        bMerged = true;
        break;
      }
    }

    if (bMerged)
      continue;

    if (inout_sends.GetCount() < MaxEnvironmentSends)
    {
      inout_sends.PushBack({pAux, fLevel});
      continue;
    }

    plUInt32 uiWeakest = 0;
    for (plUInt32 i = 1; i < inout_sends.GetCount(); ++i)
    {
      if (inout_sends[i].m_fLevel < inout_sends[uiWeakest].m_fLevel)
        uiWeakest = i;
    }

    if (inout_sends[uiWeakest].m_fLevel < fLevel)
      inout_sends[uiWeakest] = {pAux, fLevel};
  }
}

double plPlasmaAudioEventInstance::GetTimelinePosition() const
{
  if (m_pEngine == nullptr)
    return 0.0;

  const ma_uint64 uiNow = ma_engine_get_time_in_pcm_frames(m_pEngine);
  double fPosition = (static_cast<double>(uiNow) - static_cast<double>(m_uiTimelineOriginFrame)) / m_uiSampleRate;

  // a looping region moves the origin to its next pass one look-ahead span early; until then the previous pass plays
  if (m_pResource != nullptr && IsRegionLooping())
  {
    const auto& desc = m_pResource->GetDescriptor();
    if (fPosition < desc.m_LoopStart.GetSeconds())
    {
      fPosition += (desc.m_LoopEnd - desc.m_LoopStart).GetSeconds();
    }
  }

  // the playhead waits at a sustain point until the key-off
  if (m_pResource != nullptr && m_uiReleasedSustains < m_pResource->GetDescriptor().m_SustainPoints.GetCount())
  {
    fPosition = plMath::Min(fPosition, m_pResource->GetDescriptor().m_SustainPoints[m_uiReleasedSustains].GetSeconds());
  }

  return plMath::Max(fPosition, 0.0);
}

bool plPlasmaAudioEventInstance::IsInstrumentSounding(plUInt32 uiInstrument) const
{
  // a nested event the instrument started sounds for it
  if (uiInstrument < m_Instruments.GetCount() && m_Instruments[uiInstrument].m_bChildPlaying)
    return true;

  for (plUInt32 uiSlot = 0; uiSlot < VoiceSlotsPerInstrument; ++uiSlot)
  {
    const plUInt32 uiVoice = uiInstrument * VoiceSlotsPerInstrument + uiSlot;
    if (uiVoice < m_Voices.GetCount() && m_Voices[uiVoice].m_bActive)
      return true;
  }

  return false;
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
    entry.m_sEntity = (m_pEntity != nullptr) ? m_pEntity->m_sName.GetView() : plStringView();
    entry.m_uiState = m_bStopping ? plAudioVoiceSnapshotEntry::Stopping : (voice.m_bVirtual ? plAudioVoiceSnapshotEntry::Virtual : plAudioVoiceSnapshotEntry::Real);
    entry.m_uiPriority = uiPriority;
    entry.m_uiCodec = plAudioVoiceSnapshotEntry::Embedded;
    entry.m_uiLoadMode = plAudioVoiceSnapshotEntry::Embedded;
    entry.m_fAudibility = m_fAudibility;
    entry.m_fDistance = m_fDistance;
    entry.m_fOcclusion = fOcclusion;
    entry.m_fVolume = m_fCurrentVolumeMultiplier * m_fCurrentSpatialGain * GetVoiceVolume(voice);
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

bool plPlasmaAudioEventInstance::Update(plTime delta, plArrayPtr<const plVec3> listeners, plRandom& ref_sharedRng)
{
  if (m_pResource == nullptr)
    return false;

  plRandom& ref_rng = m_bSeeded ? m_SeededRng : ref_sharedRng;

  m_Elapsed += delta;
  SelectListener(listeners);

  const auto& desc = m_pResource->GetDescriptor();

  const ma_uint64 uiNow = ma_engine_get_time_in_pcm_frames(m_pEngine);
  const ma_uint64 uiHorizon = uiNow + plPlasmaAudioEventManager::GetLookAheadFrames(m_pEngine);

  // one pass is known once the clips it measures loaded
  if (m_bRequestedLimit && m_uiStopFrame == 0)
  {
    if (const plTime length = GetTimelineLength(); length.IsPositive())
      m_uiStopFrame = m_uiTimelineOriginFrame + plMath::Max<ma_uint64>(ToFrames(length), 1);
  }

  // a limited nested event ran its length: it ends with its stop fade (from the first update at or after the frame)
  if (m_uiStopFrame != 0 && !m_bStopping && uiNow >= m_uiStopFrame)
  {
    Stop(false);
  }

  // virtual timelines advance at the pitch that applied since the last update, before this update changes it
  for (auto& voice : m_Voices)
  {
    if (voice.m_bActive && voice.m_bVirtual)
    {
      AdvanceVirtualCursor(voice, uiNow);
    }
  }

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
  float fDistance = 0.0f;
  if (GetListenerDistance(fDistance))
  {
    const plUInt32 uiDistanceParam = desc.FindParameterByName("Distance");
    if (uiDistanceParam != plInvalidIndex && uiDistanceParam < m_ParameterValues.GetCount())
    {
      // the built-in distance does not seek
      m_ParameterValues[uiDistanceParam] = plMath::Clamp(fDistance, desc.m_Parameters[uiDistanceParam].m_fMin, desc.m_Parameters[uiDistanceParam].m_fMax);
      m_ParameterTargets[uiDistanceParam] = m_ParameterValues[uiDistanceParam];
    }
  }

  SeekParameters(delta);
  ApplyModulation();

  const float fOcclusionCutoff = GetOcclusionCutoff();

  // the environments with the largest amounts (each zone's falloff), blended: overlapping zones share the send level
  plHybridArray<AuxSend, MaxEnvironmentSends> environmentSends;
  if (m_pEntity != nullptr && m_pBusGraph != nullptr && m_pBusGraph->IsBuilt())
  {
    plHybridArray<AuxSend, 4> candidates;
    for (const auto& env : m_pEntity->m_Environments)
    {
      if (env.m_fAmount <= 0.0001f)
        continue;

      if (ma_sound_group* pAux = m_pBusGraph->GetEnvironmentAuxBus(env.m_sName))
      {
        candidates.PushBack({pAux, plMath::Clamp(env.m_fAmount, 0.0f, 1.0f)});
      }
    }

    candidates.Sort([](const AuxSend& a, const AuxSend& b)
      { return a.m_fLevel > b.m_fLevel; });

    float fTotal = 0.0f;
    for (plUInt32 i = 0; i < candidates.GetCount() && i < MaxEnvironmentSends; ++i)
    {
      environmentSends.PushBack(candidates[i]);
      fTotal += candidates[i].m_fLevel;
    }

    for (AuxSend& send : environmentSends)
    {
      send.m_fLevel /= plMath::Max(fTotal, 1.0f);
    }
  }

  if (m_pBusGraph != nullptr && m_pBusGraph->IsBuilt())
  {
    AddAutomationSends(environmentSends);
  }

  // the music starts and cuts its stems before the voices are looked at
  if (m_Music.IsStarted() && !m_bStopping)
  {
    UpdateMusic(uiNow, uiHorizon, ref_rng);
  }

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
          const plVec3 vDir = m_pEntity->m_LastTransform.m_vForward;
          ma_sound_set_position(&voice.m_Sound, vPos.x, vPos.y, vPos.z);
          ma_sound_set_velocity(&voice.m_Sound, vVel.x, vVel.y, vVel.z);
          ma_sound_set_direction(&voice.m_Sound, vDir.x, vDir.y, vDir.z);
          UpdateSpatializer(voice);
        }

        if (!m_bStopping)
        {
          ma_sound_set_volume(&voice.m_Sound, GetVoiceVolume(voice) * m_fCurrentVolumeMultiplier * m_fCurrentSpatialGain);
          ma_sound_set_pitch(&voice.m_Sound, voice.m_fBasePitch * m_fCurrentPitchMultiplier);
        }

        // occlusion low-pass; unchanged cutoffs are not passed on
        if (voice.m_bOcclusionLpfValid)
        {
          plMiniAudioDsp::SetFilter(&voice.m_OcclusionFilter, fOcclusionCutoff, 2, 0.0f, 1.0f);
        }

        // environment sends
        if (voice.m_bEnvSplitterValid && m_pBusGraph != nullptr)
        {
          ma_node* pSplitter = reinterpret_cast<ma_node*>(&voice.m_EnvSplitter);

          for (plUInt32 uiSend = 0; uiSend < MaxEnvironmentSends; ++uiSend)
          {
            if (uiSend >= environmentSends.GetCount())
            {
              ma_node_set_output_bus_volume(pSplitter, 1 + uiSend, 0.0f);
              continue;
            }

            // re-attaching is only needed when the environment of this send changed
            ma_sound_group* pAux = environmentSends[uiSend].m_pAux;
            if (voice.m_EnvAux[uiSend] != pAux)
            {
              ma_node_attach_output_bus(pSplitter, 1 + uiSend, reinterpret_cast<ma_node*>(pAux), 0);
              voice.m_EnvAux[uiSend] = pAux;
            }

            ma_node_set_output_bus_volume(pSplitter, 1 + uiSend, environmentSends[uiSend].m_fLevel);
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

  PassMarkers();

  // audibility for the voice ranking, from the loudest voice of the instance
  float fMaxBaseVolume = 0.0f;
  bool bAnyVoice = false;
  for (const auto& voice : m_Voices)
  {
    if (voice.m_bActive)
    {
      fMaxBaseVolume = plMath::Max(fMaxBaseVolume, GetVoiceVolume(voice));
      bAnyVoice = true;
    }
  }

  m_fDistance = (m_pEntity != nullptr) ? (m_pEntity->m_LastTransform.m_vPosition - m_vListenerPos).GetLength() : 0.0f;
  m_fAudibility = ComputeAudibility(bAnyVoice ? fMaxBaseVolume : 1.0f);

  // nested events keep their parent alive
  const bool bAnyChild = m_uiLiveChildren > 0 || !m_ChildRequests.IsEmpty();

  // music plays until it is stopped
  const bool bMusic = m_Music.IsStarted() && !m_bStopping;

  return bAnyActive || bAnyPending || bHoldOpen || bAnyChild || bMusic || HasMarkersAhead();
}

// ---------------------------------------------------------------------------------------------
// Music
// ---------------------------------------------------------------------------------------------

namespace
{
  /// A cut in the middle of a stem fades over at least this long, so it does not click.
  const plTime s_MusicCutMinFade = plTime::MakeFromMilliseconds(3);
} // namespace

void plPlasmaAudioEventInstance::SetMusicState(plStringView sState)
{
  if (!m_Music.IsStarted() || m_pResource == nullptr)
    return;

  const auto& desc = m_pResource->GetDescriptor();
  const plUInt32 uiState = desc.m_Music.FindState(sState);
  if (uiState == plInvalidIndex)
    return;

  // through the state parameter, which the music follows; without one, straight to the player
  if (m_uiMusicStateParameter < desc.m_Parameters.GetCount())
    SetParameter(desc.m_Parameters[m_uiMusicStateParameter].m_sName, static_cast<float>(uiState));
  else
    m_Music.RequestState(static_cast<plUInt16>(uiState));
}

void plPlasmaAudioEventInstance::PlayStinger(plStringView sStinger)
{
  if (!m_Music.IsStarted() || m_pResource == nullptr)
    return;

  const plUInt32 uiStinger = m_pResource->GetDescriptor().m_Music.FindStinger(sStinger);
  if (uiStinger != plInvalidIndex)
    m_Music.RequestStinger(uiStinger);
}

plAudioMusicPlayer::Position plPlasmaAudioEventInstance::GetMusicPosition() const
{
  if (!m_Music.IsStarted() || m_pEngine == nullptr)
    return plAudioMusicPlayer::Position();

  return m_Music.GetPosition(ma_engine_get_time_in_pcm_frames(m_pEngine));
}

void plPlasmaAudioEventInstance::UpdateMusic(ma_uint64 uiNow, ma_uint64 uiHorizon, plRandom& ref_rng)
{
  // the state the game asks for: the value it set, not the one seeking toward it
  if (m_uiMusicStateParameter < m_ParameterTargets.GetCount())
  {
    m_Music.RequestState(static_cast<plUInt16>(plMath::Max(plMath::Round(m_ParameterTargets[m_uiMusicStateParameter]), 0.0f)));
  }

  m_MusicPlays.Clear();
  m_MusicCuts.Clear();
  m_Music.Update(uiNow, uiHorizon, m_MusicPlays, m_MusicCuts);

  // the cuts end what plays before the change; the plays that start there may use the same instruments
  for (const plAudioMusicPlayer::StemCut& cut : m_MusicCuts)
    CutMusicStem(cut);

  for (const plAudioMusicPlayer::StemPlay& play : m_MusicPlays)
    StartMusicStem(play, ref_rng);

  m_MusicMessages.Clear();
  m_Music.TakeMessages(uiNow, m_MusicMessages);

  for (const plAudioMusicPlayer::Message& message : m_MusicMessages)
  {
    plStringBuilder sMarker;

    switch (message.m_Type)
    {
      case plAudioMusicPlayer::Message::Type::Beat:
        // the audio trigger component turns these into plMsgAudioBeat
        sMarker.SetFormat("{}{}:{}:{}", plAudioSystemReservedMarker::BeatPrefix, message.m_uiBar, message.m_uiBeat, message.m_sText);
        break;

      case plAudioMusicPlayer::Message::Type::Stinger:
        sMarker.SetFormat("Stinger: {}", message.m_sText);
        Log(plAudioLogEventType::Marker, sMarker);
        break;

      default:
        sMarker = message.m_sText;
        Log(plAudioLogEventType::Marker, sMarker);
        break;
    }

    if (m_pEventData != nullptr)
      m_pEventData->m_PassedMarkers.PushBack(sMarker);
  }

  // an editor or a game that stopped taking the markers does not grow the list forever
  if (m_pEventData != nullptr && m_pEventData->m_PassedMarkers.GetCount() > 256)
    m_pEventData->m_PassedMarkers.RemoveAtAndCopy(0, m_pEventData->m_PassedMarkers.GetCount() - 256);
}

void plPlasmaAudioEventInstance::StartMusicStem(const plAudioMusicPlayer::StemPlay& play, plRandom& ref_rng)
{
  const auto& desc = m_pResource->GetDescriptor();
  if (play.m_uiInstrument >= desc.m_Instruments.GetCount())
    return;

  const auto& instrumentDesc = desc.m_Instruments[play.m_uiInstrument];
  if (instrumentDesc.m_bMute || instrumentDesc.m_Playlist.IsEmpty())
    return;

  // a free slot, else the slot of the voice that started first (a third voice of a stem in quick changes)
  plUInt32 uiSlot = plInvalidIndex;
  for (plUInt32 uiCandidate = 0; uiCandidate < VoiceSlotsPerInstrument; ++uiCandidate)
  {
    const Voice& voice = GetVoice(play.m_uiInstrument, uiCandidate);
    if (!voice.m_bActive)
    {
      uiSlot = uiCandidate;
      break;
    }

    if (uiSlot == plInvalidIndex || voice.m_uiStartFrame < GetVoice(play.m_uiInstrument, uiSlot).m_uiStartFrame)
      uiSlot = uiCandidate;
  }

  Voice& voice = GetVoice(play.m_uiInstrument, uiSlot);
  if (voice.m_bActive)
    StopVoice(voice);

  InstrumentState& state = m_Instruments[play.m_uiInstrument];
  state.m_bDone = false;

  StartVoice(play.m_uiInstrument, uiSlot, ref_rng, play.m_uiStartFrame, play.m_uiOffsetFrames, play.m_uiEndFrame);

  // a stem whose clip still loads does not start late off the beat grid: it waits for its next segment
  state.m_bPendingTrigger = false;
  if (!voice.m_bActive)
    state.m_bDone = true;
}

void plPlasmaAudioEventInstance::CutMusicStem(const plAudioMusicPlayer::StemCut& cut)
{
  if (cut.m_uiInstrument >= m_Instruments.GetCount())
    return;

  const ma_uint64 uiMinFade = ToFrames(s_MusicCutMinFade);

  for (plUInt32 uiSlot = 0; uiSlot < VoiceSlotsPerInstrument; ++uiSlot)
  {
    Voice& voice = GetVoice(cut.m_uiInstrument, uiSlot);
    if (!voice.m_bActive)
      continue;

    const ma_uint64 uiDataStart = voice.m_uiStartFrame + voice.m_uiOffsetFrames;

    // a voice that would only begin at the cut never sounds
    if (uiDataStart >= cut.m_uiFrame)
    {
      StopVoice(voice);
      continue;
    }

    // a voice that ends before the cut anyway keeps its end
    if (voice.m_uiEndFrame != 0 && voice.m_uiEndFrame <= cut.m_uiFrame)
      continue;

    const ma_uint64 uiFade = plMath::Min(plMath::Max(cut.m_uiFadeFrames, uiMinFade), cut.m_uiFrame - uiDataStart);
    SetVoiceEnd(voice, cut.m_uiFrame, uiFade);
  }
}

void plPlasmaAudioEventInstance::SetVoiceEnd(Voice& ref_voice, ma_uint64 uiEndFrame, ma_uint64 uiFadeFrames)
{
  ref_voice.m_uiEndFrame = uiEndFrame;
  ref_voice.m_uiEndFadeFrames = uiFadeFrames;

  // the fade first: the device thread may read the new end before the new fade otherwise
  if (ref_voice.m_bSourceValid)
  {
    ref_voice.m_Source.m_iEndFadeFrames = static_cast<plInt64>(uiFadeFrames);
    ref_voice.m_Source.m_iEndFrame = static_cast<plInt64>(uiEndFrame);
  }
}

bool plPlasmaAudioEventInstance::HasMarkersAhead() const
{
  if (m_bStopping)
    return false;

  for (const plAudioEventMarkerDesc& marker : m_pResource->GetDescriptor().m_Markers)
  {
    if (marker.m_Time.GetSeconds() >= m_fMarkerPosition)
      return true;
  }

  return false;
}

bool plPlasmaAudioEventInstance::StartPendingTrigger(plUInt32 uiInstrument, ma_uint64 uiHorizon, plRandom& ref_rng)
{
  auto& state = m_Instruments[uiInstrument];

  if (!state.m_bPendingTrigger || m_bStopping || state.m_uiNextTriggerFrame > uiHorizon || IsWaitingForKeyOff(uiInstrument))
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

  PL_LOCK(m_Mutex);
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

  // The resource manager holds its own mutex while it broadcasts; that mutex is always taken before this one (see the
  // class description). Waits for the audio thread's update of the instances, which never waits for the resource manager.
  PL_LOCK(m_Mutex);

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

namespace
{
  /// The loading state of a resource; PointerOnly never blocks or takes the resource manager's mutex.
  template <typename ResourceType>
  plResourceState GetLoadingState(const plTypedResourceHandle<ResourceType>& hResource)
  {
    ResourceType* pResource = plResourceManager::BeginAcquireResource(hResource, plResourceAcquireMode::PointerOnly);
    const plResourceState state = pResource->GetLoadingState();
    plResourceManager::EndAcquireResource(pResource);
    return state;
  }
} // namespace

void plPlasmaAudioDeferredLoads::Preload(const plAudioClipResourceHandle& hClip)
{
  if (!m_Preload.Contains(hClip))
  {
    m_Preload.PushBack(hClip);
  }
}

void plPlasmaAudioDeferredLoads::Reload(const plAudioClipResourceHandle& hClip)
{
  if (!m_Reload.Contains(hClip))
  {
    m_Reload.PushBack(hClip);
  }
}

plResult plPlasmaAudioEventManager::ActivateEvent(plStringView sEventResourceId, plPlasmaAudioEntityData* pEntity, plPlasmaAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine)
{
  const plAudioEventResourceHandle hEvent = plResourceManager::LoadResource<plAudioEventResource>(sEventResourceId);
  if (!hEvent.IsValid())
    return PL_FAILURE;

  const plResourceState state = GetLoadingState(hEvent);

  if (state == plResourceState::LoadedResourceMissing)
    return PL_FAILURE;

  if (state != plResourceState::Loaded)
  {
    // the audio thread does not wait for the file: the start is queued until the event is loaded
    plResourceManager::PreloadResource(hEvent);

    PendingActivation activation;
    activation.m_hEvent = hEvent;
    activation.m_sEventId = sEventResourceId;
    activation.m_pEntity = pEntity;
    activation.m_pEventData = pEventData;
    activation.m_pBusGraph = pBusGraph;
    activation.m_pEngine = pEngine;
    QueuePendingActivation(std::move(activation));
    return PL_SUCCESS;
  }

  const plResult result = ActivateEventInternal(hEvent, sEventResourceId, pEntity, pEventData, pBusGraph, pEngine);
  FlushDeferredLoads();
  return result;
}

void plPlasmaAudioEventManager::QueuePendingActivation(PendingActivation&& activation)
{
  activation.m_RequestTime = m_Now;
  activation.m_bLooping = activation.m_pEventData != nullptr && activation.m_pEventData->m_bLooping;

  PL_LOCK(m_Mutex);

  m_EventLog.Add(plAudioLogEventType::EventLoadWait, activation.m_sEventId, activation.m_bLooping ? "starts once loaded" : "starts once loaded, within 1 s");

  // the event data now waits for this activation; a start it was still waiting for is replaced
  if (activation.m_pEventData != nullptr)
  {
    for (plUInt32 i = m_PendingActivations.GetCount(); i > 0; --i)
    {
      if (m_PendingActivations[i - 1].m_pEventData == activation.m_pEventData)
      {
        m_PendingActivations.RemoveAtAndCopy(i - 1);
      }
    }
  }

  m_PendingActivations.PushBack(std::move(activation));
}

void plPlasmaAudioEventManager::ProcessPendingActivations()
{
  // Collected under the mutex, started outside it: starting may create a legacy event resource, which takes the
  // resource manager's mutex. Everything that changes the pending list runs on the audio thread, as this does.
  plHybridArray<PendingActivation, 4> ready;

  {
    PL_LOCK(m_Mutex);

    for (plUInt32 i = 0; i < m_PendingActivations.GetCount();)
    {
      PendingActivation& activation = m_PendingActivations[i];

      plResourceState state = plResourceState::Unloaded;
      if (activation.m_hLegacySound.IsValid())
      {
        state = GetLoadingState(activation.m_hLegacySound);
      }
      else
      {
        state = GetLoadingState(activation.m_hEvent);
      }

      const bool bExpired = !activation.m_bLooping && (m_Now - activation.m_RequestTime) > MaxPendingStartTime;

      if (state == plResourceState::Loaded || state == plResourceState::LoadedResourceMissing || bExpired)
      {
        if (state == plResourceState::Loaded)
        {
          ready.PushBack(std::move(activation));
        }
        else
        {
          m_EventLog.Add(plAudioLogEventType::EventDropped, activation.m_sEventId, bExpired ? "not loaded in time" : "event missing");
        }

        m_PendingActivations.RemoveAtAndCopy(i);
      }
      else
      {
        ++i;
      }
    }
  }

  for (PendingActivation& activation : ready)
  {
    plAudioEventResourceHandle hEvent = activation.m_hEvent;

    if (activation.m_hLegacySound.IsValid())
    {
      hEvent = GetLegacyEvent(activation.m_hLegacySound);
      if (!hEvent.IsValid())
        continue;
    }

    ActivateEventInternal(hEvent, hEvent.GetResourceID(), activation.m_pEntity, activation.m_pEventData, activation.m_pBusGraph, activation.m_pEngine).IgnoreResult();
  }
}

void plPlasmaAudioEventManager::FlushDeferredLoads()
{
  plPlasmaAudioDeferredLoads loads;

  {
    PL_LOCK(m_Mutex);
    if (m_DeferredLoads.IsEmpty())
      return;

    loads.m_Preload.Swap(m_DeferredLoads.m_Preload);
    loads.m_Reload.Swap(m_DeferredLoads.m_Reload);
  }

  // without the manager's mutex: these take the resource manager's mutex, and a reload unloads the clip, which calls
  // OnResourceEvent and takes the manager's mutex in turn
  for (const plAudioClipResourceHandle& hClip : loads.m_Reload)
  {
    if (plResourceManager::ReloadResource(hClip, true))
    {
      plResourceManager::PreloadResource(hClip);
    }
  }

  for (const plAudioClipResourceHandle& hClip : loads.m_Preload)
  {
    plResourceManager::PreloadResource(hClip);
  }
}

plVec3 plPlasmaAudioEventManager::GetClosestListener(const plPlasmaAudioEntityData* pEntity) const
{
  if (m_Listeners.IsEmpty())
    return plVec3::MakeZero();

  if (pEntity == nullptr || pEntity->m_bGlobalEntity)
    return m_Listeners[0];

  plVec3 vClosest = m_Listeners[0];
  float fClosest = plMath::MaxValue<float>();
  for (const plVec3& vListener : m_Listeners)
  {
    const float fDistanceSq = (pEntity->m_LastTransform.m_vPosition - vListener).GetLengthSquared();
    if (fDistanceSq < fClosest)
    {
      fClosest = fDistanceSq;
      vClosest = vListener;
    }
  }

  return vClosest;
}

plUInt32 plPlasmaAudioEventManager::GetPendingActivationCount() const
{
  PL_LOCK(m_Mutex);
  return m_PendingActivations.GetCount();
}

plResult plPlasmaAudioEventManager::ActivateLegacySound(const plMiniAudioSoundResourceHandle& hSound, plPlasmaAudioEntityData* pEntity, plPlasmaAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine)
{
  if (!hSound.IsValid())
    return PL_FAILURE;

  const plResourceState state = GetLoadingState(hSound);

  if (state == plResourceState::LoadedResourceMissing)
    return PL_FAILURE;

  if (state != plResourceState::Loaded)
  {
    plResourceManager::PreloadResource(hSound);

    PendingActivation activation;
    activation.m_hLegacySound = hSound;
    activation.m_sEventId = hSound.GetResourceID();
    activation.m_pEntity = pEntity;
    activation.m_pEventData = pEventData;
    activation.m_pBusGraph = pBusGraph;
    activation.m_pEngine = pEngine;
    QueuePendingActivation(std::move(activation));
    return PL_SUCCESS;
  }

  const plAudioEventResourceHandle hEvent = GetLegacyEvent(hSound);
  if (!hEvent.IsValid())
    return PL_FAILURE;

  const plResult result = ActivateEventInternal(hEvent, hEvent.GetResourceID(), pEntity, pEventData, pBusGraph, pEngine);
  FlushDeferredLoads();
  return result;
}

plAudioEventResourceHandle plPlasmaAudioEventManager::GetLegacyEvent(const plMiniAudioSoundResourceHandle& hSound)
{
  const plString sSoundId = hSound.GetResourceID();

  plAudioEventResourceHandle hEvent;
  if (!m_LegacyEvents.TryGetValue(sSoundId, hEvent))
  {
    // the sound is loaded (the caller checked); PointerOnly does not wait
    plResourceLock<plMiniAudioSoundResource> pSound(hSound, plResourceAcquireMode::PointerOnly);
    if (pSound->GetLoadingState() != plResourceState::Loaded || pSound->GetNumVariants() == 0)
      return {};

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

  return hEvent;
}

plResult plPlasmaAudioEventManager::ActivateEventInternal(const plAudioEventResourceHandle& hEvent, plStringView sEventResourceId, plPlasmaAudioEntityData* pEntity, plPlasmaAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine, const plPlasmaAudioEventInstance* pParent, const plPlasmaAudioEventInstance::ChildRequest* pRequest)
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

  PL_LOCK(m_Mutex);

  // The callers checked the event is loaded. While the mutex is held its content is not freed: the unload handler waits
  // for the mutex before the resource manager frees anything. The name is for log entries.
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
          // quietest at the listener: the audibility includes distance, occlusion and bus gains, not only the volume
          plPlasmaAudioEventInstance* pQuietest = groupInstances[0];
          for (auto* pInstance : groupInstances)
          {
            if (pInstance->GetAudibility() < pQuietest->GetAudibility())
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
  pInstance->m_pEventLog = &m_EventLog;
  pInstance->m_pDeferredLoads = &m_DeferredLoads;
  pInstance->m_vListenerPos = GetClosestListener(pEntity);
  pInstance->m_uiSerial = m_uiNextInstanceSerial++;

  if (pParent != nullptr && pRequest != nullptr)
  {
    pInstance->m_uiParentSerial = pParent->m_uiSerial;
    pInstance->m_uiParentInstrument = pRequest->m_uiInstrument;
    pInstance->m_uiNestingDepth = pParent->m_uiNestingDepth + 1;
    pInstance->m_uiRequestedOriginFrame = pRequest->m_uiOriginFrame;
    pInstance->m_bRequestedLimit = pRequest->m_bLimited;
    pInstance->m_RequestedLength = pRequest->m_Length;
  }

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
  PL_LOCK(m_Mutex);

  // a start that still waits for its event is simply dropped
  for (plUInt32 i = 0; i < m_PendingActivations.GetCount(); ++i)
  {
    if (m_PendingActivations[i].m_pEventData == pEventData)
    {
      m_EventLog.Add(plAudioLogEventType::Stop, m_PendingActivations[i].m_sEventId, "before it was loaded");
      m_PendingActivations.RemoveAtAndCopy(i);
      return PL_SUCCESS;
    }
  }

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

  PL_LOCK(m_Mutex);

  pEventData->m_pInstance = nullptr;

  // a waiting one-shot still starts, unbound; a waiting looping start would have nothing left to stop it
  for (plUInt32 i = m_PendingActivations.GetCount(); i > 0; --i)
  {
    PendingActivation& activation = m_PendingActivations[i - 1];
    if (activation.m_pEventData != pEventData)
      continue;

    if (activation.m_bLooping)
    {
      m_PendingActivations.RemoveAtAndCopy(i - 1);
    }
    else
    {
      activation.m_pEventData = nullptr;
    }
  }

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
  PL_LOCK(m_Mutex);

  for (plUInt32 i = m_PendingActivations.GetCount(); i > 0; --i)
  {
    if (m_PendingActivations[i - 1].m_pEntity == pEntity)
    {
      m_PendingActivations.RemoveAtAndCopy(i - 1);
    }
  }

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

void plPlasmaAudioEventManager::GetDebugSnapshot(plAudioDebugSnapshot& out_snapshot) const
{
  PL_LOCK(m_Mutex);

  out_snapshot.m_Listeners = m_Listeners;
  out_snapshot.m_uiRealVoices = GetRealVoiceCount();
  out_snapshot.m_uiVirtualVoices = GetVirtualVoiceCount();
  out_snapshot.m_uiMaxRealVoices = plAudioVoiceManager::GetMaxRealVoices();
  out_snapshot.m_Emitters.Clear();

  for (const auto& pInstance : m_Instances)
  {
    plAudioDebugEmitter emitter;
    if (pInstance->GetDebugEmitter(emitter))
    {
      out_snapshot.m_Emitters.PushBack(std::move(emitter));
    }
  }
}

void plPlasmaAudioEventManager::DetachFromBusGraph(const plMiniAudioBusGraph* pBusGraph)
{
  PL_LOCK(m_Mutex);

  for (auto& pInstance : m_Instances)
  {
    if (pInstance->GetBusGraph() == pBusGraph)
    {
      pInstance->DetachFromBusGraph();
    }
  }
}

void plPlasmaAudioEventManager::ReattachToBusGraph(const plMiniAudioBusGraph* pBusGraph)
{
  PL_LOCK(m_Mutex);

  for (auto& pInstance : m_Instances)
  {
    if (pInstance->GetBusGraph() == pBusGraph)
    {
      pInstance->ReattachToBusGraph();
    }
  }
}

void plPlasmaAudioEventManager::StopAll(bool bImmediate)
{
  PL_LOCK(m_Mutex);

  // one entry for all instances
  if (!m_Instances.IsEmpty())
  {
    plStringBuilder sDetail;
    sDetail.SetFormat("{} instances, {}", m_Instances.GetCount(), bImmediate ? "immediate" : "fade out");
    m_EventLog.Add(plAudioLogEventType::Stop, "<all>", sDetail);
  }

  m_PendingActivations.Clear();

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
    m_bDeviceRecovery = false;
  }
}

void plPlasmaAudioEventInstance::DetachForDeviceRecovery()
{
  if (m_pEngine == nullptr)
    return;

  const ma_uint64 uiNow = ma_engine_get_time_in_pcm_frames(m_pEngine);
  for (Voice& voice : m_Voices)
  {
    if (voice.m_bActive && voice.m_bVirtual)
      AdvanceVirtualCursor(voice, uiNow);

    if (voice.m_bSoundValid && !voice.m_bVirtual)
    {
      voice.m_uiRecoveryCursor = static_cast<ma_uint64>(static_cast<plInt64>(voice.m_Source.m_iPublishedCursor));
      voice.m_RecoveryFader = voice.m_Sound.engineNode.fader;
      voice.m_bRecoveryFadeApplied = voice.m_Source.m_bStartFadeApplied;
      voice.m_uiRecoveryStartFadeFrames = voice.m_Source.m_uiStartFadeFrames;
      voice.m_fRecoveryStartFadeVolume = voice.m_Source.m_fStartFadeVolume;
      voice.m_uiRecoveryStopFrame = ma_node_get_state_time(&voice.m_Sound, ma_node_state_stopped);
    }
    ReleaseVoiceChain(voice);
  }
  m_pEngine = nullptr;
}

plResult plPlasmaAudioEventInstance::RestoreAfterDeviceRecovery(ma_engine* pEngine)
{
  if (pEngine == nullptr || ma_engine_get_sample_rate(pEngine) != m_uiSampleRate)
    return PL_FAILURE;

  m_pEngine = pEngine;
  const ma_uint64 uiNow = ma_engine_get_time_in_pcm_frames(pEngine);
  bool bRestored = true;
  for (Voice& voice : m_Voices)
  {
    if (!voice.m_bActive || voice.m_bVirtual || voice.m_bSoundValid)
      continue;

    if (BuildVoiceChain(voice, plMath::Max(uiNow, voice.m_uiStartFrame), voice.m_uiRecoveryCursor, false,
          voice.m_uiRecoveryStartFadeFrames, voice.m_fRecoveryStartFadeVolume) != BuildResult::Success)
    {
      bRestored = false;
      continue;
    }

    if (voice.m_bRecoveryFadeApplied)
    {
      // The new sound may have a different channel count; only the fade's progress belongs to the old graph.
      const ma_fader_config config = voice.m_Sound.engineNode.fader.config;
      voice.m_Sound.engineNode.fader = voice.m_RecoveryFader;
      voice.m_Sound.engineNode.fader.config = config;
      voice.m_Source.m_bStartFadeApplied = true;
    }
    ma_sound_set_stop_time_in_pcm_frames(&voice.m_Sound, voice.m_uiRecoveryStopFrame);
    if (m_bStopping)
      ma_data_source_set_looping(voice.m_pInner, false);
  }
  ReattachToBusGraph();
  return bRestored ? PL_SUCCESS : PL_FAILURE;
}

void plPlasmaAudioEventManager::DetachForDeviceRecovery()
{
  PL_LOCK(m_Mutex);
  m_bDeviceRecovery = true;
  for (auto& pInstance : m_Instances)
    pInstance->DetachForDeviceRecovery();
}

plResult plPlasmaAudioEventManager::RestoreAfterDeviceRecovery(ma_engine* pEngine)
{
  PL_LOCK(m_Mutex);
  bool bRestored = pEngine != nullptr;
  for (auto& pInstance : m_Instances)
  {
    if (pInstance->RestoreAfterDeviceRecovery(pEngine).Failed())
      bRestored = false;
  }
  if (!bRestored)
    return PL_FAILURE;

  for (auto& pending : m_PendingActivations)
    pending.m_pEngine = pEngine;
  m_bDeviceRecovery = false;
  return PL_SUCCESS;
}

void plPlasmaAudioEventManager::SetEventParameter(const plPlasmaAudioEntityData* pEntity, plStringView sParamName, float fValue)
{
  PL_LOCK(m_Mutex);

  for (auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEntity == pEntity)
    {
      pInstance->SetParameter(sParamName, fValue);
    }
  }
}

void plPlasmaAudioEventManager::ResetEventParameter(const plPlasmaAudioEntityData* pEntity, plStringView sParamName)
{
  PL_LOCK(m_Mutex);

  for (auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEntity == pEntity)
    {
      pInstance->ResetParameter(sParamName);
    }
  }
}

void plPlasmaAudioEventManager::KeyOff(const plPlasmaAudioEntityData* pEntity)
{
  PL_LOCK(m_Mutex);

  for (auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEntity == pEntity)
    {
      pInstance->KeyOff();
    }
  }
}

void plPlasmaAudioEventManager::SetMusicState(const plPlasmaAudioEntityData* pEntity, plStringView sState)
{
  PL_LOCK(m_Mutex);

  for (auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEntity == pEntity && pInstance->IsMusic())
      pInstance->SetMusicState(sState);
  }
}

void plPlasmaAudioEventManager::PlayStinger(const plPlasmaAudioEntityData* pEntity, plStringView sStinger)
{
  PL_LOCK(m_Mutex);

  for (auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEntity == pEntity && pInstance->IsMusic())
      pInstance->PlayStinger(sStinger);
  }
}

bool plPlasmaAudioEventManager::GetMusicPosition(const plPlasmaAudioEventData* pEventData, plAudioMusicPlayer::Position& out_position) const
{
  PL_LOCK(m_Mutex);

  for (const auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEventData == pEventData && pInstance->IsMusic())
    {
      out_position = pInstance->GetMusicPosition();
      return true;
    }
  }

  return false;
}

void plPlasmaAudioEventManager::TakeMarkers(plPlasmaAudioEventData* pEventData, plDynamicArray<plString>& out_markers)
{
  PL_LOCK(m_Mutex);

  for (plString& sMarker : pEventData->m_PassedMarkers)
  {
    out_markers.PushBack(std::move(sMarker));
  }

  pEventData->m_PassedMarkers.Clear();
}

void plPlasmaAudioEventManager::GetSustainState(const plPlasmaAudioEventData* pEventData, plUInt32& out_uiSustainsLeft, bool& out_bHolding) const
{
  PL_LOCK(m_Mutex);

  out_uiSustainsLeft = 0;
  out_bHolding = false;

  for (const auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEventData == pEventData)
    {
      pInstance->GetSustainState(out_uiSustainsLeft, out_bHolding);
      return;
    }
  }
}

bool plPlasmaAudioEventManager::IsEventPlaying(const plPlasmaAudioEventData* pEventData) const
{
  PL_LOCK(m_Mutex);

  for (const PendingActivation& activation : m_PendingActivations)
  {
    if (activation.m_pEventData == pEventData)
      return true;
  }

  for (const auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEventData == pEventData)
      return true;
  }

  return false;
}

bool plPlasmaAudioEventManager::GetEventState(const plPlasmaAudioEventData* pEventData, bool& out_bWaiting, double& out_fPosition, plDynamicArray<bool>& out_sounding, plDynamicArray<float>& out_rangeGains) const
{
  PL_LOCK(m_Mutex);

  out_bWaiting = false;
  out_fPosition = 0.0;
  out_sounding.Clear();
  out_rangeGains.Clear();

  for (const PendingActivation& activation : m_PendingActivations)
  {
    if (activation.m_pEventData == pEventData)
    {
      out_bWaiting = true;
      return true;
    }
  }

  for (const auto& pInstance : m_Instances)
  {
    if (pInstance->m_pEventData != pEventData)
      continue;

    out_fPosition = pInstance->GetTimelinePosition();

    const plUInt32 uiInstruments = pInstance->GetInstrumentCount();
    out_sounding.SetCount(uiInstruments);
    out_rangeGains.SetCount(uiInstruments);
    for (plUInt32 i = 0; i < uiInstruments; ++i)
    {
      out_sounding[i] = pInstance->IsInstrumentSounding(i);
      out_rangeGains[i] = pInstance->GetInstrumentRangeGain(i);
    }
    return true;
  }

  return false;
}

void plPlasmaAudioEventManager::Update(plTime delta, plArrayPtr<const plVec3> listeners)
{
  {
    PL_LOCK(m_Mutex);

    if (m_bDeviceRecovery)
      return;

    m_Now += delta;

    if (!listeners.IsEmpty())
    {
      m_Listeners.Clear();
      for (const plVec3& vListener : listeners)
      {
        m_Listeners.PushBack(vListener);
      }
    }
  }

  // starts whose events finished loading; outside the mutex, which ActivateEventInternal takes itself
  ProcessPendingActivations();

  UpdateInstances(delta);

  // the clip loads the instances asked for, without the mutex (see the class description)
  FlushDeferredLoads();
}

void plPlasmaAudioEventManager::LinkNestedInstances(plHashTable<plUInt64, plPlasmaAudioEventInstance*>& out_parents)
{
  bool bAnyChild = false;
  for (auto& pInstance : m_Instances)
  {
    pInstance->m_uiLiveChildren = 0;
    bAnyChild |= pInstance->m_uiParentSerial != 0;
  }

  if (!bAnyChild)
    return;

  for (auto& pInstance : m_Instances)
  {
    out_parents.Insert(pInstance->m_uiSerial, pInstance.Borrow());
  }

  // a child stops with its parent, or when the parent is gone
  for (auto& pInstance : m_Instances)
  {
    if (pInstance->m_uiParentSerial == 0)
      continue;

    plPlasmaAudioEventInstance* pParent = nullptr;
    if (out_parents.TryGetValue(pInstance->m_uiParentSerial, pParent))
    {
      ++pParent->m_uiLiveChildren;

      if (pParent->IsStopping() && !pInstance->IsStopping())
        pInstance->Stop(false);
    }
    else if (!pInstance->IsStopping())
    {
      pInstance->Stop(false);
    }
  }
}

void plPlasmaAudioEventManager::StartNestedInstances()
{
  plHybridArray<plPlasmaAudioEventInstance*, 4> parents;
  for (auto& pInstance : m_Instances)
  {
    if (!pInstance->m_ChildRequests.IsEmpty())
      parents.PushBack(pInstance.Borrow());
  }

  for (plPlasmaAudioEventInstance* pParent : parents)
  {
    const plHybridArray<plPlasmaAudioEventInstance::ChildRequest, 1> requests = pParent->m_ChildRequests;
    pParent->m_ChildRequests.Clear();

    for (auto request : requests)
    {
      const plUInt32 uiInstancesBefore = m_Instances.GetCount();
      const plResourceState state = request.m_hEvent.IsValid() ? GetLoadingState(request.m_hEvent) : plResourceState::LoadedResourceMissing;

      // a nested event still loading (the parent preloads it) is waited for, up to MaxPendingStartTime; it then starts
      // where its timeline is by now
      if (state != plResourceState::Loaded && state != plResourceState::LoadedResourceMissing)
      {
        if (!request.m_bWaiting)
        {
          request.m_bWaiting = true;
          request.m_WaitStart = m_Now;
          m_EventLog.Add(plAudioLogEventType::EventLoadWait, request.m_hEvent.GetResourceID(), "nested event starts once loaded");
        }

        if (m_Now - request.m_WaitStart <= MaxPendingStartTime)
        {
          pParent->m_ChildRequests.PushBack(request);
          continue;
        }
      }

      const bool bCanStart = state == plResourceState::Loaded && pParent->m_uiNestingDepth < plPlasmaAudioEventInstance::MaxNestingDepth;

      if (bCanStart)
      {
        ActivateEventInternal(request.m_hEvent, request.m_hEvent.GetResourceID(), pParent->m_pEntity, nullptr, pParent->GetBusGraph(), pParent->GetEngine(), pParent, &request).IgnoreResult();
      }

      // not started (missing, still loading, too deep, or rejected by its polyphony): the instrument goes on without it
      if (m_Instances.GetCount() == uiInstancesBefore)
      {
        m_EventLog.Add(plAudioLogEventType::EventDropped, request.m_hEvent.IsValid() ? request.m_hEvent.GetResourceID().GetView() : plStringView("<no event>"), "nested event not started");
        pParent->OnChildFinished(request.m_uiInstrument, m_Rng);
      }
    }
  }
}

void plPlasmaAudioEventManager::UpdateInstances(plTime delta)
{
  PL_LOCK(m_Mutex);

  const plArrayPtr<const plVec3> listeners = m_Listeners.GetArrayPtr();

  // The ranking of the last update stays in use while the instances update, so voices that start in their updates are
  // admitted against it; RecycleInstance drops the candidates of each finished instance.
  {
    PL_PROFILE_SCOPE("PlasmaAudio.Instances");

    plHashTable<plUInt64, plPlasmaAudioEventInstance*> parents;
    LinkNestedInstances(parents);

    for (plUInt32 i = 0; i < m_Instances.GetCount();)
    {
      if (!m_Instances[i]->Update(delta, listeners, m_Rng))
      {
        plStringBuilder sDetail;
        sDetail.SetFormat("after {} s", plArgF(m_Instances[i]->GetAge().GetSeconds(), 2));
        m_EventLog.Add(plAudioLogEventType::Finished, m_Instances[i]->GetDisplayName(), sDetail);

        // a parent lives while its children do, so it is still there
        plPlasmaAudioEventInstance* pParent = nullptr;
        if (m_Instances[i]->m_uiParentSerial != 0 && parents.TryGetValue(m_Instances[i]->m_uiParentSerial, pParent))
        {
          pParent->OnChildFinished(m_Instances[i]->m_uiParentInstrument, m_Rng);
        }

        RecycleInstance(std::move(m_Instances[i]));
        m_Instances.RemoveAtAndSwap(i);
      }
      else
      {
        ++i;
      }
    }

    StartNestedInstances();
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
  PL_LOCK(m_Mutex);

  plUInt32 uiCount = 0;
  for (const auto& pInstance : m_Instances)
  {
    uiCount += pInstance->GetRealVoiceCount();
  }
  return uiCount;
}

plUInt32 plPlasmaAudioEventManager::GetVirtualVoiceCount() const
{
  PL_LOCK(m_Mutex);

  plUInt32 uiCount = 0;
  for (const auto& pInstance : m_Instances)
  {
    uiCount += pInstance->GetVirtualVoiceCount();
  }
  return uiCount;
}

void plPlasmaAudioEventManager::GetEventVoiceCounts(const plPlasmaAudioEventData* pEventData, plUInt32& out_uiReal, plUInt32& out_uiVirtual) const
{
  PL_LOCK(m_Mutex);

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
  PL_LOCK(m_Mutex);
  return m_VoiceManager.GetFlipCount();
}

void plPlasmaAudioEventManager::FillSnapshot(plAudioProfilingSnapshot& out_snapshot) const
{
  PL_LOCK(m_Mutex);

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
