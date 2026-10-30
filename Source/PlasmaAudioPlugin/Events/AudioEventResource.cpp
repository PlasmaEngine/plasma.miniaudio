#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <AudioSystem/Core/AudioSystem.h>
#include <AudioSystem/Core/AudioSystemAllocator.h>
#include <AudioSystem/Core/AudioSystemData.h>
#include <PlasmaAudioPlugin/Events/AudioEventResource.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <MiniAudio/miniaudio.h>

// version 2: instrument m_bMute
// version 3: event fade-in + loop region
// version 4: instrument switch-state filter
// version 5: playlist entry clip reference
// version 6: voice priority + virtual mode
// version 7: instrument parameter range
// version 8: attenuation curve, cone, spread, occlusion settings
// version 9: seek speeds, automation targets' settings, nested events, markers, sustain points
// version 10: music
static constexpr plTypeVersion kAudioEventDescVersion = 10;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioEventResource, 1, plRTTIDefaultAllocator<plAudioEventResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioAttenuationCurve, 1)
  PL_ENUM_CONSTANTS(plAudioAttenuationCurve::Inverse, plAudioAttenuationCurve::Linear, plAudioAttenuationCurve::Logarithmic, plAudioAttenuationCurve::Custom, plAudioAttenuationCurve::None)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioEventPlaylistMode, 1)
  PL_ENUM_CONSTANTS(plAudioEventPlaylistMode::Random, plAudioEventPlaylistMode::Shuffle, plAudioEventPlaylistMode::Sequential)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioEventStealingMode, 1)
  PL_ENUM_CONSTANTS(plAudioEventStealingMode::None, plAudioEventStealingMode::Oldest, plAudioEventStealingMode::Quietest)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioEventVirtualMode, 1)
  PL_ENUM_CONSTANTS(plAudioEventVirtualMode::Resume, plAudioEventVirtualMode::Restart, plAudioEventVirtualMode::Kill)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioEventAutomationTarget, 1)
  PL_ENUM_CONSTANTS(plAudioEventAutomationTarget::EventVolume, plAudioEventAutomationTarget::EventPitch)
  PL_ENUM_CONSTANTS(plAudioEventAutomationTarget::InstrumentVolume, plAudioEventAutomationTarget::FilterCutoff, plAudioEventAutomationTarget::SendLevel)
PL_END_STATIC_REFLECTED_ENUM;
// clang-format on

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plAudioEventResource);

void plAudioEventPlaylistEntryDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_uiSample;
  inout_stream << m_fWeight;
  inout_stream << m_sClip;
}

void plAudioEventPlaylistEntryDesc::Load(plStreamReader& inout_stream, plTypeVersion uiVersion)
{
  inout_stream >> m_uiSample;
  inout_stream >> m_fWeight;

  if (uiVersion >= 5)
  {
    inout_stream >> m_sClip;
  }
}

void plAudioEventInstrumentDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_sName;
  inout_stream << m_StartDelay;
  inout_stream << static_cast<plUInt8>(m_ePlaylistMode);
  inout_stream << m_bLoop;
  inout_stream << m_bMute;
  inout_stream << m_sSwitchState;
  inout_stream << m_MinRetriggerDelay;
  inout_stream << m_MaxRetriggerDelay;
  inout_stream << m_fVolumeDb;
  inout_stream << m_fVolumeRandDb;
  inout_stream << m_fPitchSemitones;
  inout_stream << m_fPitchRandSemitones;
  inout_stream << m_uiRangeParameter;
  inout_stream << m_fRangeMin;
  inout_stream << m_fRangeMax;
  inout_stream << m_fRangeFade;

  inout_stream << m_Playlist.GetCount();
  for (const auto& entry : m_Playlist)
    entry.Save(inout_stream);
}

void plAudioEventInstrumentDesc::Load(plStreamReader& inout_stream, plTypeVersion uiVersion)
{
  inout_stream >> m_sName;
  inout_stream >> m_StartDelay;
  plUInt8 uiMode = 0;
  inout_stream >> uiMode;
  m_ePlaylistMode = static_cast<plAudioEventPlaylistMode::Enum>(uiMode);
  inout_stream >> m_bLoop;

  if (uiVersion >= 2)
  {
    inout_stream >> m_bMute;
  }

  if (uiVersion >= 4)
  {
    inout_stream >> m_sSwitchState;
  }
  inout_stream >> m_MinRetriggerDelay;
  inout_stream >> m_MaxRetriggerDelay;
  inout_stream >> m_fVolumeDb;
  inout_stream >> m_fVolumeRandDb;
  inout_stream >> m_fPitchSemitones;
  inout_stream >> m_fPitchRandSemitones;

  if (uiVersion >= 7)
  {
    inout_stream >> m_uiRangeParameter;
    inout_stream >> m_fRangeMin;
    inout_stream >> m_fRangeMax;
    inout_stream >> m_fRangeFade;
  }

  plUInt32 uiCount = 0;
  inout_stream >> uiCount;
  m_Playlist.SetCount(uiCount);
  for (auto& entry : m_Playlist)
    entry.Load(inout_stream, uiVersion);
}

float plAudioEventInstrumentDesc::GetRangeGain(float fValue) const
{
  if (m_uiRangeParameter == plAudioEventNoParameter)
    return 1.0f;

  const float fMin = plMath::Min(m_fRangeMin, m_fRangeMax);
  const float fMax = plMath::Max(m_fRangeMin, m_fRangeMax);

  if (fValue >= fMin && fValue <= fMax)
    return 1.0f;

  if (m_fRangeFade <= 0.0f)
    return 0.0f;

  const float fOutside = fValue < fMin ? fMin - fValue : fValue - fMax;
  return plMath::Clamp(1.0f - fOutside / m_fRangeFade, 0.0f, 1.0f);
}

void plAudioEventParameterDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_sName;
  inout_stream << m_fMin;
  inout_stream << m_fMax;
  inout_stream << m_fDefault;
}

void plAudioEventParameterDesc::Load(plStreamReader& inout_stream)
{
  inout_stream >> m_sName;
  inout_stream >> m_fMin;
  inout_stream >> m_fMax;
  inout_stream >> m_fDefault;
}

void plAudioEventAutomationDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_uiParameter;
  inout_stream << static_cast<plUInt8>(m_eTarget);
  m_Curve.Save(inout_stream);
}

void plAudioEventAutomationDesc::Load(plStreamReader& inout_stream)
{
  inout_stream >> m_uiParameter;
  plUInt8 uiTarget = 0;
  inout_stream >> uiTarget;
  m_eTarget = static_cast<plAudioEventAutomationTarget::Enum>(uiTarget);
  m_Curve.Load(inout_stream);
  m_Curve.SortControlPoints();
  m_Curve.CreateLinearApproximation();
}

plResult plAudioEventDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream.WriteVersion(kAudioEventDescVersion);

  inout_stream << m_sOutputBus;
  inout_stream << m_uiMaxInstances;
  inout_stream << static_cast<plUInt8>(m_eStealing);
  inout_stream << m_uiPriority;
  inout_stream << static_cast<plUInt8>(m_eVirtualMode);
  inout_stream << m_Cooldown;
  inout_stream << m_StopFadeOut;
  inout_stream << m_FadeIn;
  inout_stream << m_bLoopRegion;
  inout_stream << m_LoopStart;
  inout_stream << m_LoopEnd;

  inout_stream << m_bSpatialize;
  inout_stream << m_fMinDistance;
  inout_stream << m_fMaxDistance;
  inout_stream << m_fRolloff;
  inout_stream << m_fDopplerFactor;

  inout_stream << static_cast<plUInt8>(m_eAttenuationCurve);
  inout_stream << m_AttenuationPoints.GetCount();
  for (const plVec2& point : m_AttenuationPoints)
    inout_stream << point;
  inout_stream << m_fConeInnerAngle;
  inout_stream << m_fConeOuterAngle;
  inout_stream << m_fConeOuterGainDb;
  inout_stream << m_fSpread;
  inout_stream << m_fOcclusionCutoffHz;
  inout_stream << m_fOcclusionVolumeDb;

  inout_stream << m_Instruments.GetCount();
  for (const auto& instrument : m_Instruments)
    instrument.Save(inout_stream);

  inout_stream << m_Parameters.GetCount();
  for (const auto& parameter : m_Parameters)
    parameter.Save(inout_stream);

  inout_stream << m_Automations.GetCount();
  for (const auto& automation : m_Automations)
    automation.Save(inout_stream);

  inout_stream << m_SampleNames.GetCount();
  for (const auto& sName : m_SampleNames)
    inout_stream << sName;

  // version 9: seek speeds, the new automation targets' settings, nested events, markers and sustain points
  for (const auto& parameter : m_Parameters)
    inout_stream << parameter.m_fSeekSpeed;

  for (const auto& automation : m_Automations)
  {
    inout_stream << automation.m_uiInstrument;
    inout_stream << automation.m_sBus;
  }

  for (const auto& instrument : m_Instruments)
  {
    for (const auto& entry : instrument.m_Playlist)
      inout_stream << entry.m_sEvent;

    inout_stream << instrument.m_NestedLength;
  }

  inout_stream << m_Markers.GetCount();
  for (const plAudioEventMarkerDesc& marker : m_Markers)
  {
    inout_stream << marker.m_sName;
    inout_stream << marker.m_Time;
  }

  inout_stream << m_SustainPoints.GetCount();
  for (plTime time : m_SustainPoints)
    inout_stream << time;

  m_Music.Save(inout_stream);

  return PL_SUCCESS;
}

plResult plAudioEventDesc::Load(plStreamReader& inout_stream)
{
  const plTypeVersion uiVersion = inout_stream.ReadVersion(kAudioEventDescVersion);

  inout_stream >> m_sOutputBus;
  inout_stream >> m_uiMaxInstances;
  plUInt8 uiStealing = 0;
  inout_stream >> uiStealing;
  m_eStealing = static_cast<plAudioEventStealingMode::Enum>(uiStealing);

  if (uiVersion >= 6)
  {
    inout_stream >> m_uiPriority;
    plUInt8 uiVirtualMode = 0;
    inout_stream >> uiVirtualMode;
    m_eVirtualMode = static_cast<plAudioEventVirtualMode::Enum>(uiVirtualMode);
  }

  inout_stream >> m_Cooldown;
  inout_stream >> m_StopFadeOut;

  if (uiVersion >= 3)
  {
    inout_stream >> m_FadeIn;
    inout_stream >> m_bLoopRegion;
    inout_stream >> m_LoopStart;
    inout_stream >> m_LoopEnd;
  }

  inout_stream >> m_bSpatialize;
  inout_stream >> m_fMinDistance;
  inout_stream >> m_fMaxDistance;
  inout_stream >> m_fRolloff;
  inout_stream >> m_fDopplerFactor;

  plUInt32 uiCount = 0;

  // older events keep the inverse curve, which is what they played with, and get no cone, spread or occlusion volume
  m_eAttenuationCurve = plAudioAttenuationCurve::Inverse;
  m_AttenuationPoints.Clear();
  m_fConeInnerAngle = 360.0f;
  m_fConeOuterAngle = 360.0f;
  m_fConeOuterGainDb = -12.0f;
  m_fSpread = 0.0f;
  m_fOcclusionCutoffHz = 250.0f;
  m_fOcclusionVolumeDb = 0.0f;

  if (uiVersion >= 8)
  {
    plUInt8 uiCurve = 0;
    inout_stream >> uiCurve;
    m_eAttenuationCurve = static_cast<plAudioAttenuationCurve::Enum>(uiCurve);

    inout_stream >> uiCount;
    m_AttenuationPoints.SetCount(uiCount);
    for (plVec2& point : m_AttenuationPoints)
      inout_stream >> point;

    inout_stream >> m_fConeInnerAngle;
    inout_stream >> m_fConeOuterAngle;
    inout_stream >> m_fConeOuterGainDb;
    inout_stream >> m_fSpread;
    inout_stream >> m_fOcclusionCutoffHz;
    inout_stream >> m_fOcclusionVolumeDb;
  }

  inout_stream >> uiCount;
  m_Instruments.SetCount(uiCount);
  for (auto& instrument : m_Instruments)
    instrument.Load(inout_stream, uiVersion);

  inout_stream >> uiCount;
  m_Parameters.SetCount(uiCount);
  for (auto& parameter : m_Parameters)
    parameter.Load(inout_stream);

  inout_stream >> uiCount;
  m_Automations.SetCount(uiCount);
  for (auto& automation : m_Automations)
    automation.Load(inout_stream);

  inout_stream >> uiCount;
  m_SampleNames.SetCount(uiCount);
  for (auto& sName : m_SampleNames)
    inout_stream >> sName;

  m_Markers.Clear();
  m_SustainPoints.Clear();
  m_Music = plAudioMusicDesc();

  if (uiVersion < 9)
    return PL_SUCCESS;

  for (auto& parameter : m_Parameters)
    inout_stream >> parameter.m_fSeekSpeed;

  for (auto& automation : m_Automations)
  {
    inout_stream >> automation.m_uiInstrument;
    inout_stream >> automation.m_sBus;
  }

  for (auto& instrument : m_Instruments)
  {
    for (auto& entry : instrument.m_Playlist)
      inout_stream >> entry.m_sEvent;

    inout_stream >> instrument.m_NestedLength;
  }

  inout_stream >> uiCount;
  m_Markers.SetCount(uiCount);
  for (plAudioEventMarkerDesc& marker : m_Markers)
  {
    inout_stream >> marker.m_sName;
    inout_stream >> marker.m_Time;
  }

  inout_stream >> uiCount;
  m_SustainPoints.SetCount(uiCount);
  for (plTime& time : m_SustainPoints)
    inout_stream >> time;

  if (uiVersion >= 10)
    m_Music.Load(inout_stream);

  return PL_SUCCESS;
}

float plAudioEventDesc::GetDistanceGain(float fDistance) const
{
  if (!m_bSpatialize)
    return 1.0f;

  const float fMin = plMath::Max(m_fMinDistance, 0.0001f);
  const float fMax = plMath::Max(m_fMaxDistance, fMin + 0.0001f);

  if (fDistance <= fMin)
    return 1.0f;

  if (fDistance >= fMax)
    return 0.0f;

  const float fNormalized = (fDistance - fMin) / (fMax - fMin);

  float fGain = 1.0f;
  switch (m_eAttenuationCurve)
  {
    case plAudioAttenuationCurve::Inverse:
      fGain = fMin / (fMin + plMath::Max(m_fRolloff, 0.0f) * (fDistance - fMin));
      break;

    case plAudioAttenuationCurve::Linear:
      fGain = 1.0f - fNormalized;
      break;

    case plAudioAttenuationCurve::Logarithmic:
      fGain = 1.0f - plMath::Ln(fDistance / fMin) / plMath::Ln(fMax / fMin);
      break;

    case plAudioAttenuationCurve::Custom:
    {
      // straight lines from (0, 1) through the points; the last point's gain holds to the max distance
      plVec2 prev(0.0f, 1.0f);
      fGain = m_AttenuationPoints.IsEmpty() ? 1.0f - fNormalized : m_AttenuationPoints.PeekBack().y;

      for (const plVec2& point : m_AttenuationPoints)
      {
        if (fNormalized <= point.x)
        {
          const float fT = point.x > prev.x ? (fNormalized - prev.x) / (point.x - prev.x) : 1.0f;
          fGain = plMath::Lerp(prev.y, point.y, plMath::Clamp(fT, 0.0f, 1.0f));
          break;
        }

        prev = point;
      }
      break;
    }

    case plAudioAttenuationCurve::None:
      break;
  }

  // the last 15 % of [min, max] fade to silence, which is where the voice manager virtualizes the event
  constexpr float fFadeShare = 0.15f;
  const float fFade = plMath::Clamp((1.0f - fNormalized) / fFadeShare, 0.0f, 1.0f);

  return plMath::Clamp(fGain, 0.0f, 1.0f) * fFade;
}

float plAudioEventDesc::GetConeGain(float fAngleDegrees) const
{
  const float fInner = plMath::Clamp(m_fConeInnerAngle, 0.0f, 360.0f) * 0.5f;
  const float fOuter = plMath::Clamp(plMath::Max(m_fConeOuterAngle, m_fConeInnerAngle), 0.0f, 360.0f) * 0.5f;
  const float fAngle = plMath::Abs(fAngleDegrees);

  if (fAngle <= fInner)
    return 1.0f;

  const float fOuterGain = plMath::Pow(10.0f, m_fConeOuterGainDb / 20.0f);
  if (fAngle >= fOuter)
    return fOuterGain;

  return plMath::Lerp(1.0f, fOuterGain, (fAngle - fInner) / plMath::Max(fOuter - fInner, 0.0001f));
}

float plAudioEventDesc::GetOcclusionCutoff(float fOcclusion) const
{
  // exponential sweep from fully open (22 kHz) down to the event's cutoff
  const float fCutoff = plMath::Clamp(m_fOcclusionCutoffHz, 20.0f, 22000.0f);
  return 22000.0f * plMath::Pow(fCutoff / 22000.0f, plMath::Clamp(fOcclusion, 0.0f, 1.0f));
}

float plAudioEventDesc::GetOcclusionGain(float fOcclusion) const
{
  return plMath::Pow(10.0f, m_fOcclusionVolumeDb * plMath::Clamp(fOcclusion, 0.0f, 1.0f) / 20.0f);
}

plUInt32 plAudioEventDesc::FindParameterByName(plStringView sName) const
{
  for (plUInt32 i = 0; i < m_Parameters.GetCount(); ++i)
  {
    if (m_Parameters[i].m_sName.IsEqual_NoCase(sName))
      return i;
  }
  return plInvalidIndex;
}

plAudioEventResource::plAudioEventResource()
  : plResource(DoUpdate::OnAnyThread, 1)
{
  ModifyMemoryUsage().m_uiMemoryCPU = sizeof(plAudioEventResource);
}

plAudioEventResource::~plAudioEventResource()
{
  UnlinkLoaded();
}

plMutex plAudioEventResource::s_LoadedMutex;
plAudioEventResource* plAudioEventResource::s_pFirstLoaded = nullptr;

void plAudioEventResource::LinkLoaded()
{
  plUInt64 uiSampleBytes = 0;
  for (const plDataBuffer& data : m_SampleData)
  {
    uiSampleBytes += data.GetCount();
  }

  PL_LOCK(s_LoadedMutex);

  m_uiSampleBytes = uiSampleBytes;

  if (m_bLinkedLoaded)
    return;

  m_pPrevLoaded = nullptr;
  m_pNextLoaded = s_pFirstLoaded;
  if (s_pFirstLoaded != nullptr)
  {
    s_pFirstLoaded->m_pPrevLoaded = this;
  }
  s_pFirstLoaded = this;
  m_bLinkedLoaded = true;
}

void plAudioEventResource::UnlinkLoaded()
{
  PL_LOCK(s_LoadedMutex);

  if (!m_bLinkedLoaded)
    return;

  if (m_pPrevLoaded != nullptr)
  {
    m_pPrevLoaded->m_pNextLoaded = m_pNextLoaded;
  }
  else
  {
    s_pFirstLoaded = m_pNextLoaded;
  }

  if (m_pNextLoaded != nullptr)
  {
    m_pNextLoaded->m_pPrevLoaded = m_pPrevLoaded;
  }

  m_pPrevLoaded = nullptr;
  m_pNextLoaded = nullptr;
  m_bLinkedLoaded = false;
}

void plAudioEventResource::GatherLoadedEvents(plDynamicArray<plAudioEventMemorySnapshotEntry>& out_events)
{
  out_events.Clear();

  PL_LOCK(s_LoadedMutex);

  for (const plAudioEventResource* pEvent = s_pFirstLoaded; pEvent != nullptr; pEvent = pEvent->m_pNextLoaded)
  {
    plAudioEventMemorySnapshotEntry& entry = out_events.ExpandAndGetRef();
    entry.m_sName = !pEvent->GetResourceDescription().IsEmpty() ? pEvent->GetResourceDescription() : pEvent->GetResourceID();
    entry.m_uiSampleBytes = pEvent->m_uiSampleBytes;
  }
}

plUInt64 plAudioEventResource::GetTotalSampleBytes()
{
  PL_LOCK(s_LoadedMutex);

  plUInt64 uiBytes = 0;
  for (const plAudioEventResource* pEvent = s_pFirstLoaded; pEvent != nullptr; pEvent = pEvent->m_pNextLoaded)
  {
    uiBytes += pEvent->m_uiSampleBytes;
  }

  return uiBytes;
}

const plAudioClipResourceHandle& plAudioEventResource::GetClip(plUInt32 uiInstrument, plUInt32 uiEntry) const
{
  static const plAudioClipResourceHandle s_hNone;

  if (uiInstrument >= m_InstrumentClipOffsets.GetCount())
    return s_hNone;

  const plUInt32 uiIndex = m_InstrumentClipOffsets[uiInstrument] + uiEntry;
  const plUInt32 uiEnd = (uiInstrument + 1 < m_InstrumentClipOffsets.GetCount()) ? m_InstrumentClipOffsets[uiInstrument + 1] : m_Clips.GetCount();

  if (uiIndex >= uiEnd)
    return s_hNone;

  return m_Clips[uiIndex];
}

void plAudioEventResource::GetSampleLength(plUInt32 uiIndex, plUInt64& out_uiFrames, plUInt32& out_uiSampleRate) const
{
  out_uiFrames = 0;
  out_uiSampleRate = 0;

  if (uiIndex >= m_SampleData.GetCount())
    return;

  if (m_SampleLengths.GetCount() != m_SampleData.GetCount())
  {
    m_SampleLengths.Clear();
    m_SampleLengths.SetCount(m_SampleData.GetCount());
  }

  SampleLength& length = m_SampleLengths[uiIndex];

  if (!length.m_bProbed)
  {
    length.m_bProbed = true;

    ma_decoder decoder;
    if (ma_decoder_init_memory(m_SampleData[uiIndex].GetData(), m_SampleData[uiIndex].GetCount(), nullptr, &decoder) == MA_SUCCESS)
    {
      ma_uint64 uiFrames = 0;
      if (ma_decoder_get_length_in_pcm_frames(&decoder, &uiFrames) == MA_SUCCESS)
      {
        length.m_uiFrames = uiFrames;
        length.m_uiSampleRate = decoder.outputSampleRate;
      }

      ma_decoder_uninit(&decoder);
    }
  }

  out_uiFrames = length.m_uiFrames;
  out_uiSampleRate = length.m_uiSampleRate;
}

plAudioEventResourceHandle plAudioEventResource::GetNestedEvent(plUInt32 uiInstrument, plUInt32 uiEntry) const
{
  if (uiInstrument >= m_InstrumentClipOffsets.GetCount() || uiInstrument >= m_Desc.m_Instruments.GetCount())
    return {};

  const plUInt32 uiIndex = m_InstrumentClipOffsets[uiInstrument] + uiEntry;
  const plUInt32 uiEnd = (uiInstrument + 1 < m_InstrumentClipOffsets.GetCount()) ? m_InstrumentClipOffsets[uiInstrument + 1] : m_NestedEvents.GetCount();

  if (uiIndex >= uiEnd)
    return {};

  // an event that nests itself keeps no handle to itself (see SetupClips)
  if (!m_NestedEvents[uiIndex].IsValid() && m_Desc.m_Instruments[uiInstrument].m_Playlist[uiEntry].m_sEvent == GetResourceID())
    return plAudioEventResourceHandle(const_cast<plAudioEventResource*>(this));

  return m_NestedEvents[uiIndex];
}

void plAudioEventResource::SetupClips()
{
  m_Clips.Clear();
  m_NestedEvents.Clear();
  m_InstrumentClipOffsets.Clear();
  m_SampleLengths.Clear();

  bool bAnyReference = false;
  for (const auto& instrument : m_Desc.m_Instruments)
  {
    for (const auto& entry : instrument.m_Playlist)
    {
      bAnyReference |= !entry.m_sClip.IsEmpty() || !entry.m_sEvent.IsEmpty();
    }
  }

  // events without clips or nested events (embedded samples only) keep the arrays empty
  if (!bAnyReference)
    return;

  m_InstrumentClipOffsets.SetCount(m_Desc.m_Instruments.GetCount());

  for (plUInt32 i = 0; i < m_Desc.m_Instruments.GetCount(); ++i)
  {
    m_InstrumentClipOffsets[i] = m_Clips.GetCount();

    for (const auto& entry : m_Desc.m_Instruments[i].m_Playlist)
    {
      plAudioClipResourceHandle& hClip = m_Clips.ExpandAndGetRef();
      plAudioEventResourceHandle& hEvent = m_NestedEvents.ExpandAndGetRef();

      // a nested event takes precedence over a clip; a handle of the event to itself would keep it loaded forever
      if (!entry.m_sEvent.IsEmpty())
      {
        if (entry.m_sEvent != GetResourceID())
        {
          hEvent = plResourceManager::LoadResource<plAudioEventResource>(entry.m_sEvent);
          plResourceManager::PreloadResource(hEvent);
        }
        continue;
      }

      if (entry.m_sClip.IsEmpty())
        continue;

      hClip = plResourceManager::LoadResource<plAudioClipResource>(entry.m_sClip);
      plResourceManager::PreloadResource(hClip);
    }
  }
}

plResourceLoadDesc plAudioEventResource::UnloadData(Unload WhatToUnload)
{
  PL_IGNORE_UNUSED(WhatToUnload);

  UnlinkLoaded();

  m_Desc = plAudioEventDesc();
  m_SampleData.Clear();
  m_SampleData.Compact();
  m_Clips.Clear();
  m_Clips.Compact();
  m_NestedEvents.Clear();
  m_NestedEvents.Compact();
  m_InstrumentClipOffsets.Clear();
  m_InstrumentClipOffsets.Compact();
  m_SampleLengths.Clear();
  m_SampleLengths.Compact();

  ModifyMemoryUsage().m_uiMemoryCPU = sizeof(plAudioEventResource);

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Unloaded;

  return res;
}

plResourceLoadDesc plAudioEventResource::UpdateContent(plStreamReader* pStream)
{
  PL_LOG_BLOCK("plAudioEventResource::UpdateContent", GetResourceIdOrDescription());

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;

  if (pStream == nullptr)
  {
    res.m_State = plResourceState::LoadedResourceMissing;
    return res;
  }

  UnlinkLoaded();

  // the standard file reader writes the absolute file path into the stream
  plString sAbsFilePath;
  (*pStream) >> sAbsFilePath;

  // skip the asset file header at the start of the file
  plAssetFileHeader AssetHash;
  AssetHash.Read(*pStream).IgnoreResult();

  if (m_Desc.Load(*pStream).Failed())
  {
    res.m_State = plResourceState::LoadedResourceMissing;
    return res;
  }

  plUInt32 uiNumSamples = 0;
  *pStream >> uiNumSamples;

  m_SampleData.SetCount(uiNumSamples);

  for (plUInt32 i = 0; i < uiNumSamples; ++i)
  {
    plUInt32 uiSize = 0;
    *pStream >> uiSize;

    m_SampleData[i].SetCountUninitialized(uiSize);
    if (pStream->ReadBytes(m_SampleData[i].GetData(), uiSize) != uiSize)
    {
      res.m_State = plResourceState::LoadedResourceMissing;
      return res;
    }
  }

  SetupClips();

  // register an ATL trigger under this resource's id, so plAudioTriggerComponent /
  // plSoundInterface::PlaySound can fire the event by asset reference
  // plAudioTriggerComponent / plSoundInterface::PlaySound can fire the event by asset reference. A reload finds the
  // trigger already registered; registering again would leak the new trigger data, which the ATL skips.
  if (auto* pAudioSystem = plAudioSystem::GetSingleton(); pAudioSystem != nullptr && pAudioSystem->GetTriggerId(GetResourceID()) == 0)
  {
    const plAudioSystemDataID uiTriggerId = plHashHelper<plStringView>::Hash(GetResourceID());
    pAudioSystem->RegisterTrigger(uiTriggerId, PL_AUDIOSYSTEM_NEW(plAudioSystemStringTriggerData, uiTriggerId, GetResourceID()));
  }

  LinkLoaded();

  res.m_State = plResourceState::Loaded;
  return res;
}

void plAudioEventResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plAudioEventResource);
  out_NewMemoryUsage.m_uiMemoryCPU += m_SampleData.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryCPU += m_Clips.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryCPU += m_InstrumentClipOffsets.GetHeapMemoryUsage();

  for (const auto& data : m_SampleData)
  {
    out_NewMemoryUsage.m_uiMemoryCPU += data.GetHeapMemoryUsage();
  }

  out_NewMemoryUsage.m_uiMemoryGPU = 0;
}

PL_RESOURCE_IMPLEMENT_CREATEABLE(plAudioEventResource, plAudioEventResourceDescriptor)
{
  UnlinkLoaded();

  m_Desc = descriptor.m_Desc;
  m_SampleData = descriptor.m_SampleData;

  SetupClips();
  LinkLoaded();

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Loaded;

  return res;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Events_AudioEventResource);
