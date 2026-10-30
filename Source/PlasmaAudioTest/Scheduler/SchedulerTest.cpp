#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioPlugin/Mixer/MiniAudioDspNodes.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

PL_CREATE_SIMPLE_TEST_GROUP(Scheduler);

namespace
{
  constexpr float SilenceThreshold = 1e-6f;

  void Report(plStringView sText)
  {
    plStringBuilder sTmp = sText;
    plTestFramework::Output(plTestOutput::Message, "%s", sTmp.GetData());
  }

  /// \brief Checks the seams of a looping cosine cycle rendered from uiOnset on.
  ///
  /// A seam is clean when no sample-to-sample step around it is larger than the largest step inside the
  /// steady cosine (+1e-4). The reference skips the first frames after the onset, where the voice's filters
  /// settle from silence. Every step from there to the end of the last loop is checked as well, so a seam
  /// that is not where it is expected shows up too.
  void CheckLoopSeams(const plDynamicArray<float>& output, plUInt32 uiOnset, plUInt32 uiCycleFrames, plUInt32 uiLoops, plStringView sLabel)
  {
    constexpr plUInt32 uiSettleFrames = 200;
    constexpr plUInt32 uiSeamWindow = 16;

    if (!PL_TEST_BOOL_MSG(uiOnset != plInvalidIndex, "no output"))
      return;

    const plUInt32 uiEnd = uiOnset + uiLoops * uiCycleFrames;
    if (!PL_TEST_BOOL_MSG(uiEnd <= output.GetCount(), "rendered too little"))
      return;

    const float fInnerStep = plAudioTestUtils::MaxStep(output, uiOnset + uiSettleFrames, uiOnset + uiCycleFrames - uiSeamWindow);
    const float fAllowed = fInnerStep + 1e-4f;

    float fWorstSeam = 0.0f;
    for (plUInt32 k = 1; k < uiLoops; ++k)
    {
      const plUInt32 uiSeam = uiOnset + k * uiCycleFrames;
      fWorstSeam = plMath::Max(fWorstSeam, plAudioTestUtils::MaxStep(output, uiSeam - uiSeamWindow, uiSeam + uiSeamWindow));
    }

    const float fOverall = plAudioTestUtils::MaxStep(output, uiOnset + uiSettleFrames, uiEnd);
    plStringBuilder sMsg;
    sMsg.SetFormat("{}: onset {}, max inner step {}, worst seam step {}, max step over {} loops {}", sLabel, uiOnset, fInnerStep, fWorstSeam, uiLoops, fOverall);
    Report(sMsg);

    PL_TEST_BOOL_MSG(fWorstSeam <= fAllowed, "%s: seam step %f exceeds %f", sMsg.GetData(), fWorstSeam, fAllowed);
    PL_TEST_BOOL_MSG(fOverall <= fAllowed, "%s: step %f exceeds %f", sMsg.GetData(), fOverall, fAllowed);
  }
} // namespace

namespace
{
  /// \brief Activates a single-instrument event with the given start delay at engine frame 1000 and returns the
  /// first non-silent output frame, counted from the activation frame.
  ///
  /// uiLeadFrames frames are rendered (and updated) right after the activation, before the regular blocks,
  /// which moves every later block boundary by that amount relative to the voice's start frame.
  plUInt32 MeasureStartFrame(plStringView sEventId, plTime startDelay, plUInt32 uiLeadFrames)
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return plInvalidIndex;

    // a block-unaligned engine time at activation, so the delay has to be measured from the activation frame
    engine.Render(1000, nullptr);

    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(9600, 0.5f));
    res.m_Desc.m_Instruments[0].m_StartDelay = startDelay;
    const plAudioEventResourceHandle hEvent = plAudioTestUtils::CreateEventResource(sEventId, std::move(res));

    plPlasmaAudioEventData eventData;
    plPlasmaAudioEventManager manager;

    PL_TEST_INT(engine.GetTime(), 1000);
    PL_TEST_RESULT(manager.ActivateEvent(sEventId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));
    PL_TEST_BOOL(manager.IsEventPlaying(&eventData));

    plDynamicArray<float> output;
    if (uiLeadFrames > 0)
    {
      engine.Render(uiLeadFrames, &output);
      manager.Update(plTime::MakeFromSeconds(static_cast<double>(uiLeadFrames) / plOfflineAudioEngine::SampleRate), plVec3::MakeZero());
    }
    engine.RenderBlocks(30, &manager, &output);

    return plAudioTestUtils::FindFirstAbove(output, SilenceThreshold);
  }
} // namespace

namespace
{
  /// \brief Renders a -6 dB DC event with the given fade-in and returns the output level after the fade has ended.
  float MeasureSettledLevel(plStringView sEventId, plTime fadeIn)
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return 0.0f;

    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(48000, 0.5f));
    res.m_Desc.m_Instruments[0].m_fVolumeDb = -6.0f;
    res.m_Desc.m_FadeIn = fadeIn;
    const plAudioEventResourceHandle hEvent = plAudioTestUtils::CreateEventResource(sEventId, std::move(res));

    plPlasmaAudioEventData eventData;
    plPlasmaAudioEventManager manager;
    PL_TEST_RESULT(manager.ActivateEvent(sEventId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

    plDynamicArray<float> output;
    engine.RenderBlocks(30, &manager, &output);

    // 30 blocks = 14400 frames; the 20 ms fade ends near frame 960
    if (!PL_TEST_BOOL(output.GetCount() > 12000))
      return 0.0f;

    return output[12000];
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Scheduler, FadeInReachesVoiceVolume)
{
  // the fade-in envelope is multiplied with the voice volume, so after the fade an attenuated voice must sit at the
  // same level as the same voice without a fade (a fade that targets the voice volume would square it)
  const float fNoFade = MeasureSettledLevel("PlasmaAudioTest:FadeInLevel_None", plTime::MakeZero());
  const float fFaded = MeasureSettledLevel("PlasmaAudioTest:FadeInLevel_20ms", plTime::MakeFromMilliseconds(20));

  plStringBuilder sMsg;
  sMsg.SetFormat("FadeInReachesVoiceVolume: level without fade {}, after a 20 ms fade {}", fNoFade, fFaded);
  Report(sMsg);

  PL_TEST_BOOL(fNoFade > 0.01f);
  PL_TEST_FLOAT(fFaded, fNoFade, 1e-4f);
}

PL_CREATE_SIMPLE_TEST(Scheduler, StartDelayExact)
{
  // the voice -> bus -> endpoint chain delays every voice by a constant number of frames; a zero-delay
  // start measures it, so the delayed start can be checked for the exact 4800 frames on top
  const plUInt32 uiLatency = MeasureStartFrame("PlasmaAudioTest:StartDelay0", plTime::MakeZero(), 0);

  // 0: the start frame falls on a render block boundary; 7: it falls 7 frames before one
  const plUInt32 leadFrames[] = {0, 7};

  for (plUInt32 uiLead : leadFrames)
  {
    plStringBuilder sId;
    sId.SetFormat("PlasmaAudioTest:StartDelay100_{}", uiLead);
    const plUInt32 uiDelayed = MeasureStartFrame(sId, plTime::MakeFromMilliseconds(100), uiLead);

    plStringBuilder sMsg;
    sMsg.SetFormat("StartDelayExact (block offset {}): zero-delay start at +{} frames, 100 ms start at +{} frames, difference {} (expected 4800)", uiLead, uiLatency, uiDelayed, static_cast<plInt64>(uiDelayed) - static_cast<plInt64>(uiLatency));
    Report(sMsg);

    if (!PL_TEST_BOOL(uiLatency != plInvalidIndex && uiDelayed != plInvalidIndex))
      return;

    PL_TEST_INT(static_cast<plInt64>(uiDelayed) - static_cast<plInt64>(uiLatency), 4800);
  }
}

PL_CREATE_SIMPLE_TEST(Scheduler, LoopSeam)
{
  // 48 whole periods of 100 frames: the sample ends exactly where it starts
  constexpr plUInt32 uiCycleFrames = 4800;
  constexpr plUInt32 uiLoops = 5;
  constexpr plUInt32 uiBlocks = (uiLoops + 1) * uiCycleFrames / plOfflineAudioEngine::BlockFrames + 2;

  const plDynamicArray<float> cycle = plAudioTestUtils::MakeCosineCycle(uiCycleFrames, 48, 0.5f);

  PL_TEST_BLOCK(plTestBlock::Enabled, "Loop region")
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(cycle);
    res.m_Desc.m_bLoopRegion = true;
    res.m_Desc.m_LoopStart = plTime::MakeZero();
    res.m_Desc.m_LoopEnd = plTime::MakeFromSeconds(static_cast<double>(uiCycleFrames) / plOfflineAudioEngine::SampleRate);
    const plAudioEventResourceHandle hEvent = plAudioTestUtils::CreateEventResource("PlasmaAudioTest:LoopRegion", std::move(res));

    plPlasmaAudioEventData eventData;
    eventData.m_bLooping = true;
    plPlasmaAudioEventManager manager;

    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:LoopRegion", nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

    plDynamicArray<float> output;
    engine.RenderBlocks(uiBlocks, &manager, &output);

    CheckLoopSeams(output, plAudioTestUtils::FindFirstAbove(output, SilenceThreshold), uiCycleFrames, uiLoops, "LoopSeam/Loop region");
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Gapless instrument loop")
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(cycle);
    res.m_Desc.m_Instruments[0].m_bLoop = true;
    const plAudioEventResourceHandle hEvent = plAudioTestUtils::CreateEventResource("PlasmaAudioTest:GaplessLoop", std::move(res));

    plPlasmaAudioEventData eventData;
    eventData.m_bLooping = true;
    plPlasmaAudioEventManager manager;

    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:GaplessLoop", nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

    plDynamicArray<float> output;
    engine.RenderBlocks(uiBlocks, &manager, &output);

    CheckLoopSeams(output, plAudioTestUtils::FindFirstAbove(output, SilenceThreshold), uiCycleFrames, uiLoops, "LoopSeam/Gapless instrument loop");
  }

  // two playlist entries disable the decoder loop: every pass is a new voice scheduled at the previous voice's end frame.
  // 4800 frames end on a render block boundary, 4100 frames (41 periods) end inside a block.
  const plUInt32 scheduledCycles[][2] = {{4800, 48}, {4100, 41}};

  for (const auto& cfg : scheduledCycles)
  {
    const plUInt32 uiFrames = cfg[0];

    plStringBuilder sLabel;
    sLabel.SetFormat("LoopSeam/Scheduled re-trigger, {} frames", uiFrames);

    PL_TEST_BLOCK(plTestBlock::Enabled, sLabel.GetData())
    {
      plOfflineAudioEngine engine;
      if (!PL_TEST_BOOL(engine.IsValid()))
        return;

      plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeCosineCycle(uiFrames, cfg[1], 0.5f));
      res.m_Desc.m_Instruments[0].m_bLoop = true;
      const plAudioEventPlaylistEntryDesc entry = res.m_Desc.m_Instruments[0].m_Playlist[0];
      res.m_Desc.m_Instruments[0].m_Playlist.PushBack(entry);

      plStringBuilder sId;
      sId.SetFormat("PlasmaAudioTest:ScheduledLoop{}", uiFrames);
      const plAudioEventResourceHandle hEvent = plAudioTestUtils::CreateEventResource(sId, std::move(res));

      plPlasmaAudioEventData eventData;
      eventData.m_bLooping = true;
      plPlasmaAudioEventManager manager;

      PL_TEST_RESULT(manager.ActivateEvent(sId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

      plDynamicArray<float> output;
      engine.RenderBlocks((uiLoops + 1) * uiFrames / plOfflineAudioEngine::BlockFrames + 2, &manager, &output);

      CheckLoopSeams(output, plAudioTestUtils::FindFirstAbove(output, SilenceThreshold), uiFrames, uiLoops, sLabel);
    }
  }
}

namespace
{
  void RunScattererGap(plUInt32 uiSampleFrames)
  {
    constexpr plUInt32 uiExpectedGap = 2400; // 50 ms at 48 kHz

    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(uiSampleFrames, 0.5f));
    res.m_Desc.m_Instruments[0].m_bLoop = true;
    res.m_Desc.m_Instruments[0].m_MinRetriggerDelay = plTime::MakeFromMilliseconds(50);
    res.m_Desc.m_Instruments[0].m_MaxRetriggerDelay = plTime::MakeFromMilliseconds(50);

    plStringBuilder sId;
    sId.SetFormat("PlasmaAudioTest:Scatterer{}", uiSampleFrames);
    const plAudioEventResourceHandle hEvent = plAudioTestUtils::CreateEventResource(sId, std::move(res));

    plPlasmaAudioEventData eventData;
    eventData.m_bLooping = true;
    plPlasmaAudioEventManager manager;

    PL_TEST_RESULT(manager.ActivateEvent(sId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

    plDynamicArray<float> output;
    engine.RenderBlocks(5 * (uiSampleFrames + uiExpectedGap) / plOfflineAudioEngine::BlockFrames, &manager, &output);

    // onsets are exact (silence -> DC); the gap is the onset distance minus the sample length
    const plDynamicArray<plUInt32> onsets = plAudioTestUtils::FindOnsets(output, 1e-3f, 256);

    if (!PL_TEST_BOOL_MSG(onsets.GetCount() >= 4, "only %u playbacks", onsets.GetCount()))
      return;

    for (plUInt32 i = 1; i < onsets.GetCount(); ++i)
    {
      const plInt64 iGap = static_cast<plInt64>(onsets[i]) - static_cast<plInt64>(onsets[i - 1]) - uiSampleFrames;

      // the audible length of the previous playback, between its half-level crossings; the filters shape the
      // rising and the falling edge of the DC block the same way, so this is the played length in frames
      plUInt32 uiFirstHalf = onsets[i - 1];
      while (uiFirstHalf < onsets[i] && plMath::Abs(output[uiFirstHalf]) < 0.25f)
        ++uiFirstHalf;

      plUInt32 uiLastHalf = onsets[i] - 1;
      while (uiLastHalf > uiFirstHalf && plMath::Abs(output[uiLastHalf]) < 0.25f)
        --uiLastHalf;

      const plUInt32 uiPlayedFrames = uiLastHalf - uiFirstHalf + 1;
      const plInt64 iSilence = static_cast<plInt64>(onsets[i]) - static_cast<plInt64>(onsets[i - 1]) - uiPlayedFrames;

      plStringBuilder sMsg;
      sMsg.SetFormat("ScattererGap ({} frame sample): playback {} at frame {}, onset gap {} frames, previous playback audible for {} frames, silence {} frames", uiSampleFrames, i, onsets[i], iGap, uiPlayedFrames, iSilence);
      Report(sMsg);

      PL_TEST_INT(iGap, uiExpectedGap);
      PL_TEST_INT(uiPlayedFrames, uiSampleFrames);
      PL_TEST_INT(iSilence, uiExpectedGap);
    }
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Scheduler, ScattererGap)
{
  // a sample length that ends on a render block boundary, and one that ends inside a block
  PL_TEST_BLOCK(plTestBlock::Enabled, "4800 frame sample")
  {
    RunScattererGap(4800);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "4100 frame sample")
  {
    RunScattererGap(4100);
  }
}

namespace
{
  struct SchedulerBudget
  {
    SchedulerBudget() { plAudioVoiceManager::SetBudgets(64, 64); }
    ~SchedulerBudget() { plAudioVoiceManager::SetBudgets(-1, -1); }
  };

  // Every update remains inside the configured 960-frame look-ahead. Varying the block size prevents the
  // fixture's starts and ends from accidentally passing because they coincide with an ATL update.
  void RenderIrregular(plOfflineAudioEngine& engine, plPlasmaAudioEventManager& manager, plUInt32 uiFrames, plDynamicArray<float>& output)
  {
    constexpr plUInt32 blocks[] = {17, 743, 251, 959, 61, 480};
    for (plUInt32 i = 0, uiRendered = 0; uiRendered < uiFrames; ++i)
    {
      const plUInt32 uiBlock = plMath::Min(blocks[i % PL_ARRAY_SIZE(blocks)], uiFrames - uiRendered);
      engine.Render(uiBlock, &output);
      manager.Update(plTime::MakeFromSeconds(static_cast<double>(uiBlock) / plOfflineAudioEngine::SampleRate), plVec3::MakeZero());
      uiRendered += uiBlock;
    }
  }

  void AddSchedulerLoad(plOfflineAudioEngine& engine, plPlasmaAudioEventManager& manager, plPlasmaAudioEventData (&events)[32], plStringView sId)
  {
    // Silent PCM still runs 32 real decoder/filter/voice chains without masking the measured event's onsets.
    // The test's explicit 64-voice budget keeps all of them real throughout the render.
    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(997, 0.0f));
    res.m_Desc.m_uiMaxInstances = 32;
    res.m_Desc.m_Instruments[0].m_bLoop = true;
    const auto hEvent = plAudioTestUtils::CreateEventResource(sId, std::move(res));
    for (auto& event : events)
    {
      event.m_bLooping = true;
      PL_TEST_RESULT(manager.ActivateEvent(sId, nullptr, &event, engine.GetBusGraph(), engine.GetEngine()));
    }
    PL_TEST_INT(manager.GetRealVoiceCount(), 32);
  }

  plDynamicArray<plUInt32> RenderLoadedRetriggers(plStringView sId, plUInt32 uiDelay, bool bLoad)
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return {};

    engine.Render(1000, nullptr);
    plPlasmaAudioEventData loadEvents[32];
    plPlasmaAudioEventData event;
    plPlasmaAudioEventManager manager;
    if (bLoad)
    {
      plStringBuilder sLoadId(sId, ":Load");
      AddSchedulerLoad(engine, manager, loadEvents, sLoadId);
    }

    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(4100, 0.5f));
    auto& instrument = res.m_Desc.m_Instruments[0];
    instrument.m_StartDelay = plTime::MakeFromSeconds(static_cast<double>(uiDelay) / plOfflineAudioEngine::SampleRate);
    instrument.m_bLoop = true;
    instrument.m_MinRetriggerDelay = plTime::MakeFromMilliseconds(50);
    instrument.m_MaxRetriggerDelay = instrument.m_MinRetriggerDelay;
    const auto hEvent = plAudioTestUtils::CreateEventResource(sId, std::move(res));
    event.m_bLooping = true;
    PL_TEST_RESULT(manager.ActivateEvent(sId, nullptr, &event, engine.GetBusGraph(), engine.GetEngine()));

    plDynamicArray<float> output;
    RenderIrregular(engine, manager, uiDelay + 5 * (4100 + 2400), output);
    if (bLoad)
      PL_TEST_BOOL(manager.GetRealVoiceCount() >= 32);
    return plAudioTestUtils::FindOnsets(output, 1e-3f, 256);
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Scheduler, ExactFramesUnderLoad)
{
  SchedulerBudget budget;
  const auto reference = RenderLoadedRetriggers("PlasmaAudioTest:TimingReference", 0, false);
  const auto delayed = RenderLoadedRetriggers("PlasmaAudioTest:TimingLoaded", 4807, true);
  if (!PL_TEST_INT(reference.GetCount(), 5) || !PL_TEST_INT(delayed.GetCount(), 5))
    return;

  for (plUInt32 i = 0; i < 5; ++i)
  {
    // Subtract only the fixed graph latency measured in the zero-delay control. No frame tolerance.
    PL_TEST_INT(delayed[i] - reference[0], 4807 + i * 6500);
    PL_TEST_INT(reference[i] - reference[0], i * 6500);
  }
  Report("ExactFramesUnderLoad: 32 real background voices, irregular 17..959 frame updates, 4807-frame start and 6500-frame retrigger period; required error 0 frames.");
}

PL_CREATE_SIMPLE_TEST(Scheduler, LoopSeamMinus80dBFS)
{
  SchedulerBudget budget;
  constexpr plUInt32 uiCycleFrames = 4100;
  constexpr plUInt32 uiLoops = 6;
  constexpr plUInt32 uiGuard = 256;
  constexpr float fLimit = 0.0001f; // 20 * log10(0.0001) = -80 dBFS; absolute, not relative to an inner step

  // A separate acceptance fixture; the existing cosine seam regression above stays unchanged. A raised-cosine
  // pulse with 256 zero frames at each end has exactly equal endpoints and zero slope. The guard lets the voice
  // filters decay before a scheduled voice is replaced. Compare to the same six cycles in ONE continuous voice.
  auto cycle = plAudioTestUtils::MakeDc(uiCycleFrames, 0.0f);
  for (plUInt32 i = uiGuard; i < uiCycleFrames - uiGuard; ++i)
  {
    const double phase = 2.0 * plMath::Pi<double>() * (i - uiGuard) / (uiCycleFrames - 2 * uiGuard - 1);
    cycle[i] = 0.25f * (1.0f - static_cast<float>(plMath::Cos(plAngle::MakeFromRadian(phase))));
  }
  PL_TEST_FLOAT(cycle[0], cycle.PeekBack(), 0.0f);

  plDynamicArray<float> unrolled;
  for (plUInt32 i = 0; i < uiLoops; ++i)
    unrolled.PushBackRange(cycle);

  plDynamicArray<float> reference;
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;
    const auto hEvent = plAudioTestUtils::CreateEventResource("PlasmaAudioTest:SeamReference", plAudioTestUtils::MakeSingleInstrumentEvent(unrolled));
    plPlasmaAudioEventData event;
    plPlasmaAudioEventManager manager;
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:SeamReference", nullptr, &event, engine.GetBusGraph(), engine.GetEngine()));
    RenderIrregular(engine, manager, unrolled.GetCount(), reference);
  }

  const char* modes[] = {"Region", "Native", "Scheduled"};
  for (plUInt32 mode = 0; mode < PL_ARRAY_SIZE(modes); ++mode)
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;
    plPlasmaAudioEventData loadEvents[32];
    plPlasmaAudioEventData event;
    plPlasmaAudioEventManager manager;
    plStringBuilder id("PlasmaAudioTest:StrictSeam", modes[mode]);
    plStringBuilder loadId(id, ":Load");
    AddSchedulerLoad(engine, manager, loadEvents, loadId);

    auto res = plAudioTestUtils::MakeSingleInstrumentEvent(cycle);
    if (mode == 0)
    {
      res.m_Desc.m_bLoopRegion = true;
      res.m_Desc.m_LoopEnd = plTime::MakeFromSeconds(static_cast<double>(uiCycleFrames) / plOfflineAudioEngine::SampleRate);
    }
    else
    {
      res.m_Desc.m_Instruments[0].m_bLoop = true;
      if (mode == 2)
      {
        const auto entry = res.m_Desc.m_Instruments[0].m_Playlist[0];
        res.m_Desc.m_Instruments[0].m_Playlist.PushBack(entry);
      }
    }
    const auto hEvent = plAudioTestUtils::CreateEventResource(id, std::move(res));
    event.m_bLooping = true;
    PL_TEST_RESULT(manager.ActivateEvent(id, nullptr, &event, engine.GetBusGraph(), engine.GetEngine()));
    plDynamicArray<float> output;
    RenderIrregular(engine, manager, unrolled.GetCount(), output);
    PL_TEST_BOOL(manager.GetRealVoiceCount() >= 33);
    if (!PL_TEST_INT(output.GetCount(), reference.GetCount()))
      continue;

    float maxError = 0.0f;
    float maxSeam = 0.0f;
    float peak = 0.0f;
    for (plUInt32 i = 0; i < output.GetCount(); ++i)
    {
      PL_TEST_BOOL(plMath::IsFinite(output[i]));
      maxError = plMath::Max(maxError, plMath::Abs(output[i] - reference[i]));
      peak = plMath::Max(peak, plMath::Abs(output[i]));
    }
    for (plUInt32 i = 1; i < uiLoops; ++i)
      maxSeam = plMath::Max(maxSeam, plAudioTestUtils::MaxStep(output, i * uiCycleFrames - 64, i * uiCycleFrames + 64));

    PL_TEST_BOOL_MSG(peak > 0.4f, "%s must render the pulse, not silence", modes[mode]);
    PL_TEST_BOOL_MSG(maxError <= fLimit, "%s: max absolute residual %g exceeds -80 dBFS (%g)", modes[mode], maxError, fLimit);
    PL_TEST_BOOL_MSG(maxSeam <= fLimit, "%s: max seam step %g exceeds -80 dBFS (%g)", modes[mode], maxSeam, fLimit);
    plStringBuilder message;
    message.SetFormat("LoopSeamMinus80dBFS/{}: max residual {}, max seam step {}, limit {}", modes[mode], maxError, maxSeam, fLimit);
    Report(message);
  }
}

namespace
{
  /// \brief An engine set up like plMiniAudioSingleton's fallback without a playback device: no device, no mixer,
  /// only the clock node on the endpoint, advanced through plMiniAudioDsp::RenderWithoutDevice.
  class HeadlessEngine
  {
  public:
    static constexpr ma_uint32 SampleRate = 48000;

    HeadlessEngine()
    {
      ma_engine_config cfg = ma_engine_config_init();
      cfg.noDevice = MA_TRUE;
      cfg.channels = 2;
      cfg.sampleRate = SampleRate;

      m_bEngineInitialized = ma_engine_init(&cfg, &m_Engine) == MA_SUCCESS;
      m_bValid = m_bEngineInitialized && plMiniAudioDsp::InitClockNode(&m_Engine, &m_ClockNode).Succeeded();
    }

    ~HeadlessEngine()
    {
      plMiniAudioDsp::UninitClockNode(&m_ClockNode);

      if (m_bEngineInitialized)
      {
        ma_engine_uninit(&m_Engine);
      }
    }

    PL_NODISCARD bool IsValid() const { return m_bValid; }
    PL_NODISCARD ma_engine* GetEngine() { return &m_Engine; }
    PL_NODISCARD ma_uint64 GetTime() { return ma_engine_get_time_in_pcm_frames(&m_Engine); }

    ma_uint64 Advance(plTime delta, plPlasmaAudioEventManager* pManager)
    {
      const ma_uint64 uiRendered = plMiniAudioDsp::RenderWithoutDevice(&m_Engine, delta, m_fRemainder, m_Scratch);

      if (pManager != nullptr)
      {
        pManager->Update(delta, plVec3::MakeZero());
      }

      return uiRendered;
    }

  private:
    ma_engine m_Engine = {};
    plMaClockNode m_ClockNode;
    bool m_bEngineInitialized = false;
    bool m_bValid = false;
    double m_fRemainder = 0.0;
    plDynamicArray<float> m_Scratch;
  };

  /// \brief Advances the engine in 60 Hz updates until the instance of eventData ends; returns the update count, or
  /// plInvalidIndex when it still plays after uiMaxUpdates. Sets out_bVoiceSeen when a voice was active in between.
  plUInt32 AdvanceUntilFinished(HeadlessEngine& ref_engine, plPlasmaAudioEventManager& ref_manager, const plPlasmaAudioEventData& eventData, plUInt32 uiMaxUpdates, bool& out_bVoiceSeen)
  {
    out_bVoiceSeen = false;

    for (plUInt32 i = 1; i <= uiMaxUpdates; ++i)
    {
      ref_engine.Advance(plTime::MakeFromSeconds(1.0 / 60.0), &ref_manager);

      if (ref_manager.GetActiveVoiceCount() > 0)
        out_bVoiceSeen = true;

      if (!ref_manager.IsEventPlaying(&eventData))
        return i;
    }

    return plInvalidIndex;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Scheduler, HeadlessClock)
{
  // 60 Hz updates are exactly 800 frames at 48 kHz
  constexpr plUInt32 uiFramesPerUpdate = 800;

  PL_TEST_BLOCK(plTestBlock::Enabled, "Clock advances by the update delta")
  {
    HeadlessEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    for (plUInt32 i = 0; i < 10; ++i)
    {
      PL_TEST_INT(engine.Advance(plTime::MakeFromSeconds(1.0 / 60.0), nullptr), uiFramesPerUpdate);
    }
    PL_TEST_INT(engine.GetTime(), 10 * uiFramesPerUpdate);

    // fractional frames are carried: three updates of 533.33 frames render 1600 frames together
    ma_uint64 uiRendered = 0;
    for (plUInt32 i = 0; i < 3; ++i)
    {
      uiRendered += engine.Advance(plTime::MakeFromSeconds(1.0 / 90.0), nullptr);
    }
    PL_TEST_BOOL_MSG(uiRendered >= 1599 && uiRendered <= 1600, "rendered %llu frames for 3 x 1/90 s", static_cast<unsigned long long>(uiRendered));

    // a stall is not caught up beyond one second
    PL_TEST_INT(engine.Advance(plTime::MakeFromSeconds(5.0), nullptr), HeadlessEngine::SampleRate);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "One-shot finishes after its length")
  {
    HeadlessEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    // 100 ms
    plAudioTestUtils::CreateEventResource("PlasmaAudioTest:HeadlessOneShot", plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(4800, 0.5f)));

    plPlasmaAudioEventData eventData;
    plPlasmaAudioEventManager manager;
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:HeadlessOneShot", nullptr, &eventData, nullptr, engine.GetEngine()));

    bool bVoiceSeen = false;
    const plUInt32 uiUpdates = AdvanceUntilFinished(engine, manager, eventData, 60, bVoiceSeen);

    plStringBuilder sMsg;
    sMsg.SetFormat("HeadlessClock one-shot: finished after {} updates, engine time {}", uiUpdates, engine.GetTime());
    Report(sMsg);

    PL_TEST_BOOL(bVoiceSeen);
    // 100 ms are 6 updates; the voice's end is noticed in the update after it ran out
    PL_TEST_BOOL_MSG(uiUpdates >= 6 && uiUpdates <= 8, "%s", sMsg.GetData());
    PL_TEST_INT(manager.GetInstanceCount(), 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Delayed start fires with nothing else attached")
  {
    HeadlessEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    // 100 ms of sound after a 200 ms delay: while waiting, only the clock node is attached to the endpoint
    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(4800, 0.5f));
    res.m_Desc.m_Instruments[0].m_StartDelay = plTime::MakeFromMilliseconds(200);
    plAudioTestUtils::CreateEventResource("PlasmaAudioTest:HeadlessDelayed", std::move(res));

    plPlasmaAudioEventData eventData;
    plPlasmaAudioEventManager manager;
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:HeadlessDelayed", nullptr, &eventData, nullptr, engine.GetEngine()));
    PL_TEST_INT(manager.GetActiveVoiceCount(), 0);

    bool bVoiceSeen = false;
    const plUInt32 uiUpdates = AdvanceUntilFinished(engine, manager, eventData, 60, bVoiceSeen);

    plStringBuilder sMsg;
    sMsg.SetFormat("HeadlessClock delayed start: finished after {} updates, engine time {}", uiUpdates, engine.GetTime());
    Report(sMsg);

    PL_TEST_BOOL(bVoiceSeen);
    // 300 ms are 18 updates
    PL_TEST_BOOL_MSG(uiUpdates >= 18 && uiUpdates <= 20, "%s", sMsg.GetData());
    PL_TEST_INT(manager.GetInstanceCount(), 0);
  }
}
