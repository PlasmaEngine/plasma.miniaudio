#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Music/AudioMusicPlayer.h>

void plAudioMusicPlayer::Reset()
{
  m_pDesc = nullptr;
  m_uiSampleRate = 48000;
  m_uiStartFrame = 0;
  m_uiHorizon = 0;
  m_Placed.Clear();
  m_uiTargetState = NoState;
  m_uiRequestedState = NoState;
  m_bChangePending = false;
  m_Change = PendingChange();
  m_Stingers.Clear();
  m_Messages.Clear();
}

void plAudioMusicPlayer::Start(const plAudioMusicDesc* pDesc, plUInt64 uiStartFrame, plUInt32 uiSampleRate, plUInt16 uiState)
{
  Reset();

  if (pDesc == nullptr || !pDesc->IsMusic())
    return;

  m_pDesc = pDesc;
  m_uiSampleRate = plMath::Max(uiSampleRate, 1u);
  m_uiStartFrame = uiStartFrame;
  m_uiHorizon = uiStartFrame;

  m_uiTargetState = (uiState < pDesc->m_States.GetCount()) ? uiState : 0;
  m_uiRequestedState = m_uiTargetState;
}

void plAudioMusicPlayer::RequestState(plUInt16 uiState)
{
  if (m_pDesc == nullptr || uiState >= m_pDesc->m_States.GetCount() || uiState == m_uiRequestedState)
    return;

  m_uiRequestedState = uiState;

  // back to where the music heads anyway: nothing changes
  if (uiState == m_uiTargetState)
  {
    m_bChangePending = false;
    return;
  }

  m_bChangePending = true;
  m_Change.m_uiTo = uiState;
  m_Change.m_uiFrame = 0;
  m_Change.m_Transition = m_pDesc->FindTransition(m_uiTargetState, uiState);
}

void plAudioMusicPlayer::RequestStinger(plUInt32 uiStinger)
{
  if (m_pDesc == nullptr || uiStinger >= m_pDesc->m_Stingers.GetCount())
    return;

  PendingStinger& stinger = m_Stingers.ExpandAndGetRef();
  stinger.m_uiStinger = uiStinger;
}

double plAudioMusicPlayer::GetBeatFrames(const plAudioMusicSegmentDesc& segment) const
{
  return segment.GetBeatSeconds() * m_uiSampleRate;
}

plUInt64 plAudioMusicPlayer::GetFrameOfBeat(const Placed& placed, double fBeats) const
{
  const double fFrame = placed.m_fGridOrigin + fBeats * GetBeatFrames(m_pDesc->m_Segments[placed.m_uiSegment]);
  return static_cast<plUInt64>(plMath::Max(fFrame, 0.0) + 0.5);
}

const plAudioMusicPlayer::Placed* plAudioMusicPlayer::GetPlacedAt(plUInt64 uiFrame) const
{
  for (const Placed& placed : m_Placed)
  {
    if (placed.m_uiStart <= uiFrame && uiFrame < placed.m_uiMusicalEnd)
      return &placed;
  }

  return m_Placed.IsEmpty() ? nullptr : &m_Placed.PeekBack();
}

plUInt64 plAudioMusicPlayer::GetLanding(plAudioMusicQuantize::Enum quantize, plUInt64 uiEarliest) const
{
  const Placed* pPlaced = GetPlacedAt(uiEarliest);
  if (pPlaced == nullptr || quantize == plAudioMusicQuantize::Immediate || pPlaced->m_uiMusicalEnd <= uiEarliest)
    return uiEarliest;

  const plAudioMusicSegmentDesc& segment = m_pDesc->m_Segments[pPlaced->m_uiSegment];
  const double fBeat = GetBeatFrames(segment);
  const double fBeatsPerBar = plMath::Max<plUInt8>(segment.m_uiBeatsPerBar, 1);

  // beats from the grid origin to the earliest frame; a frame on a boundary is that boundary
  const double fBeatsIn = (static_cast<double>(uiEarliest) - pPlaced->m_fGridOrigin) / fBeat - 1e-9;

  plUInt64 uiFrame = pPlaced->m_uiEnd;

  switch (quantize)
  {
    case plAudioMusicQuantize::NextBeat:
      uiFrame = GetFrameOfBeat(*pPlaced, plMath::Ceil(fBeatsIn));
      break;

    case plAudioMusicQuantize::NextBar:
      uiFrame = GetFrameOfBeat(*pPlaced, plMath::Ceil(fBeatsIn / fBeatsPerBar) * fBeatsPerBar);
      break;

    case plAudioMusicQuantize::ExitCue:
      for (plUInt16 uiCue : segment.m_ExitCues)
      {
        const plUInt64 uiCueFrame = GetFrameOfBeat(*pPlaced, uiCue * fBeatsPerBar);
        if (uiCueFrame >= uiEarliest && uiCueFrame < uiFrame)
          uiFrame = uiCueFrame;
      }
      break;

    default:
      break;
  }

  // a boundary past the segment's end is its end, where the next grid begins
  return plMath::Clamp(uiFrame, uiEarliest, plMath::Max(pPlaced->m_uiEnd, uiEarliest));
}

void plAudioMusicPlayer::Place(plUInt16 uiSegment, plUInt16 uiState, plUInt16 uiCycle, bool bTransition, plUInt64 uiStart, plUInt64 uiOffset, plDynamicArray<StemPlay>& out_plays)
{
  const plAudioMusicSegmentDesc& segment = m_pDesc->m_Segments[uiSegment];

  Placed& placed = m_Placed.ExpandAndGetRef();
  placed.m_uiSegment = uiSegment;
  placed.m_uiState = uiState;
  placed.m_uiCycle = uiCycle;
  placed.m_bTransition = bTransition;
  placed.m_fGridOrigin = static_cast<double>(uiStart) - static_cast<double>(uiOffset);
  placed.m_uiStart = uiStart;
  placed.m_uiEnd = plMath::Max(GetFrameOfBeat(placed, segment.GetBeatCount()), uiStart + 1);
  placed.m_uiMusicalEnd = placed.m_uiEnd;

  // the first beat at or after the start: a segment that starts at a later position skips the beats before it
  placed.m_uiBeatsDelivered = static_cast<plUInt64>(plMath::Ceil(static_cast<double>(uiOffset) / GetBeatFrames(segment) - 1e-9));

  for (plUInt8 uiStem : segment.m_Stems)
  {
    StemPlay& play = out_plays.ExpandAndGetRef();
    play.m_uiInstrument = uiStem;
    play.m_uiStartFrame = uiStart;
    play.m_uiOffsetFrames = uiOffset;
    play.m_uiEndFrame = placed.m_uiEnd;
  }
}

void plAudioMusicPlayer::PlaceNext(plDynamicArray<StemPlay>& out_plays)
{
  const Placed last = m_Placed.PeekBack();

  // after a transition's segment, the target state begins; otherwise the state goes on to its next segment
  const plUInt16 uiCycle = last.m_bTransition ? 0 : static_cast<plUInt16>((last.m_uiCycle + 1) % m_pDesc->m_States[last.m_uiState].m_Segments.GetCount());
  const plUInt16 uiSegment = m_pDesc->m_States[last.m_uiState].m_Segments[uiCycle];

  Place(uiSegment, last.m_uiState, uiCycle, false, last.m_uiMusicalEnd, 0, out_plays);
}

void plAudioMusicPlayer::CommitChange(plDynamicArray<StemPlay>& out_plays, plDynamicArray<StemCut>& out_cuts)
{
  const plUInt64 uiLanding = m_Change.m_uiFrame;
  const plAudioMusicTransitionDesc& transition = m_Change.m_Transition;
  const plUInt16 uiFrom = m_uiTargetState;
  const plUInt16 uiTo = m_Change.m_uiTo;

  m_bChangePending = false;
  m_uiTargetState = uiTo;

  // the segment that plays at the landing decides the fade's beat length and the position a same-position change keeps
  const Placed* pAt = GetPlacedAt(uiLanding);
  const double fFadeFrames = (pAt != nullptr) ? plMath::Max(transition.m_fFadeBeats, 0.0f) * GetBeatFrames(m_pDesc->m_Segments[pAt->m_uiSegment]) : 0.0;
  const plUInt64 uiFade = static_cast<plUInt64>(fFadeFrames + 0.5);
  const double fPosition = (pAt != nullptr) ? static_cast<double>(uiLanding) - pAt->m_fGridOrigin : 0.0;

  // the music placed so far ends at the landing: what started before fades out from there, what would start later goes
  for (plUInt32 i = m_Placed.GetCount(); i > 0; --i)
  {
    Placed& placed = m_Placed[i - 1];
    if (placed.m_uiMusicalEnd <= uiLanding)
      continue;

    const plAudioMusicSegmentDesc& segment = m_pDesc->m_Segments[placed.m_uiSegment];
    const bool bNotStarted = placed.m_uiStart >= uiLanding;
    const plUInt64 uiStemEnd = bNotStarted ? placed.m_uiStart : plMath::Min(uiLanding + uiFade, placed.m_uiEnd);

    for (plUInt8 uiStem : segment.m_Stems)
    {
      StemCut& cut = out_cuts.ExpandAndGetRef();
      cut.m_uiInstrument = uiStem;
      cut.m_uiFrame = uiStemEnd;
      cut.m_uiFadeFrames = bNotStarted ? 0 : uiStemEnd - uiLanding;
    }

    if (bNotStarted)
      m_Placed.RemoveAtAndCopy(i - 1);
    else
      placed.m_uiMusicalEnd = uiLanding;
  }

  plStringBuilder sText;
  sText.SetFormat("{} -> {}", m_pDesc->m_States[uiFrom].m_sName, m_pDesc->m_States[uiTo].m_sName);
  AddMessage(uiLanding, Message::Type::Transition, sText);

  if (transition.m_uiVia < m_pDesc->m_Segments.GetCount())
  {
    Place(transition.m_uiVia, uiTo, 0, true, uiLanding, 0, out_plays);
    return;
  }

  const plAudioMusicStateDesc& state = m_pDesc->m_States[uiTo];
  if (state.m_Segments.IsEmpty())
    return;

  // the target's first segment, at the position the music was at when it keeps the position
  const plUInt16 uiSegment = state.m_Segments[0];
  plUInt64 uiOffset = 0;

  if (transition.m_eStartAt == plAudioMusicStartAt::SamePosition && fPosition > 0.0)
  {
    const plAudioMusicSegmentDesc& target = m_pDesc->m_Segments[uiSegment];
    const double fLength = target.GetLengthSeconds() * m_uiSampleRate;
    uiOffset = static_cast<plUInt64>(fPosition - plMath::Floor(fPosition / fLength) * fLength + 0.5);
  }

  Place(uiSegment, uiTo, 0, false, uiLanding, uiOffset, out_plays);
}

void plAudioMusicPlayer::Update(plUInt64 uiNow, plUInt64 uiHorizon, plDynamicArray<StemPlay>& out_plays, plDynamicArray<StemCut>& out_cuts)
{
  if (m_pDesc == nullptr)
    return;

  m_uiHorizon = uiHorizon;

  // segments that ended before now go; the one playing now stays
  while (m_Placed.GetCount() > 1 && m_Placed[0].m_uiMusicalEnd <= uiNow && m_Placed[1].m_uiStart <= uiNow)
  {
    m_Placed.RemoveAtAndCopy(0);
  }

  if (m_Placed.IsEmpty())
  {
    const plAudioMusicStateDesc& state = m_pDesc->m_States[m_uiTargetState];
    if (state.m_Segments.IsEmpty())
      return;

    Place(state.m_Segments[0], m_uiTargetState, 0, false, m_uiStartFrame, 0, out_plays);
  }

  // A change commits before the segment it lands in is continued, so nothing is placed behind its landing. The landing
  // is decided once the placed music reaches the horizon, on the grid of the segment that plays there.
  for (plUInt32 uiGuard = 0; uiGuard < 64; ++uiGuard)
  {
    if (m_bChangePending && m_Change.m_uiFrame != 0 && m_Change.m_uiFrame <= uiHorizon)
    {
      CommitChange(out_plays, out_cuts);
      continue;
    }

    if (m_Placed.IsEmpty())
      break;

    if (m_Placed.PeekBack().m_uiMusicalEnd <= uiHorizon)
    {
      PlaceNext(out_plays);
      continue;
    }

    if (m_bChangePending && m_Change.m_uiFrame == 0)
    {
      m_Change.m_uiFrame = plMath::Max<plUInt64>(GetLanding(m_Change.m_Transition.m_eChangeAt, uiHorizon), 1);
      continue;
    }

    break;
  }

  // stingers land on the grid of what plays at the horizon
  for (plUInt32 i = 0; i < m_Stingers.GetCount();)
  {
    PendingStinger& pending = m_Stingers[i];
    const plAudioMusicStingerDesc& stinger = m_pDesc->m_Stingers[pending.m_uiStinger];

    if (pending.m_uiFrame == 0)
      pending.m_uiFrame = plMath::Max<plUInt64>(GetLanding(stinger.m_eAt, uiHorizon), 1);

    if (pending.m_uiFrame > uiHorizon)
    {
      ++i;
      continue;
    }

    StemPlay& play = out_plays.ExpandAndGetRef();
    play.m_uiInstrument = stinger.m_uiInstrument;
    play.m_uiStartFrame = pending.m_uiFrame;

    AddMessage(pending.m_uiFrame, Message::Type::Stinger, stinger.m_sName);
    m_Stingers.RemoveAtAndCopy(i);
  }

  AddBeatMessages(uiHorizon);
}

void plAudioMusicPlayer::AddMessage(plUInt64 uiFrame, Message::Type type, plStringView sText, plUInt16 uiBar, plUInt8 uiBeat)
{
  // in frame order; at the same frame, in the order they were added
  plUInt32 uiInsert = m_Messages.GetCount();
  while (uiInsert > 0 && m_Messages[uiInsert - 1].m_uiFrame > uiFrame)
    --uiInsert;

  Message message;
  message.m_uiFrame = uiFrame;
  message.m_Type = type;
  message.m_sText = sText;
  message.m_uiBar = uiBar;
  message.m_uiBeat = uiBeat;
  m_Messages.InsertAt(uiInsert, std::move(message));
}

void plAudioMusicPlayer::AddBeatMessages(plUInt64 uiFrame)
{
  if (!m_pDesc->m_bBeatMessages)
    return;

  for (Placed& placed : m_Placed)
  {
    const plAudioMusicSegmentDesc& segment = m_pDesc->m_Segments[placed.m_uiSegment];
    const plUInt8 uiBeatsPerBar = plMath::Max<plUInt8>(segment.m_uiBeatsPerBar, 1);

    for (;;)
    {
      const plUInt64 uiBeatFrame = GetFrameOfBeat(placed, static_cast<double>(placed.m_uiBeatsDelivered));
      if (uiBeatFrame >= placed.m_uiMusicalEnd || uiBeatFrame > uiFrame)
        break;

      const plUInt16 uiBar = static_cast<plUInt16>(placed.m_uiBeatsDelivered / uiBeatsPerBar + 1);
      const plUInt8 uiBeat = static_cast<plUInt8>(placed.m_uiBeatsDelivered % uiBeatsPerBar + 1);
      AddMessage(uiBeatFrame, Message::Type::Beat, segment.m_sName, uiBar, uiBeat);
      ++placed.m_uiBeatsDelivered;
    }
  }
}

void plAudioMusicPlayer::TakeMessages(plUInt64 uiNow, plDynamicArray<Message>& out_messages)
{
  plUInt32 uiDue = 0;
  while (uiDue < m_Messages.GetCount() && m_Messages[uiDue].m_uiFrame <= uiNow)
  {
    out_messages.PushBack(m_Messages[uiDue]);
    ++uiDue;
  }

  if (uiDue > 0)
    m_Messages.RemoveAtAndCopy(0, uiDue);
}

plAudioMusicPlayer::Position plAudioMusicPlayer::GetPosition(plUInt64 uiFrame) const
{
  Position position;
  if (m_pDesc == nullptr)
    return position;

  if (m_bChangePending)
  {
    position.m_uiPendingState = m_Change.m_uiTo;
    position.m_uiPendingFrame = m_Change.m_uiFrame;
  }

  const Placed* pPlaced = GetPlacedAt(uiFrame);
  if (pPlaced == nullptr)
    return position;

  const plAudioMusicSegmentDesc& segment = m_pDesc->m_Segments[pPlaced->m_uiSegment];
  const plUInt8 uiBeatsPerBar = plMath::Max<plUInt8>(segment.m_uiBeatsPerBar, 1);
  const double fBeats = plMath::Max((static_cast<double>(uiFrame) - pPlaced->m_fGridOrigin) / GetBeatFrames(segment), 0.0);
  const plUInt64 uiBeat = plMath::Min(static_cast<plUInt64>(fBeats + 1e-9), static_cast<plUInt64>(segment.GetBeatCount() - 1));

  position.m_uiState = pPlaced->m_uiState;
  position.m_uiSegment = pPlaced->m_uiSegment;
  position.m_bTransitionSegment = pPlaced->m_bTransition;
  position.m_uiBar = static_cast<plUInt16>(uiBeat / uiBeatsPerBar + 1);
  position.m_uiBeat = static_cast<plUInt8>(uiBeat % uiBeatsPerBar + 1);
  position.m_fTempo = segment.m_fTempo;
  position.m_uiBeatsPerBar = uiBeatsPerBar;
  return position;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Music_AudioMusicPlayer);
