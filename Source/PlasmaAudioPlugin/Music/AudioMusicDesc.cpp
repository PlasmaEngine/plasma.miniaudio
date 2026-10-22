#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Music/AudioMusicDesc.h>

// version 1: segments, states, transitions, stingers, beat messages
static constexpr plTypeVersion kAudioMusicDescVersion = 1;

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioMusicQuantize, 1)
  PL_ENUM_CONSTANTS(plAudioMusicQuantize::Immediate, plAudioMusicQuantize::NextBeat, plAudioMusicQuantize::NextBar, plAudioMusicQuantize::EndOfSegment, plAudioMusicQuantize::ExitCue)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioMusicStartAt, 1)
  PL_ENUM_CONSTANTS(plAudioMusicStartAt::SegmentStart, plAudioMusicStartAt::SamePosition)
PL_END_STATIC_REFLECTED_ENUM;
// clang-format on

plAudioMusicTransitionDesc plAudioMusicDesc::FindTransition(plUInt16 uiFrom, plUInt16 uiTo) const
{
  const plAudioMusicTransitionDesc* pAny = nullptr;

  for (const plAudioMusicTransitionDesc& transition : m_Transitions)
  {
    if (transition.m_uiTo != uiTo)
      continue;

    if (transition.m_uiFrom == uiFrom)
      return transition;

    if (transition.m_uiFrom == plAudioMusicTransitionDesc::AnyState && pAny == nullptr)
      pAny = &transition;
  }

  if (pAny != nullptr)
    return *pAny;

  plAudioMusicTransitionDesc fallback;
  fallback.m_uiFrom = uiFrom;
  fallback.m_uiTo = uiTo;
  return fallback;
}

plUInt32 plAudioMusicDesc::FindState(plStringView sName) const
{
  for (plUInt32 i = 0; i < m_States.GetCount(); ++i)
  {
    if (m_States[i].m_sName.IsEqual_NoCase(sName))
      return i;
  }

  return plInvalidIndex;
}

plUInt32 plAudioMusicDesc::FindStinger(plStringView sName) const
{
  for (plUInt32 i = 0; i < m_Stingers.GetCount(); ++i)
  {
    if (m_Stingers[i].m_sName.IsEqual_NoCase(sName))
      return i;
  }

  return plInvalidIndex;
}

void plAudioMusicDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream.WriteVersion(kAudioMusicDescVersion);

  inout_stream << m_Segments.GetCount();
  for (const plAudioMusicSegmentDesc& segment : m_Segments)
  {
    inout_stream << segment.m_sName;
    inout_stream << segment.m_fTempo;
    inout_stream << segment.m_uiBeatsPerBar;
    inout_stream << segment.m_uiBars;
    inout_stream.WriteArray(segment.m_ExitCues).IgnoreResult();
    inout_stream.WriteArray(segment.m_Stems).IgnoreResult();
  }

  inout_stream << m_States.GetCount();
  for (const plAudioMusicStateDesc& state : m_States)
  {
    inout_stream << state.m_sName;
    inout_stream.WriteArray(state.m_Segments).IgnoreResult();
  }

  inout_stream << m_Transitions.GetCount();
  for (const plAudioMusicTransitionDesc& transition : m_Transitions)
  {
    inout_stream << transition.m_uiFrom;
    inout_stream << transition.m_uiTo;
    inout_stream << static_cast<plUInt8>(transition.m_eChangeAt);
    inout_stream << transition.m_fFadeBeats;
    inout_stream << transition.m_uiVia;
    inout_stream << static_cast<plUInt8>(transition.m_eStartAt);
  }

  inout_stream << m_Stingers.GetCount();
  for (const plAudioMusicStingerDesc& stinger : m_Stingers)
  {
    inout_stream << stinger.m_sName;
    inout_stream << stinger.m_uiInstrument;
    inout_stream << static_cast<plUInt8>(stinger.m_eAt);
  }

  inout_stream << m_bBeatMessages;
}

void plAudioMusicDesc::Load(plStreamReader& inout_stream)
{
  inout_stream.ReadVersion(kAudioMusicDescVersion);

  plUInt32 uiCount = 0;
  plUInt8 uiEnum = 0;

  inout_stream >> uiCount;
  m_Segments.SetCount(uiCount);
  for (plAudioMusicSegmentDesc& segment : m_Segments)
  {
    inout_stream >> segment.m_sName;
    inout_stream >> segment.m_fTempo;
    inout_stream >> segment.m_uiBeatsPerBar;
    inout_stream >> segment.m_uiBars;
    inout_stream.ReadArray(segment.m_ExitCues).IgnoreResult();
    inout_stream.ReadArray(segment.m_Stems).IgnoreResult();
  }

  inout_stream >> uiCount;
  m_States.SetCount(uiCount);
  for (plAudioMusicStateDesc& state : m_States)
  {
    inout_stream >> state.m_sName;
    inout_stream.ReadArray(state.m_Segments).IgnoreResult();
  }

  inout_stream >> uiCount;
  m_Transitions.SetCount(uiCount);
  for (plAudioMusicTransitionDesc& transition : m_Transitions)
  {
    inout_stream >> transition.m_uiFrom;
    inout_stream >> transition.m_uiTo;
    inout_stream >> uiEnum;
    transition.m_eChangeAt = static_cast<plAudioMusicQuantize::Enum>(uiEnum);
    inout_stream >> transition.m_fFadeBeats;
    inout_stream >> transition.m_uiVia;
    inout_stream >> uiEnum;
    transition.m_eStartAt = static_cast<plAudioMusicStartAt::Enum>(uiEnum);
  }

  inout_stream >> uiCount;
  m_Stingers.SetCount(uiCount);
  for (plAudioMusicStingerDesc& stinger : m_Stingers)
  {
    inout_stream >> stinger.m_sName;
    inout_stream >> stinger.m_uiInstrument;
    inout_stream >> uiEnum;
    stinger.m_eAt = static_cast<plAudioMusicQuantize::Enum>(uiEnum);
  }

  inout_stream >> m_bBeatMessages;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Music_AudioMusicDesc);
