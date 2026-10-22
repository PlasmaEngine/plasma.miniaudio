#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <AudioSystem/Core/AudioMiddleware.h>
#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioPlugin/Music/AudioMusicPlayer.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

PL_CREATE_SIMPLE_TEST_GROUP(Music);

namespace
{
  constexpr plUInt32 FramesPerSecond = plOfflineAudioEngine::SampleRate;

  /// 120 BPM at 48 kHz
  constexpr plUInt64 Beat = FramesPerSecond / 2;
  constexpr plUInt64 Bar = 4 * Beat;

  plAudioMusicSegmentDesc& AddSegment(plAudioMusicDesc& ref_music, plStringView sName, plUInt16 uiBars, plUInt8 uiStem, float fTempo = 120.0f)
  {
    plAudioMusicSegmentDesc& segment = ref_music.m_Segments.ExpandAndGetRef();
    segment.m_sName = sName;
    segment.m_fTempo = fTempo;
    segment.m_uiBeatsPerBar = 4;
    segment.m_uiBars = uiBars;
    segment.m_Stems.PushBack(uiStem);
    return segment;
  }

  plAudioMusicStateDesc& AddState(plAudioMusicDesc& ref_music, plStringView sName, std::initializer_list<plUInt16> segments)
  {
    plAudioMusicStateDesc& state = ref_music.m_States.ExpandAndGetRef();
    state.m_sName = sName;
    for (plUInt16 uiSegment : segments)
      state.m_Segments.PushBack(uiSegment);
    return state;
  }

  plAudioMusicTransitionDesc& AddTransition(plAudioMusicDesc& ref_music, plUInt16 uiFrom, plUInt16 uiTo, plAudioMusicQuantize::Enum changeAt)
  {
    plAudioMusicTransitionDesc& transition = ref_music.m_Transitions.ExpandAndGetRef();
    transition.m_uiFrom = uiFrom;
    transition.m_uiTo = uiTo;
    transition.m_eChangeAt = changeAt;
    return transition;
  }

  /// Runs a player in blocks the way an event instance does: horizon = now + two blocks.
  struct PlayerRun
  {
    plAudioMusicPlayer m_Player;
    plDynamicArray<plAudioMusicPlayer::StemPlay> m_Plays;
    plDynamicArray<plAudioMusicPlayer::StemCut> m_Cuts;
    plDynamicArray<plAudioMusicPlayer::Message> m_Messages;
    plUInt64 m_uiNow = 0;

    void Start(const plAudioMusicDesc& music, plUInt16 uiState = 0) { m_Player.Start(&music, 0, FramesPerSecond, uiState); }

    void RunTo(plUInt64 uiFrame)
    {
      while (m_uiNow < uiFrame)
      {
        m_Player.Update(m_uiNow, m_uiNow + plOfflineAudioEngine::LookAheadFrames, m_Plays, m_Cuts);
        m_Player.TakeMessages(m_uiNow, m_Messages);
        m_uiNow += plOfflineAudioEngine::BlockFrames;
      }
    }

    const plAudioMusicPlayer::StemPlay* FindPlay(plUInt8 uiStem, plUInt64 uiStart) const
    {
      for (const auto& play : m_Plays)
      {
        if (play.m_uiInstrument == uiStem && play.m_uiStartFrame == uiStart)
          return &play;
      }
      return nullptr;
    }

    plUInt32 CountPlays(plUInt8 uiStem) const
    {
      plUInt32 uiCount = 0;
      for (const auto& play : m_Plays)
        uiCount += play.m_uiInstrument == uiStem ? 1 : 0;
      return uiCount;
    }
  };

  /// Two states: A (segment "A1", 2 bars, stem 0) and B (segment "B1", 2 bars, stem 1).
  plAudioMusicDesc MakeTwoStates()
  {
    plAudioMusicDesc music;
    AddSegment(music, "A1", 2, 0);
    AddSegment(music, "B1", 2, 1);
    AddState(music, "A", {0});
    AddState(music, "B", {1});
    return music;
  }

  /// A music event: one instrument per stem, each playing the embedded sample of the same index.
  plAudioEventResourceDescriptor MakeMusicEvent(const plAudioMusicDesc& music, plArrayPtr<const plDynamicArray<float>> stems)
  {
    plAudioEventResourceDescriptor res;
    plAudioEventDesc& desc = res.m_Desc;
    desc.m_bSpatialize = false;
    desc.m_FadeIn = plTime::MakeZero();
    desc.m_StopFadeOut = plTime::MakeZero();
    desc.m_uiPriority = 255;

    for (plUInt32 i = 0; i < stems.GetCount(); ++i)
    {
      plAudioEventInstrumentDesc& instrument = desc.m_Instruments.ExpandAndGetRef();
      plStringBuilder sName;
      sName.SetFormat("Stem {}", i);
      instrument.m_sName = sName;

      plAudioEventPlaylistEntryDesc& entry = instrument.m_Playlist.ExpandAndGetRef();
      entry.m_uiSample = static_cast<plUInt16>(i);

      desc.m_SampleNames.PushBack(instrument.m_sName);
      res.m_SampleData.PushBack(plAudioTestUtils::MakeWavF32(stems[i], plOfflineAudioEngine::SampleRate));
    }

    plAudioEventParameterDesc& state = desc.m_Parameters.ExpandAndGetRef();
    state.m_sName = plAudioMusicDesc::StateParameter;
    state.m_fMin = 0.0f;
    state.m_fMax = static_cast<float>(plMath::Max(music.m_States.GetCount(), 1u) - 1);

    desc.m_Music = music;
    return res;
  }

  /// A ramp from 0 to fTo over uiFrames: a sample's value says where in the stem it is.
  plDynamicArray<float> MakeRamp(plUInt32 uiFrames, float fTo)
  {
    plDynamicArray<float> samples;
    samples.SetCountUninitialized(uiFrames);
    for (plUInt32 i = 0; i < uiFrames; ++i)
      samples[i] = fTo * static_cast<float>(i) / static_cast<float>(uiFrames);
    return samples;
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// The scheduler, frame by frame

PL_CREATE_SIMPLE_TEST(Music, SegmentsFollowGaplessly)
{
  plAudioMusicDesc music;
  AddSegment(music, "A1", 2, 0);
  AddSegment(music, "A2", 1, 1);
  AddState(music, "A", {0, 1});

  PlayerRun run;
  run.Start(music);

  // the player keeps the segments that play now and later: positions are asked for while their segment plays
  run.RunTo(2 * Bar + Beat + 1);
  const plAudioMusicPlayer::Position position = run.m_Player.GetPosition(2 * Bar + Beat);
  PL_TEST_INT(position.m_uiSegment, 1);
  PL_TEST_INT(position.m_uiBar, 1);
  PL_TEST_INT(position.m_uiBeat, 2);

  run.RunTo(5 * Bar);

  const auto* pFirst = run.FindPlay(0, 0);
  const auto* pSecond = run.FindPlay(1, 2 * Bar);
  const auto* pAgain = run.FindPlay(0, 3 * Bar);

  if (PL_TEST_BOOL(pFirst != nullptr && pSecond != nullptr && pAgain != nullptr))
  {
    PL_TEST_INT(pFirst->m_uiEndFrame, 2 * Bar);
    PL_TEST_INT(pSecond->m_uiEndFrame, 3 * Bar);
    PL_TEST_INT(pAgain->m_uiEndFrame, 5 * Bar);
  }

  PL_TEST_BOOL(run.m_Cuts.IsEmpty());
}

PL_CREATE_SIMPLE_TEST(Music, ChangeOnNextBar)
{
  plAudioMusicDesc music = MakeTwoStates();
  AddTransition(music, 0, 1, plAudioMusicQuantize::NextBar);

  PlayerRun run;
  run.Start(music);
  run.RunTo(10000);
  run.m_Player.RequestState(1);

  // the change is decided at once and lands on the next bar
  run.RunTo(run.m_uiNow + plOfflineAudioEngine::BlockFrames);
  PL_TEST_INT(run.m_Player.GetPosition(run.m_uiNow).m_uiPendingFrame, Bar);

  run.RunTo(3 * Bar);

  if (PL_TEST_INT(run.m_Cuts.GetCount(), 1))
  {
    PL_TEST_INT(run.m_Cuts[0].m_uiInstrument, 0);
    PL_TEST_INT(run.m_Cuts[0].m_uiFrame, Bar);
    PL_TEST_INT(run.m_Cuts[0].m_uiFadeFrames, 0);
  }

  const auto* pB = run.FindPlay(1, Bar);
  if (PL_TEST_BOOL(pB != nullptr))
    PL_TEST_INT(pB->m_uiEndFrame, 3 * Bar);

  bool bMessage = false;
  for (const auto& message : run.m_Messages)
    bMessage |= message.m_Type == plAudioMusicPlayer::Message::Type::Transition && message.m_uiFrame == Bar && message.m_sText == "A -> B";
  PL_TEST_BOOL(bMessage);

  PL_TEST_INT(run.m_Player.GetTargetState(), 1);
  PL_TEST_INT(run.m_Player.GetPosition(2 * Bar).m_uiState, 1);
}

PL_CREATE_SIMPLE_TEST(Music, FadeViaAndExitCue)
{
  // fade over two beats from the change
  {
    plAudioMusicDesc music = MakeTwoStates();
    AddTransition(music, 0, 1, plAudioMusicQuantize::NextBar).m_fFadeBeats = 2.0f;

    PlayerRun run;
    run.Start(music);
    run.RunTo(10000);
    run.m_Player.RequestState(1);
    run.RunTo(2 * Bar);

    if (PL_TEST_INT(run.m_Cuts.GetCount(), 1))
    {
      PL_TEST_INT(run.m_Cuts[0].m_uiFrame, Bar + 2 * Beat);
      PL_TEST_INT(run.m_Cuts[0].m_uiFadeFrames, 2 * Beat);
    }
    PL_TEST_BOOL(run.FindPlay(1, Bar) != nullptr);
  }

  // a transition segment of one bar plays first, then the target from its start
  {
    plAudioMusicDesc music = MakeTwoStates();
    AddSegment(music, "Intro", 1, 2);
    AddTransition(music, 0, 1, plAudioMusicQuantize::NextBar).m_uiVia = 2;

    PlayerRun run;
    run.Start(music);
    run.RunTo(10000);
    run.m_Player.RequestState(1);
    run.RunTo(Bar + Beat + 1);
    PL_TEST_BOOL(run.m_Player.GetPosition(Bar + Beat).m_bTransitionSegment);
    run.RunTo(3 * Bar);

    PL_TEST_BOOL(run.FindPlay(2, Bar) != nullptr);
    PL_TEST_BOOL(run.FindPlay(1, 2 * Bar) != nullptr);
    PL_TEST_INT(run.CountPlays(1), 1);
  }

  // exit cues: the next cue ahead, else the segment's end
  {
    plAudioMusicDesc music;
    AddSegment(music, "A1", 4, 0).m_ExitCues.PushBack(2);
    AddSegment(music, "B1", 2, 1);
    AddState(music, "A", {0});
    AddState(music, "B", {1});
    AddTransition(music, 0, 1, plAudioMusicQuantize::ExitCue);

    PlayerRun early;
    early.Start(music);
    early.RunTo(Bar / 2);
    early.m_Player.RequestState(1);
    early.RunTo(3 * Bar);
    PL_TEST_BOOL(early.FindPlay(1, 2 * Bar) != nullptr);

    PlayerRun late;
    late.Start(music);
    late.RunTo(2 * Bar + Beat);
    late.m_Player.RequestState(1);
    late.RunTo(5 * Bar);
    PL_TEST_BOOL(late.FindPlay(1, 4 * Bar) != nullptr);
    PL_TEST_INT(late.CountPlays(1), 1);
  }
}

PL_CREATE_SIMPLE_TEST(Music, SamePositionTempoAndCancel)
{
  // the target starts where the music is: five beats into its stem
  {
    plAudioMusicDesc music = MakeTwoStates();
    AddTransition(music, 0, 1, plAudioMusicQuantize::NextBeat).m_eStartAt = plAudioMusicStartAt::SamePosition;

    PlayerRun run;
    run.Start(music);
    run.RunTo(4 * Beat + 1000);
    run.m_Player.RequestState(1);
    run.RunTo(5 * Beat + 1);

    // its beats count from the position it started at
    const plAudioMusicPlayer::Position position = run.m_Player.GetPosition(5 * Beat);
    PL_TEST_INT(position.m_uiState, 1);
    PL_TEST_INT(position.m_uiBar, 2);
    PL_TEST_INT(position.m_uiBeat, 2);

    run.RunTo(3 * Bar);

    const auto* pB = run.FindPlay(1, 5 * Beat);
    if (PL_TEST_BOOL(pB != nullptr))
    {
      PL_TEST_INT(pB->m_uiOffsetFrames, 5 * Beat);
      PL_TEST_INT(pB->m_uiEndFrame, 2 * Bar);
    }
  }

  // a segment of another tempo ends on its own grid
  {
    plAudioMusicDesc music;
    AddSegment(music, "A1", 2, 0);
    AddSegment(music, "B1", 1, 1, 140.0f);
    AddState(music, "A", {0});
    AddState(music, "B", {1});
    AddTransition(music, 0, 1, plAudioMusicQuantize::NextBar);

    PlayerRun run;
    run.Start(music);
    run.RunTo(10000);
    run.m_Player.RequestState(1);
    run.RunTo(3 * Bar);

    const plUInt64 uiBar140 = static_cast<plUInt64>(4.0 * 60.0 / 140.0 * FramesPerSecond + 0.5);
    const auto* pB = run.FindPlay(1, Bar);
    if (PL_TEST_BOOL(pB != nullptr))
      PL_TEST_INT(pB->m_uiEndFrame, Bar + uiBar140);
    PL_TEST_BOOL(run.FindPlay(1, Bar + uiBar140) != nullptr);
  }

  // asking for the state the music plays before the change landed drops it
  {
    plAudioMusicDesc music = MakeTwoStates();

    PlayerRun run;
    run.Start(music);
    run.RunTo(10000);
    run.m_Player.RequestState(1);
    run.RunTo(20000);
    run.m_Player.RequestState(0);
    run.RunTo(3 * Bar);

    PL_TEST_BOOL(run.m_Cuts.IsEmpty());
    PL_TEST_INT(run.CountPlays(1), 0);
  }
}

PL_CREATE_SIMPLE_TEST(Music, StingersAndBeats)
{
  plAudioMusicDesc music = MakeTwoStates();
  music.m_bBeatMessages = true;

  plAudioMusicStingerDesc& stinger = music.m_Stingers.ExpandAndGetRef();
  stinger.m_sName = "Hit";
  stinger.m_uiInstrument = 3;
  stinger.m_eAt = plAudioMusicQuantize::NextBeat;

  PlayerRun run;
  run.Start(music);
  run.RunTo(1000);
  run.m_Player.RequestStinger(0);
  run.RunTo(Bar + Beat);

  const auto* pHit = run.FindPlay(3, Beat);
  if (PL_TEST_BOOL(pHit != nullptr))
    PL_TEST_INT(pHit->m_uiEndFrame, 0);

  // beats 1.1 to 2.1 (the run stops before 2.2) and the stinger
  plHybridArray<plUInt64, 8> beatFrames;
  bool bStinger = false;
  for (const auto& message : run.m_Messages)
  {
    if (message.m_Type == plAudioMusicPlayer::Message::Type::Beat)
      beatFrames.PushBack(message.m_uiFrame);
    bStinger |= message.m_Type == plAudioMusicPlayer::Message::Type::Stinger && message.m_uiFrame == Beat;
  }

  PL_TEST_BOOL(bStinger);
  if (PL_TEST_INT(beatFrames.GetCount(), 5))
  {
    for (plUInt32 i = 0; i < beatFrames.GetCount(); ++i)
      PL_TEST_INT(beatFrames[i], i * Beat);
  }

  const plAudioMusicPlayer::Message* pBar2 = nullptr;
  for (const auto& message : run.m_Messages)
  {
    if (message.m_Type == plAudioMusicPlayer::Message::Type::Beat && message.m_uiFrame == Bar)
      pBar2 = &message;
  }
  if (PL_TEST_BOOL(pBar2 != nullptr))
  {
    PL_TEST_INT(pBar2->m_uiBar, 2);
    PL_TEST_INT(pBar2->m_uiBeat, 1);
    PL_TEST_STRING(pBar2->m_sText, "A1");
  }

  // messages are handed out once the engine reached them
  for (const auto& message : run.m_Messages)
    PL_TEST_BOOL(message.m_uiFrame < run.m_uiNow);
}

//////////////////////////////////////////////////////////////////////////
// Played through an event instance

PL_CREATE_SIMPLE_TEST(Music, SwitchIsSampleAccurate)
{
  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plAudioMusicDesc music = MakeTwoStates();
  music.m_bBeatMessages = true;
  AddTransition(music, 0, 1, plAudioMusicQuantize::NextBar);

  const plDynamicArray<float> stems[] = {plAudioTestUtils::MakeDc(2 * Bar, 0.5f), plAudioTestUtils::MakeDc(2 * Bar, 0.25f)};
  const plStringView sEventId = "PlasmaAudioTest:Music_Switch";
  plAudioTestUtils::CreateEventResource(sEventId, MakeMusicEvent(music, stems));

  plPlasmaAudioEventData eventData;
  eventData.m_bLooping = true;
  plPlasmaAudioEventManager manager;
  PL_TEST_RESULT(manager.ActivateEvent(sEventId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

  plDynamicArray<float> output;
  engine.RenderBlocks(20, &manager, &output);
  manager.SetEventParameter(nullptr, plAudioMusicDesc::StateParameter, 1.0f);
  engine.RenderBlocks(300, &manager, &output);

  plAudioMusicPlayer::Position position;
  PL_TEST_BOOL(manager.GetMusicPosition(&eventData, position));
  PL_TEST_INT(position.m_uiState, 1);

  manager.StopAll(true);

  // Every voice passes the occlusion low-pass, which delays it by a couple of frames and rings at an onset: the delay
  // is measured at A's start (frame 0), and the switch is checked against it. A plays up to the bar, fades out over the
  // last 3 ms before it, and B starts on the bar's frame.
  const plUInt32 uiDelay = plAudioTestUtils::FindFirstAbove(output, 0.2f);
  const plUInt32 uiBar = static_cast<plUInt32>(Bar) + uiDelay;
  const plUInt32 uiFade = FramesPerSecond * 3 / 1000;
  const plUInt32 uiSettle = 40;

  PL_TEST_BOOL(uiDelay < 8);
  PL_TEST_FLOAT(output[uiBar - uiFade - uiSettle], 0.5f, 1e-3f);
  PL_TEST_BOOL(output[uiBar - 1] < 0.02f);
  PL_TEST_INT(plAudioTestUtils::FindFirstAbove(output, 0.15f, uiBar - 1), uiBar);
  PL_TEST_FLOAT(output[uiBar + uiSettle], 0.25f, 1e-3f);

  // nothing of A behind the bar
  PL_TEST_FLOAT(output[uiBar + 2 * static_cast<plUInt32>(Beat)], 0.25f, 1e-3f);

  plDynamicArray<plString> markers;
  manager.TakeMarkers(&eventData, markers);

  // B's beats count from its start: its second beat is one beat behind the bar
  plStringBuilder sBeat;
  sBeat.SetFormat("{}1:2:B1", plAudioSystemReservedMarker::BeatPrefix);
  PL_TEST_BOOL(markers.Contains(plString("A -> B")));
  PL_TEST_BOOL(markers.Contains(plString(sBeat)));
}

PL_CREATE_SIMPLE_TEST(Music, SamePositionAndStingerInAudio)
{
  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plAudioMusicDesc music = MakeTwoStates();
  AddTransition(music, 0, 1, plAudioMusicQuantize::NextBeat).m_eStartAt = plAudioMusicStartAt::SamePosition;

  plAudioMusicStingerDesc& stinger = music.m_Stingers.ExpandAndGetRef();
  stinger.m_sName = "Hit";
  stinger.m_uiInstrument = 2;
  stinger.m_eAt = plAudioMusicQuantize::NextBeat;

  // B is a ramp, so its value tells where in the stem it plays
  const plDynamicArray<float> stems[] = {plAudioTestUtils::MakeDc(2 * Bar, 0.5f), MakeRamp(2 * Bar, 0.5f), plAudioTestUtils::MakeDc(Beat, 0.125f)};
  const plStringView sEventId = "PlasmaAudioTest:Music_SamePosition";
  plAudioTestUtils::CreateEventResource(sEventId, MakeMusicEvent(music, stems));

  plPlasmaAudioEventData eventData;
  eventData.m_bLooping = true;
  plPlasmaAudioEventManager manager;
  PL_TEST_RESULT(manager.ActivateEvent(sEventId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

  plDynamicArray<float> output;

  // the stinger lands on beat 2 over A
  engine.RenderBlocks(2, &manager, &output);
  manager.PlayStinger(nullptr, "hit");
  engine.RenderBlocks(200, &manager, &output);

  // the change lands on beat 6 and B continues from there
  manager.SetEventParameter(nullptr, plAudioMusicDesc::StateParameter, 1.0f);
  engine.RenderBlocks(100, &manager, &output);
  manager.StopAll(true);

  // the voices' low-pass delay, measured at A's start (see SwitchIsSampleAccurate)
  const plUInt32 uiDelay = plAudioTestUtils::FindFirstAbove(output, 0.2f);
  const plUInt32 uiSettle = 40;
  const plUInt32 uiBeat = static_cast<plUInt32>(Beat);

  PL_TEST_FLOAT(output[uiBeat + uiDelay - 1], 0.5f, 2e-2f);
  PL_TEST_INT(plAudioTestUtils::FindFirstAbove(output, 0.55f, uiBeat - 100), uiBeat + uiDelay);
  PL_TEST_FLOAT(output[uiBeat + uiDelay + uiSettle], 0.625f, 1e-3f);
  PL_TEST_FLOAT(output[2 * uiBeat + uiDelay + uiSettle], 0.5f, 1e-3f);

  // 202 blocks are 96960 frames: the next beat at or after the horizon is beat 5 (120000); B goes on from there
  const plUInt32 uiChange = 5 * uiBeat;
  PL_TEST_FLOAT(output[uiChange - 200], 0.5f, 1e-3f);
  PL_TEST_FLOAT(output[uiChange + uiDelay + uiSettle], 0.5f * (uiChange + uiSettle) / static_cast<float>(2 * Bar), 1e-3f);
  PL_TEST_FLOAT(output[uiChange + 1000], 0.5f * (uiChange + 1000 - uiDelay) / static_cast<float>(2 * Bar), 1e-3f);
}
