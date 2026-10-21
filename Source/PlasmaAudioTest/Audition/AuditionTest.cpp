#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

PL_CREATE_SIMPLE_TEST_GROUP(Audition);

namespace
{
  constexpr float SilenceThreshold = 1e-4f;

  void Report(plStringView sText)
  {
    plStringBuilder sTmp = sText;
    plTestFramework::Output(plTestOutput::Message, "%s", sTmp.GetData());
  }

  /// \brief Largest magnitude of the last uiFrames samples.
  float TailLevel(const plDynamicArray<float>& samples, plUInt32 uiFrames)
  {
    float fLevel = 0.0f;
    for (plUInt32 i = samples.GetCount() - plMath::Min(uiFrames, samples.GetCount()); i < samples.GetCount(); ++i)
      fLevel = plMath::Max(fLevel, plMath::Abs(samples[i]));
    return fLevel;
  }

  plUInt32 CountAbove(const plDynamicArray<float>& samples, float fThreshold)
  {
    plUInt32 uiCount = 0;
    for (float f : samples)
      uiCount += plMath::Abs(f) > fThreshold ? 1 : 0;
    return uiCount;
  }

  /// \brief One instrument with one DC entry per level, picked at random.
  plAudioEventResourceDescriptor MakeRandomLevelsEvent(plArrayPtr<const float> levels)
  {
    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(48000, levels[0]));
    plAudioEventInstrumentDesc& instrument = res.m_Desc.m_Instruments[0];
    instrument.m_ePlaylistMode = plAudioEventPlaylistMode::Random;

    for (plUInt32 i = 1; i < levels.GetCount(); ++i)
    {
      plAudioEventPlaylistEntryDesc& entry = instrument.m_Playlist.ExpandAndGetRef();
      entry.m_uiSample = static_cast<plUInt16>(i);
      entry.m_fWeight = 1.0f;

      plStringBuilder sName;
      sName.SetFormat("Sample{}.wav", i);
      res.m_Desc.m_SampleNames.PushBack(sName);
      res.m_SampleData.PushBack(plAudioTestUtils::MakeWavF32(plAudioTestUtils::MakeDc(48000, levels[i]), plOfflineAudioEngine::SampleRate));
    }

    return res;
  }

  /// \brief Plays the event seeded with uiSeed and returns the level it settles at.
  float PlaySeeded(plStringView sEventId, plUInt32 uiSeed)
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return 0.0f;

    plPlasmaAudioEventManager manager;
    plPlasmaAudioEventData eventData;
    eventData.m_bSeeded = true;
    eventData.m_uiSeed = uiSeed;

    PL_TEST_RESULT(manager.ActivateEvent(sEventId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

    plDynamicArray<float> output;
    engine.RenderBlocks(10, &manager, &output);
    manager.StopAll(true);
    return TailLevel(output, plOfflineAudioEngine::BlockFrames);
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Audition, RangeGain)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Gain curve")
  {
    plAudioEventInstrumentDesc instrument;
    PL_TEST_FLOAT(instrument.GetRangeGain(-100.0f), 1.0f, 0.0f);

    instrument.m_uiRangeParameter = 0;
    instrument.m_fRangeMin = 0.6f;
    instrument.m_fRangeMax = 1.0f;
    instrument.m_fRangeFade = 0.2f;
    PL_TEST_FLOAT(instrument.GetRangeGain(0.8f), 1.0f, 0.0f);
    PL_TEST_FLOAT(instrument.GetRangeGain(0.5f), 0.5f, 0.0001f);
    PL_TEST_FLOAT(instrument.GetRangeGain(0.4f), 0.0f, 0.0001f);
    PL_TEST_FLOAT(instrument.GetRangeGain(1.1f), 0.5f, 0.0001f);

    instrument.m_fRangeFade = 0.0f;
    PL_TEST_FLOAT(instrument.GetRangeGain(0.59f), 0.0f, 0.0f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Playback follows the parameter")
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(4 * 48000, 0.5f));
    plAudioEventParameterDesc& parameter = res.m_Desc.m_Parameters.ExpandAndGetRef();
    parameter.m_sName = "Surface";
    parameter.m_fMin = 0.0f;
    parameter.m_fMax = 1.0f;
    parameter.m_fDefault = 0.2f;

    plAudioEventInstrumentDesc& instrument = res.m_Desc.m_Instruments[0];
    instrument.m_uiRangeParameter = 0;
    instrument.m_fRangeMin = 0.6f;
    instrument.m_fRangeMax = 1.0f;
    instrument.m_fRangeFade = 0.2f;

    plAudioTestUtils::CreateEventResource("PlasmaAudioTest:AuditionRange", std::move(res));

    plPlasmaAudioEntityData entity(7, true);
    plPlasmaAudioEventData eventData;
    plPlasmaAudioEventManager manager;

    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:AuditionRange", &entity, &eventData, engine.GetBusGraph(), engine.GetEngine()));

    // outside the range and its fade: silent
    plDynamicArray<float> output;
    engine.RenderBlocks(20, &manager, &output);
    const float fOutside = TailLevel(output, 10 * plOfflineAudioEngine::BlockFrames);

    bool bWaiting = true;
    double fPosition = 0.0;
    plDynamicArray<bool> sounding;
    plDynamicArray<float> rangeGains;
    PL_TEST_BOOL(manager.GetEventState(&eventData, bWaiting, fPosition, sounding, rangeGains));
    PL_TEST_BOOL(!bWaiting);
    if (PL_TEST_INT(rangeGains.GetCount(), 1))
      PL_TEST_FLOAT(rangeGains[0], 0.0f, 0.0001f);

    // inside the range: full level
    manager.SetEventParameter(&entity, "Surface", 0.8f);
    output.Clear();
    engine.RenderBlocks(20, &manager, &output);
    const float fInside = TailLevel(output, 10 * plOfflineAudioEngine::BlockFrames);

    // halfway down the fade: half the level
    manager.SetEventParameter(&entity, "Surface", 0.5f);
    output.Clear();
    engine.RenderBlocks(20, &manager, &output);
    const float fFade = TailLevel(output, 10 * plOfflineAudioEngine::BlockFrames);

    PL_TEST_BOOL(manager.GetEventState(&eventData, bWaiting, fPosition, sounding, rangeGains));
    if (PL_TEST_INT(rangeGains.GetCount(), 1))
      PL_TEST_FLOAT(rangeGains[0], 0.5f, 0.0001f);

    manager.StopAll(true);

    plStringBuilder sMsg;
    sMsg.SetFormat("Audition RangeGain: outside {}, inside {}, half fade {} (ratio {})", fOutside, fInside, fFade, fInside > 0.0f ? fFade / fInside : 0.0f);
    Report(sMsg);

    PL_TEST_BOOL(fOutside < SilenceThreshold);
    PL_TEST_BOOL(fInside > 0.1f);
    PL_TEST_FLOAT(fFade / plMath::Max(fInside, 0.0001f), 0.5f, 0.02f);
  }
}

PL_CREATE_SIMPLE_TEST(Audition, SeededPicks)
{
  const float levels[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:AuditionSeeded", MakeRandomLevelsEvent(levels));

  plHybridArray<float, 16> picks;
  for (plUInt32 uiSeed = 1; uiSeed <= 12; ++uiSeed)
  {
    const float fFirst = PlaySeeded("PlasmaAudioTest:AuditionSeeded", uiSeed);
    const float fSecond = PlaySeeded("PlasmaAudioTest:AuditionSeeded", uiSeed);

    // the same seed picks the same entry
    PL_TEST_FLOAT(fFirst, fSecond, 0.0001f);
    picks.PushBack(fFirst);
  }

  // and different seeds do not all pick the same one
  bool bDifferent = false;
  for (float f : picks)
    bDifferent |= plMath::Abs(f - picks[0]) > 0.01f;

  PL_TEST_BOOL(bDifferent);
}

PL_CREATE_SIMPLE_TEST(Audition, StartOffsetAndPlayhead)
{
  // one second of signal
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:AuditionOffset", plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(48000, 0.5f)));

  // the position is read after 40 blocks (0.4 s), while both auditions still play
  auto play = [](double fOffset, plUInt32& out_uiAudible, double& out_fPosition)
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    // the timeline origin of an offset start lies before the start, so the engine clock must have run that long
    engine.Render(plOfflineAudioEngine::SampleRate, nullptr);

    plPlasmaAudioEventManager manager;
    plPlasmaAudioEventData eventData;
    eventData.m_StartOffset = plTime::MakeFromSeconds(fOffset);

    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:AuditionOffset", nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

    plDynamicArray<float> output;
    engine.RenderBlocks(40, &manager, &output);

    bool bWaiting = false;
    plDynamicArray<bool> sounding;
    plDynamicArray<float> rangeGains;
    PL_TEST_BOOL(manager.GetEventState(&eventData, bWaiting, out_fPosition, sounding, rangeGains));

    engine.RenderBlocks(100, &manager, &output);
    out_uiAudible = CountAbove(output, SilenceThreshold);
  };

  plUInt32 uiFull = 0;
  plUInt32 uiOffset = 0;
  double fFullPosition = 0.0;
  double fOffsetPosition = 0.0;
  play(0.0, uiFull, fFullPosition);
  play(0.5, uiOffset, fOffsetPosition);

  plStringBuilder sMsg;
  sMsg.SetFormat("Audition StartOffset: audible frames {} from 0 s, {} from 0.5 s; position after 0.4 s: {} and {}", uiFull, uiOffset, fFullPosition, fOffsetPosition);
  Report(sMsg);

  // the offset skips half of the signal (a 10 ms fade-in at the resume point and a look-ahead of slack)
  PL_TEST_BOOL(plMath::Abs(static_cast<plInt32>(uiFull) - 48000) < 200);
  PL_TEST_BOOL(plMath::Abs(static_cast<plInt32>(uiOffset) - 24000) < 1500);

  // the playhead counts timeline time, from the offset on
  PL_TEST_DOUBLE(fFullPosition, 0.4, 0.03);
  PL_TEST_DOUBLE(fOffsetPosition, 0.9, 0.03);
}
