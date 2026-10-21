#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

PL_CREATE_SIMPLE_TEST_GROUP(EventModel);

namespace
{
  constexpr float SilenceThreshold = 1e-6f;
  constexpr plUInt32 FramesPerSecond = plOfflineAudioEngine::SampleRate;

  void Report(plStringView sText)
  {
    plStringBuilder sTmp = sText;
    plTestFramework::Output(plTestOutput::Message, "%s", sTmp.GetData());
  }

  /// \brief Adds a parameter "Speed" (0..10, default 0) with the given seek speed.
  void AddSpeedParameter(plAudioEventDesc& ref_desc, float fSeekSpeed)
  {
    plAudioEventParameterDesc& parameter = ref_desc.m_Parameters.ExpandAndGetRef();
    parameter.m_sName = "Speed";
    parameter.m_fMin = 0.0f;
    parameter.m_fMax = 10.0f;
    parameter.m_fDefault = 0.0f;
    parameter.m_fSeekSpeed = fSeekSpeed;
  }

  /// \brief Adds an automation of parameter 0 with a straight curve from (0, fAtMin) to (10, fAtMax).
  plAudioEventAutomationDesc& AddAutomation(plAudioEventDesc& ref_desc, plAudioEventAutomationTarget::Enum target, double fAtMin, double fAtMax)
  {
    plAudioEventAutomationDesc& automation = ref_desc.m_Automations.ExpandAndGetRef();
    automation.m_uiParameter = 0;
    automation.m_eTarget = target;
    automation.m_Curve.AddControlPoint(0.0).m_Position.y = fAtMin;
    automation.m_Curve.AddControlPoint(10.0).m_Position.y = fAtMax;
    automation.m_Curve.SortControlPoints();
    automation.m_Curve.CreateLinearApproximation();
    return automation;
  }

  /// \brief Largest magnitude in [uiBegin, uiEnd).
  float PeakOf(const plDynamicArray<float>& samples, plUInt32 uiBegin, plUInt32 uiEnd)
  {
    float fPeak = 0.0f;
    for (plUInt32 i = uiBegin; i < uiEnd && i < samples.GetCount(); ++i)
      fPeak = plMath::Max(fPeak, plMath::Abs(samples[i]));
    return fPeak;
  }

  /// \brief Plays the event for uiBlocks blocks and returns the peak of the last 20 blocks.
  float MeasureSteadyPeak(plStringView sEventId, plAudioEventResourceDescriptor&& res, plUInt32 uiBlocks = 60)
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return 0.0f;

    const plAudioEventResourceHandle hEvent = plAudioTestUtils::CreateEventResource(sEventId, std::move(res));

    plPlasmaAudioEventData eventData;
    plPlasmaAudioEventManager manager;
    PL_TEST_RESULT(manager.ActivateEvent(sEventId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

    plDynamicArray<float> output;
    engine.RenderBlocks(uiBlocks, &manager, &output);
    manager.StopAll(true);

    return PeakOf(output, output.GetCount() - 20 * plOfflineAudioEngine::BlockFrames, output.GetCount());
  }
} // namespace

PL_CREATE_SIMPLE_TEST(EventModel, ParameterSeekSpeed)
{
  // the volume follows Speed / 10; Speed is set from 0 to 10 at once and seeks at 10 units per second
  auto measure = [](plStringView sEventId, float fSeekSpeed, plDynamicArray<float>& out_output, plUInt32& out_uiSetFrame)
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(3 * FramesPerSecond, 0.5f));
    AddSpeedParameter(res.m_Desc, fSeekSpeed);
    AddAutomation(res.m_Desc, plAudioEventAutomationTarget::EventVolume, 0.0, 1.0);
    const plAudioEventResourceHandle hEvent = plAudioTestUtils::CreateEventResource(sEventId, std::move(res));

    plPlasmaAudioEventData eventData;
    plPlasmaAudioEventManager manager;
    PL_TEST_RESULT(manager.ActivateEvent(sEventId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

    engine.RenderBlocks(10, &manager, &out_output);
    out_uiSetFrame = out_output.GetCount();
    manager.SetEventParameter(nullptr, "Speed", 10.0f);
    engine.RenderBlocks(150, &manager, &out_output);
    manager.StopAll(true);
  };

  plDynamicArray<float> jump;
  plDynamicArray<float> seek;
  plUInt32 uiJumpSet = 0;
  plUInt32 uiSeekSet = 0;
  measure("PlasmaAudioTest:SeekSpeed_Jump", 0.0f, jump, uiJumpSet);
  measure("PlasmaAudioTest:SeekSpeed_Seek", 10.0f, seek, uiSeekSet);

  if (!PL_TEST_BOOL(jump.GetCount() > uiJumpSet + FramesPerSecond && seek.GetCount() > uiSeekSet + FramesPerSecond + 1000))
    return;

  // full level: the event without a seek after 0.1 s
  const float fFull = PeakOf(jump, uiJumpSet + FramesPerSecond / 10, uiJumpSet + FramesPerSecond / 10 + 480);
  const float fHalfway = PeakOf(seek, uiSeekSet + FramesPerSecond / 2, uiSeekSet + FramesPerSecond / 2 + 480);
  const float fArrived = PeakOf(seek, uiSeekSet + FramesPerSecond + 1000, uiSeekSet + FramesPerSecond + 1480);

  plStringBuilder sMsg;
  sMsg.SetFormat("ParameterSeekSpeed: full {}, seeking after 0.5 s {}, after 1.02 s {}", fFull, fHalfway, fArrived);
  Report(sMsg);

  PL_TEST_BOOL(fFull > 0.1f);
  PL_TEST_FLOAT(fHalfway / fFull, 0.5f, 0.05f);
  PL_TEST_FLOAT(fArrived / fFull, 1.0f, 0.01f);
}

PL_CREATE_SIMPLE_TEST(EventModel, InstrumentVolumeAutomation)
{
  const float fReference = MeasureSteadyPeak("PlasmaAudioTest:InstrumentVolume_Off", plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(2 * FramesPerSecond, 0.5f)));

  plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(2 * FramesPerSecond, 0.5f));
  AddSpeedParameter(res.m_Desc, 0.0f);
  AddAutomation(res.m_Desc, plAudioEventAutomationTarget::InstrumentVolume, 0.25, 0.25).m_uiInstrument = 0;

  // an automation of an instrument the event does not have changes nothing
  AddAutomation(res.m_Desc, plAudioEventAutomationTarget::InstrumentVolume, 0.0, 0.0).m_uiInstrument = 5;

  const float fAutomated = MeasureSteadyPeak("PlasmaAudioTest:InstrumentVolume_On", std::move(res));

  plStringBuilder sMsg;
  sMsg.SetFormat("InstrumentVolumeAutomation: without {}, with 0.25 {}", fReference, fAutomated);
  Report(sMsg);

  PL_TEST_BOOL(fReference > 0.1f);
  PL_TEST_FLOAT(fAutomated / fReference, 0.25f, 0.01f);
}

PL_CREATE_SIMPLE_TEST(EventModel, FilterCutoffAutomation)
{
  // 12 kHz: 4 frames per period
  const plDynamicArray<float> tone = plAudioTestUtils::MakeCosineCycle(2 * FramesPerSecond, 2 * FramesPerSecond / 4, 0.5f);

  const float fOpen = MeasureSteadyPeak("PlasmaAudioTest:FilterCutoff_Open", plAudioTestUtils::MakeSingleInstrumentEvent(tone));

  plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(tone);
  AddSpeedParameter(res.m_Desc, 0.0f);
  AddAutomation(res.m_Desc, plAudioEventAutomationTarget::FilterCutoff, 300.0, 300.0);
  const float fFiltered = MeasureSteadyPeak("PlasmaAudioTest:FilterCutoff_300", std::move(res));

  plStringBuilder sMsg;
  sMsg.SetFormat("FilterCutoffAutomation: 12 kHz open {}, through a 300 Hz cutoff {}", fOpen, fFiltered);
  Report(sMsg);

  PL_TEST_BOOL(fOpen > 0.1f);
  PL_TEST_BOOL(fFiltered < 0.05f * fOpen);
}

PL_CREATE_SIMPLE_TEST(EventModel, SendLevelAutomation)
{
  // DC adds up: the dry signal and the send to the aux bus both reach the master bus, so the level above the dry
  // level is what the send carries
  auto measure = [](plStringView sEventId, double fSendLevel) -> float
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return 0.0f;

    plAudioMixerDesc mixer;
    mixer.m_Buses.ExpandAndGetRef().m_sName = "Master";
    plAudioBusDesc& aux = mixer.m_Buses.ExpandAndGetRef();
    aux.m_sName = "Reverb";
    aux.m_uiParent = 0;
    aux.m_bIsAux = true;

    if (!PL_TEST_RESULT(engine.GetBusGraph()->Build(mixer, engine.GetEngine())))
      return 0.0f;

    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(2 * FramesPerSecond, 0.25f));
    AddSpeedParameter(res.m_Desc, 0.0f);
    AddAutomation(res.m_Desc, plAudioEventAutomationTarget::SendLevel, fSendLevel, fSendLevel).m_sBus = "Reverb";
    const plAudioEventResourceHandle hEvent = plAudioTestUtils::CreateEventResource(sEventId, std::move(res));

    plPlasmaAudioEventData eventData;
    plPlasmaAudioEventManager manager;
    PL_TEST_RESULT(manager.ActivateEvent(sEventId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

    plDynamicArray<float> output;
    engine.RenderBlocks(60, &manager, &output);
    manager.StopAll(true);

    return PeakOf(output, output.GetCount() - 20 * plOfflineAudioEngine::BlockFrames, output.GetCount());
  };

  const float fNone = measure("PlasmaAudioTest:SendLevel_0", 0.0);
  const float fQuarter = measure("PlasmaAudioTest:SendLevel_25", 0.25);
  const float fHalf = measure("PlasmaAudioTest:SendLevel_50", 0.5);

  plStringBuilder sMsg;
  sMsg.SetFormat("SendLevelAutomation: send 0 {}, 0.25 {}, 0.5 {}", fNone, fQuarter, fHalf);
  Report(sMsg);

  PL_TEST_BOOL(fNone > 0.1f);
  PL_TEST_FLOAT((fQuarter - fNone) / fNone, 0.25f, 0.01f);
  PL_TEST_FLOAT((fHalf - fNone) / fNone, 0.5f, 0.01f);
}

PL_CREATE_SIMPLE_TEST(EventModel, SustainAndKeyOff)
{
  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  // A plays 0.1 s from the start; B would start at 0.5 s, behind the sustain point at 0.3 s
  plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(FramesPerSecond / 10, 0.5f));
  {
    plAudioEventInstrumentDesc b = res.m_Desc.m_Instruments[0];
    b.m_sName = "B";
    b.m_StartDelay = plTime::MakeFromMilliseconds(500);
    res.m_Desc.m_Instruments.PushBack(b);
  }
  res.m_Desc.m_SustainPoints.PushBack(plTime::MakeFromMilliseconds(300));

  constexpr plStringView sEventId = "PlasmaAudioTest:SustainKeyOff";
  const plAudioEventResourceHandle hEvent = plAudioTestUtils::CreateEventResource(sEventId, std::move(res));

  plPlasmaAudioEventData eventData;
  plPlasmaAudioEventManager manager;
  PL_TEST_RESULT(manager.ActivateEvent(sEventId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

  // held for a second: only A sounds, and the instance waits
  plDynamicArray<float> output;
  engine.RenderBlocks(100, &manager, &output);
  PL_TEST_BOOL(manager.IsEventPlaying(&eventData));

  plDynamicArray<plUInt32> onsets = plAudioTestUtils::FindOnsets(output, SilenceThreshold, 2400);
  PL_TEST_INT(onsets.GetCount(), 1);

  const plUInt32 uiKeyOffFrame = output.GetCount();
  manager.KeyOff(nullptr);
  engine.RenderBlocks(60, &manager, &output);

  // B starts 0.2 s (its distance behind the sustain point) after the key-off, plus the look-ahead
  onsets = plAudioTestUtils::FindOnsets(output, SilenceThreshold, 2400);
  if (PL_TEST_INT(onsets.GetCount(), 2))
  {
    const plUInt32 uiExpected = uiKeyOffFrame + static_cast<plUInt32>(plOfflineAudioEngine::LookAheadFrames) + FramesPerSecond / 5;

    plStringBuilder sMsg;
    sMsg.SetFormat("SustainAndKeyOff: key-off at frame {}, B starts at {} (expected {})", uiKeyOffFrame, onsets[1], uiExpected);
    Report(sMsg);

    PL_TEST_BOOL(onsets[1] + plOfflineAudioEngine::BlockFrames >= uiExpected && onsets[1] <= uiExpected + plOfflineAudioEngine::BlockFrames);
  }

  // B ended: nothing is left to wait for
  engine.RenderBlocks(40, &manager, &output);
  PL_TEST_BOOL(!manager.IsEventPlaying(&eventData));

  // a second key-off without a sustain point left changes nothing
  manager.KeyOff(nullptr);
}

PL_CREATE_SIMPLE_TEST(EventModel, Markers)
{
  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  // the sample ends during the hold: the markers behind the sustain point keep the instance alive
  plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(FramesPerSecond / 2, 0.5f));
  auto addMarker = [&](plStringView sName, double fSeconds)
  {
    plAudioEventMarkerDesc& marker = res.m_Desc.m_Markers.ExpandAndGetRef();
    marker.m_sName = sName;
    marker.m_Time = plTime::MakeFromSeconds(fSeconds);
  };
  addMarker("Start", 0.0);
  addMarker("FootL", 0.1);
  addMarker("FootR", 0.25);
  addMarker("Late", 0.6);
  res.m_Desc.m_SustainPoints.PushBack(plTime::MakeFromMilliseconds(400));

  constexpr plStringView sEventId = "PlasmaAudioTest:Markers";
  const plAudioEventResourceHandle hEvent = plAudioTestUtils::CreateEventResource(sEventId, std::move(res));

  plPlasmaAudioEventData eventData;
  plPlasmaAudioEventManager manager;
  PL_TEST_RESULT(manager.ActivateEvent(sEventId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

  plHybridArray<plString, 4> markers;

  PL_TEST_BLOCK(plTestBlock::Enabled, "In order, once")
  {
    // 0.3 s
    engine.RenderBlocks(30, &manager, nullptr);
    manager.TakeMarkers(&eventData, markers);

    if (PL_TEST_INT(markers.GetCount(), 3))
    {
      PL_TEST_STRING(markers[0], "Start");
      PL_TEST_STRING(markers[1], "FootL");
      PL_TEST_STRING(markers[2], "FootR");
    }

    markers.Clear();
    manager.TakeMarkers(&eventData, markers);
    PL_TEST_INT(markers.GetCount(), 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Held at the sustain point")
  {
    // to 1 s: the playhead waits at 0.4 s, so the marker at 0.6 s is not passed
    engine.RenderBlocks(70, &manager, nullptr);
    manager.TakeMarkers(&eventData, markers);
    PL_TEST_INT(markers.GetCount(), 0);
    PL_TEST_BOOL(manager.IsEventPlaying(&eventData));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "After the key-off")
  {
    manager.KeyOff(nullptr);
    engine.RenderBlocks(30, &manager, nullptr);
    manager.TakeMarkers(&eventData, markers);

    if (PL_TEST_INT(markers.GetCount(), 1))
    {
      PL_TEST_STRING(markers[0], "Late");
    }

    // nothing is left behind a sustain point: the instance ended
    PL_TEST_BOOL(!manager.IsEventPlaying(&eventData));
  }

  manager.StopAll(true);
}

PL_CREATE_SIMPLE_TEST(EventModel, NestedEvent)
{
  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  // a 0.2 s child, started by the parent's instrument at 0.25 s
  constexpr plStringView sChildId = "PlasmaAudioTest:Nested_Child";
  const plAudioEventResourceHandle hChild = plAudioTestUtils::CreateEventResource(sChildId, plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(FramesPerSecond / 5, 0.5f)));

  plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(16, 0.0f));
  res.m_Desc.m_Instruments[0].m_StartDelay = plTime::MakeFromMilliseconds(250);
  res.m_Desc.m_Instruments[0].m_Playlist[0].m_uiSample = plAudioEventSilenceSample;
  res.m_Desc.m_Instruments[0].m_Playlist[0].m_sEvent = sChildId;

  constexpr plStringView sParentId = "PlasmaAudioTest:Nested_Parent";
  const plAudioEventResourceHandle hParent = plAudioTestUtils::CreateEventResource(sParentId, std::move(res));

  plPlasmaAudioEventData eventData;
  plPlasmaAudioEventManager manager;

  // the latency of a voice start (the first frames through the voice's filter), from the child started on its own
  plUInt32 uiLatency = 0;
  {
    plPlasmaAudioEventData childData;
    PL_TEST_RESULT(manager.ActivateEvent(sChildId, nullptr, &childData, engine.GetBusGraph(), engine.GetEngine()));

    plDynamicArray<float> output;
    engine.RenderBlocks(30, &manager, &output);
    uiLatency = plAudioTestUtils::FindFirstAbove(output, SilenceThreshold);
    PL_TEST_INT(manager.GetInstanceCount(), 0);
  }

  const plUInt32 uiActivation = static_cast<plUInt32>(engine.GetTime());
  PL_TEST_RESULT(manager.ActivateEvent(sParentId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

  plDynamicArray<float> output;
  engine.RenderBlocks(35, &manager, &output);

  // parent and child play; the child started on the frame of the parent's entry
  PL_TEST_INT(manager.GetInstanceCount(), 2);
  PL_TEST_BOOL(manager.IsEventPlaying(&eventData));

  const plUInt32 uiOnset = uiActivation + plAudioTestUtils::FindFirstAbove(output, SilenceThreshold);
  const plUInt32 uiExpected = uiActivation + FramesPerSecond / 4 + uiLatency;

  plStringBuilder sMsg;
  sMsg.SetFormat("NestedEvent: child heard from frame {} (expected {}: 0.25 s after the start plus {} frames of voice latency)", uiOnset, uiExpected, uiLatency);
  Report(sMsg);

  PL_TEST_INT(uiOnset, uiExpected);

  // the child ends after 0.2 s, and the parent with it
  engine.RenderBlocks(30, &manager, &output);
  PL_TEST_INT(manager.GetInstanceCount(), 0);
  PL_TEST_BOOL(!manager.IsEventPlaying(&eventData));

  PL_TEST_BLOCK(plTestBlock::Enabled, "Stopping the parent stops the child")
  {
    PL_TEST_RESULT(manager.ActivateEvent(sParentId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));
    engine.RenderBlocks(35, &manager, nullptr);
    PL_TEST_INT(manager.GetInstanceCount(), 2);

    PL_TEST_RESULT(manager.StopEvent(&eventData, false));
    engine.RenderBlocks(10, &manager, nullptr);
    PL_TEST_INT(manager.GetInstanceCount(), 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "A looping nested event plays one pass, or the length of its track")
  {
    // the child loops a 0.1 s DC; the parent's track does not loop
    plAudioEventResourceDescriptor looping = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(FramesPerSecond / 10, 0.5f));
    looping.m_Desc.m_Instruments[0].m_bLoop = true;
    constexpr plStringView sLoopingId = "PlasmaAudioTest:Nested_Looping";
    const plAudioEventResourceHandle hLooping = plAudioTestUtils::CreateEventResource(sLoopingId, std::move(looping));

    auto measure = [&](plStringView sParentId, plTime nestedLength) -> plUInt32
    {
      plAudioEventResourceDescriptor parent = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(16, 0.0f));
      parent.m_Desc.m_Instruments[0].m_Playlist[0].m_uiSample = plAudioEventSilenceSample;
      parent.m_Desc.m_Instruments[0].m_Playlist[0].m_sEvent = sLoopingId;
      parent.m_Desc.m_Instruments[0].m_NestedLength = nestedLength;
      const plAudioEventResourceHandle hParentEvent = plAudioTestUtils::CreateEventResource(sParentId, std::move(parent));

      plPlasmaAudioEventData data;
      PL_TEST_RESULT(manager.ActivateEvent(sParentId, nullptr, &data, engine.GetBusGraph(), engine.GetEngine()));

      // the frame the last sound ends, counted from the start; everything has ended a second later
      plDynamicArray<float> played;
      engine.RenderBlocks(100, &manager, &played);
      PL_TEST_INT(manager.GetInstanceCount(), 0);
      PL_TEST_BOOL(!manager.IsEventPlaying(&data));

      plUInt32 uiLast = 0;
      for (plUInt32 i = 0; i < played.GetCount(); ++i)
      {
        if (plMath::Abs(played[i]) > 1e-3f)
          uiLast = i;
      }

      return uiLast;
    };

    const plUInt32 uiOnePass = measure("PlasmaAudioTest:Nested_OnePass", plTime::MakeZero());
    const plUInt32 uiLength = measure("PlasmaAudioTest:Nested_Length", plTime::MakeFromMilliseconds(350));

    // the length survives the event's file format
    {
      plAudioEventDesc desc;
      desc.m_Instruments.ExpandAndGetRef().m_NestedLength = plTime::MakeFromMilliseconds(1900);

      plDefaultMemoryStreamStorage storage;
      plMemoryStreamWriter writer(&storage);
      PL_TEST_RESULT(desc.Save(writer));
      plMemoryStreamReader reader(&storage);
      plAudioEventDesc loaded;
      PL_TEST_RESULT(loaded.Load(reader));
      PL_TEST_DOUBLE(loaded.m_Instruments[0].m_NestedLength.GetSeconds(), 1.9, 1e-9);
    }

    plStringBuilder sMsg;
    sMsg.SetFormat("NestedEvent: a looping child on a non-looping track sounds until frame {} (one pass, 0.1 s), with a 0.35 s length until frame {}", uiOnePass, uiLength);
    Report(sMsg);

    // the stop takes effect in the first update at or after the end: within a block
    PL_TEST_BOOL(uiOnePass + 1 >= FramesPerSecond / 10 && uiOnePass <= FramesPerSecond / 10 + 2 * plOfflineAudioEngine::BlockFrames);
    PL_TEST_BOOL(uiLength + 1 >= FramesPerSecond * 35 / 100 && uiLength <= FramesPerSecond * 35 / 100 + 2 * plOfflineAudioEngine::BlockFrames);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "A held event with a limited nested event ends with it after the key-off")
  {
    // as in the editor's smoke event: a 1 s sound, a looping 4 s nested event limited to 2 s, a sustain point at 0.6 s
    // and markers at 0.3 and 1.0 s
    plAudioEventResourceDescriptor hum = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(4 * FramesPerSecond, 0.25f));
    hum.m_Desc.m_Instruments[0].m_bLoop = true;
    hum.m_Desc.m_StopFadeOut = plTime::MakeFromMilliseconds(100);
    constexpr plStringView sHumId = "PlasmaAudioTest:Nested_Hum";
    const plAudioEventResourceHandle hHum = plAudioTestUtils::CreateEventResource(sHumId, std::move(hum));

    plAudioEventResourceDescriptor tone = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(FramesPerSecond, 0.25f));
    {
      plAudioEventInstrumentDesc nested = tone.m_Desc.m_Instruments[0];
      nested.m_Playlist[0].m_uiSample = plAudioEventSilenceSample;
      nested.m_Playlist[0].m_sEvent = sHumId;
      nested.m_NestedLength = plTime::MakeFromSeconds(2.0);
      tone.m_Desc.m_Instruments.PushBack(nested);
    }
    tone.m_Desc.m_SustainPoints.PushBack(plTime::MakeFromMilliseconds(600));
    tone.m_Desc.m_Markers.ExpandAndGetRef() = {"Marker 1", plTime::MakeFromMilliseconds(300)};
    tone.m_Desc.m_Markers.ExpandAndGetRef() = {"Marker 2", plTime::MakeFromMilliseconds(1000)};
    constexpr plStringView sToneId = "PlasmaAudioTest:Nested_Tone";
    const plAudioEventResourceHandle hTone = plAudioTestUtils::CreateEventResource(sToneId, std::move(tone));

    plPlasmaAudioEventData data;
    const plUInt32 uiStart = static_cast<plUInt32>(engine.GetTime());
    PL_TEST_RESULT(manager.ActivateEvent(sToneId, nullptr, &data, engine.GetBusGraph(), engine.GetEngine()));

    // held until the key-off at 1.4 s
    plDynamicArray<float> output;
    engine.RenderBlocks(140, &manager, &output);
    PL_TEST_INT(manager.GetInstanceCount(), 2);
    manager.KeyOff(nullptr);

    // the nested event ends 2 s after its start (plus its 0.1 s fade); the event ends with it, its markers passed
    plUInt32 uiEndedBlock = plInvalidIndex;
    for (plUInt32 uiBlock = 0; uiBlock < 300 && uiEndedBlock == plInvalidIndex; ++uiBlock)
    {
      engine.RenderBlocks(1, &manager, &output);
      if (!manager.IsEventPlaying(&data))
        uiEndedBlock = uiBlock;
    }

    plUInt32 uiLastSound = 0;
    for (plUInt32 i = 0; i < output.GetCount(); ++i)
    {
      if (plMath::Abs(output[i]) > 1e-3f)
        uiLastSound = i;
    }

    const double fEnded = (140.0 + uiEndedBlock + 1) * plOfflineAudioEngine::BlockFrames / FramesPerSecond;
    const double fLastSound = static_cast<double>(uiLastSound) / FramesPerSecond;

    plStringBuilder sMsg;
    sMsg.SetFormat("NestedEvent: held event with a 2 s nested event, key-off at 1.4 s: last sound at {} s, event ended at {} s", plArgF(fLastSound, 3), plArgF(fEnded, 3));
    Report(sMsg);

    PL_IGNORE_UNUSED(uiStart);
    PL_TEST_BOOL(fLastSound > 1.95 && fLastSound < 2.15);
    PL_TEST_BOOL(fEnded < 2.25);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "An event that nests itself stops at the nesting limit")
  {
    plAudioEventResourceDescriptor loop = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(16, 0.0f));
    loop.m_Desc.m_Instruments[0].m_Playlist[0].m_uiSample = plAudioEventSilenceSample;
    loop.m_Desc.m_Instruments[0].m_Playlist[0].m_sEvent = "PlasmaAudioTest:Nested_Self";
    const plAudioEventResourceHandle hSelf = plAudioTestUtils::CreateEventResource("PlasmaAudioTest:Nested_Self", std::move(loop));

    plPlasmaAudioEventData selfData;
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:Nested_Self", nullptr, &selfData, engine.GetBusGraph(), engine.GetEngine()));
    engine.RenderBlocks(20, &manager, nullptr);

    // the top instance and MaxNestingDepth children at most, then everything ends
    PL_TEST_BOOL(manager.GetInstanceCount() <= 1u + plPlasmaAudioEventInstance::MaxNestingDepth);
    engine.RenderBlocks(20, &manager, nullptr);
    PL_TEST_INT(manager.GetInstanceCount(), 0);
  }
}
