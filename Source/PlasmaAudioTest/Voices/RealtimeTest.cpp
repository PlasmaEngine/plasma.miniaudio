#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <AudioSystem/Core/AudioSystemData.h>
#include <Foundation/Threading/Thread.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <PlasmaAudioPlugin/Events/AudioVoiceManager.h>
#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioPlugin/MiniAudioMiddleware.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>
#include <TestFramework/Utilities/TestLogInterface.h>

// Runtime behaviour that depends on threads, listeners and voice timelines: the cases the PlasmaAudio evaluation
// (2026-10-20) found unprotected.
PL_CREATE_SIMPLE_TEST_GROUP(Realtime);

namespace
{
  constexpr float SilenceThreshold = 1e-6f;

  struct BudgetScope
  {
    BudgetScope(plInt32 iReal, plInt32 iVirtual) { plAudioVoiceManager::SetBudgets(iReal, iVirtual); }
    ~BudgetScope() { plAudioVoiceManager::SetBudgets(-1, -1); }
  };

  struct Emitter
  {
    plPlasmaAudioEntityData m_Entity;
    plPlasmaAudioEventData m_Event;
  };

  plAudioEventResourceDescriptor MakeEvent(const plDynamicArray<float>& samples, bool bLoop, bool bSpatialize)
  {
    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(samples);
    res.m_Desc.m_uiMaxInstances = 0;
    res.m_Desc.m_eStealing = plAudioEventStealingMode::None;
    res.m_Desc.m_uiPriority = 128;
    res.m_Desc.m_bSpatialize = bSpatialize;
    res.m_Desc.m_fMinDistance = 1.0f;
    res.m_Desc.m_fMaxDistance = 100.0f;
    res.m_Desc.m_fRolloff = 1.0f;
    res.m_Desc.m_Instruments[0].m_bLoop = bLoop;
    return res;
  }

  plUInt32 GetRealCount(const plPlasmaAudioEventManager& manager, const Emitter& emitter)
  {
    plUInt32 uiReal = 0;
    plUInt32 uiVirtual = 0;
    manager.GetEventVoiceCounts(&emitter.m_Event, uiReal, uiVirtual);
    return uiReal;
  }

  plUInt32 FindLastAbove(const plDynamicArray<float>& samples, float fThreshold)
  {
    for (plUInt32 i = samples.GetCount(); i > 0; --i)
    {
      if (plMath::Abs(samples[i - 1]) > fThreshold)
        return i - 1;
    }
    return plInvalidIndex;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Realtime, VirtualResumeFollowsPitch)
{
  // A voice that was virtual while its pitch was raised resumes as far into its data as it would have played: the
  // timeline integrates the pitch over time. Measured through the voice's end, which comes earlier by the frames the
  // raised pitch skipped.
  constexpr plUInt32 uiLengthFrames = 2 * plOfflineAudioEngine::SampleRate;
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:ResumePitch", MakeEvent(plAudioTestUtils::MakeDc(uiLengthFrames, 0.5f), false, false));

  constexpr plUInt32 uiRealBlocks = 20;
  constexpr plUInt32 uiVirtualBlocks = 40;
  constexpr plUInt32 uiBlockFrames = plOfflineAudioEngine::BlockFrames;

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plPlasmaAudioEventManager manager;
  Emitter emitter;
  BudgetScope budgets(64, 512);

  plDynamicArray<float> output;
  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:ResumePitch", &emitter.m_Entity, &emitter.m_Event, engine.GetBusGraph(), engine.GetEngine()));
  engine.RenderBlocks(uiRealBlocks, &manager, &output);

  const plUInt32 uiStart = plAudioTestUtils::FindFirstAbove(output, SilenceThreshold);
  if (!PL_TEST_BOOL(uiStart != plInvalidIndex))
    return;

  // virtual at double pitch
  emitter.m_Entity.m_fPitchMultiplier = 2.0f;
  plAudioVoiceManager::SetBudgets(0, 512);
  engine.RenderBlocks(uiVirtualBlocks, &manager, &output);
  PL_TEST_INT(GetRealCount(manager, emitter), 0);

  // real again at the normal pitch
  emitter.m_Entity.m_fPitchMultiplier = 1.0f;
  plAudioVoiceManager::SetBudgets(64, 512);
  engine.RenderBlocks(1, &manager, &output);
  PL_TEST_INT(GetRealCount(manager, emitter), 1);

  engine.RenderBlocks(uiLengthFrames / uiBlockFrames + 20, &manager, &output);

  const plUInt32 uiEnd = FindLastAbove(output, SilenceThreshold);
  if (!PL_TEST_BOOL(uiEnd != plInvalidIndex))
    return;

  // real for uiRealBlocks minus the start, virtual twice as fast for uiVirtualBlocks, the rest real at pitch 1
  const double fRealFrames = static_cast<double>(uiRealBlocks * uiBlockFrames - uiStart);
  const double fVirtualFrames = static_cast<double>(uiVirtualBlocks * uiBlockFrames);
  const double fExpectedEnd = uiStart + fRealFrames + fVirtualFrames + (uiLengthFrames - fRealFrames - 2.0 * fVirtualFrames);

  // a timeline that ignored the pitch history would resume fVirtualFrames earlier in the data and end that much later
  const double fPitchUnawareEnd = fExpectedEnd + fVirtualFrames;

  plStringBuilder sMsg;
  sMsg.SetFormat("Realtime/VirtualResumeFollowsPitch: end at {}, expected {}, pitch-unaware {}", uiEnd, plArgF(fExpectedEnd, 0), plArgF(fPitchUnawareEnd, 0));
  plTestFramework::Output(plTestOutput::Message, "%s", sMsg.GetData());

  // the resume frame is quantized to blocks and the look-ahead
  PL_TEST_BOOL(plMath::Abs(static_cast<double>(uiEnd) - fExpectedEnd) < 4.0 * uiBlockFrames);
  PL_TEST_BOOL(static_cast<double>(uiEnd) < fPitchUnawareEnd - 8.0 * uiBlockFrames);
}

PL_CREATE_SIMPLE_TEST(Realtime, SplitScreenRanking)
{
  // Voices are spatialized against their closest listener, so the ranking uses the closest listener as well: an emitter
  // next to the second player outranks one far from the first.
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:SplitScreen", MakeEvent(plAudioTestUtils::MakeDc(plOfflineAudioEngine::SampleRate, 0.5f), true, true));

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plPlasmaAudioEventManager manager;
  BudgetScope budgets(1, 512);

  Emitter nearSecond; // 2 m from the second listener, beyond the max distance of the first
  nearSecond.m_Entity.m_LastTransform.m_vPosition = plVec3(502, 0, 0);

  Emitter farFirst; // 50 m from the first listener
  farFirst.m_Entity.m_LastTransform.m_vPosition = plVec3(50, 0, 0);

  const plVec3 bothListeners[] = {plVec3(0, 0, 0), plVec3(500, 0, 0)};
  const plVec3 firstListener[] = {plVec3(0, 0, 0)};

  auto Run = [&](plArrayPtr<const plVec3> listeners, plUInt32 uiBlocks)
  {
    for (plUInt32 i = 0; i < uiBlocks; ++i)
    {
      engine.Render(plOfflineAudioEngine::BlockFrames, nullptr);
      manager.Update(plOfflineAudioEngine::GetBlockDuration(), listeners);
    }
  };

  // the listeners are known before the emitters start, so their admission uses them as well
  Run(bothListeners, 1);
  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:SplitScreen", &farFirst.m_Entity, &farFirst.m_Event, engine.GetBusGraph(), engine.GetEngine()));
  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:SplitScreen", &nearSecond.m_Entity, &nearSecond.m_Event, engine.GetBusGraph(), engine.GetEngine()));

  // past the dwell time, so the ranking settles
  Run(bothListeners, 40);
  PL_TEST_INT(GetRealCount(manager, nearSecond), 1);
  PL_TEST_INT(GetRealCount(manager, farFirst), 0);

  // the second player leaves: the emitter beside it is out of range of the first
  Run(firstListener, 40);
  PL_TEST_INT(GetRealCount(manager, nearSecond), 0);
  PL_TEST_INT(GetRealCount(manager, farFirst), 1);

  manager.StopAll(true);
}

PL_CREATE_SIMPLE_TEST(Realtime, EntityDestroyedWhilePlaying)
{
  // The ATL does not always remove an entity before destroying its data (its shutdown did not). Destroying the data of
  // an entity with playing events stops them, so nothing reads the freed entity afterwards.
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:EntityTeardown", MakeEvent(plAudioTestUtils::MakeDc(plOfflineAudioEngine::SampleRate, 0.5f), true, true));

  plUniquePtr<plPlasmaAudioMiddleware> pMiddleware = PL_DEFAULT_NEW(plPlasmaAudioMiddleware);
  if (!PL_TEST_RESULT(pMiddleware->Startup()))
    return;

  PL_SCOPE_EXIT(pMiddleware->Shutdown().IgnoreResult());

  plAudioSystemEntityData* pEntity = pMiddleware->CreateEntityData(1);
  plAudioSystemEventData* pEvent = pMiddleware->CreateEventData(1);
  plAudioSystemStringTriggerData trigger(2, "PlasmaAudioTest:EntityTeardown");

  PL_TEST_RESULT(pMiddleware->ActivateTrigger(pEntity, &trigger, pEvent));

  for (plUInt32 i = 0; i < 5; ++i)
  {
    pMiddleware->Update(plTime::MakeFromMilliseconds(10));
  }

  PL_TEST_BOOL(pMiddleware->IsEventDataPlaying(pEvent));

  // the instances are stopped and let go of the entity right away; the next update removes them
  PL_TEST_RESULT(pMiddleware->DestroyEntityData(pEntity));

  for (plUInt32 i = 0; i < 5; ++i)
  {
    pMiddleware->Update(plTime::MakeFromMilliseconds(10));
  }

  PL_TEST_BOOL(!pMiddleware->IsEventDataPlaying(pEvent));
  PL_TEST_RESULT(pMiddleware->DestroyEventData(pEvent));
}

PL_CREATE_SIMPLE_TEST(Realtime, MixerValidation)
{
  // a valid routing: master <- music, master <- sfx, sfx -> reverb (aux)
  auto MakeValid = []()
  {
    plAudioMixerDesc desc;
    desc.MakeDefault();

    plAudioBusDesc& music = desc.m_Buses.ExpandAndGetRef();
    music.m_sName = "Music";
    music.m_uiParent = 0;

    plAudioBusDesc& reverb = desc.m_Buses.ExpandAndGetRef();
    reverb.m_sName = "Reverb";
    reverb.m_uiParent = 0;
    reverb.m_bIsAux = true;
    reverb.m_sEnvironmentName = "Cave";

    plAudioBusDesc& sfx = desc.m_Buses.ExpandAndGetRef();
    sfx.m_sName = "SFX";
    sfx.m_uiParent = 0;
    sfx.m_Sends.ExpandAndGetRef().m_uiTargetBus = 2;
    sfx.m_Effects.ExpandAndGetRef().InitDefaults();
    return desc;
  };

  plStringBuilder sErrors;

  PL_TEST_BLOCK(plTestBlock::Enabled, "Valid")
  {
    sErrors.Clear();
    PL_TEST_RESULT(MakeValid().Validate(sErrors));
    PL_TEST_BOOL(sErrors.IsEmpty());
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Parent cycle")
  {
    plAudioMixerDesc desc = MakeValid();
    desc.m_Buses[1].m_uiParent = 3; // Music -> SFX
    desc.m_Buses[3].m_uiParent = 1; // SFX -> Music

    sErrors.Clear();
    PL_TEST_BOOL(desc.Validate(sErrors).Failed());
    PL_TEST_BOOL(sErrors.FindSubString("cycle") != nullptr);
    PL_TEST_BOOL(desc.IsRoutedTo(1, 3) && desc.IsRoutedTo(3, 1));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Send cycle and send to a non-aux bus")
  {
    plAudioMixerDesc desc = MakeValid();
    desc.m_Buses[2].m_uiParent = 3; // Reverb -> SFX, while SFX sends to Reverb
    desc.m_Buses[1].m_Sends.ExpandAndGetRef().m_uiTargetBus = 3; // Music sends to SFX, not an aux bus

    sErrors.Clear();
    PL_TEST_BOOL(desc.Validate(sErrors).Failed());
    PL_TEST_BOOL(sErrors.FindSubString("cycle") != nullptr);
    PL_TEST_BOOL(sErrors.FindSubString("not an aux bus") != nullptr);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Duplicate names and snapshot indices")
  {
    plAudioMixerDesc desc = MakeValid();
    desc.m_Buses[1].m_sName = "sfx";

    plAudioSnapshotDesc& snapshot = desc.m_Snapshots.ExpandAndGetRef();
    snapshot.m_sName = "Combat";
    plAudioSnapshotOverrideDesc& ovr = snapshot.m_Overrides.ExpandAndGetRef();
    ovr.m_uiBus = 3;
    ovr.m_eTarget = plAudioSnapshotTarget::EffectParam;
    ovr.m_uiIndexA = 4; // SFX has one effect

    sErrors.Clear();
    PL_TEST_BOOL(desc.Validate(sErrors).Failed());
    PL_TEST_BOOL(sErrors.FindSubString("Two buses are named") != nullptr);
    PL_TEST_BOOL(sErrors.FindSubString("overrides effect 4") != nullptr);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "A cyclic mixer builds without a feedback loop")
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    plAudioMixerDesc desc = MakeValid();
    desc.m_Buses[1].m_uiParent = 3;
    desc.m_Buses[3].m_uiParent = 1;

    // logs the routing errors; the cyclic parents are connected to the output instead
    plTestLogInterface log;
    plTestLogSystemScope logScope(&log);
    log.ExpectMessage("the mixer has routing errors", plLogMsgType::ErrorMsg);

    PL_TEST_RESULT(engine.GetBusGraph()->Build(desc, engine.GetEngine()));
    engine.Render(plOfflineAudioEngine::BlockFrames, nullptr);
  }
}

namespace
{
  /// Renders the engine continuously, as the device thread would.
  class plDeviceThread final : public plThread
  {
  public:
    explicit plDeviceThread(plOfflineAudioEngine& ref_engine)
      : plThread("PlasmaAudio Test Device")
      , m_Engine(ref_engine)
    {
    }

    plAtomicBool m_bStop;
    plAtomicInteger32 m_iBlocks;
    plAtomicBool m_bNonFinite;

  private:
    virtual plUInt32 Run() override
    {
      while (!m_bStop)
      {
        plDynamicArray<float> block;
        m_Engine.Render(plOfflineAudioEngine::BlockFrames, &block);

        for (float f : block)
        {
          if (!plMath::IsFinite(f))
            m_bNonFinite = true;
        }

        m_iBlocks.Increment();
        plThreadUtils::YieldTimeSlice();
      }
      return 0;
    }

    plOfflineAudioEngine& m_Engine;
  };
} // namespace

PL_CREATE_SIMPLE_TEST(Realtime, ThreadedDspStress)
{
  // The engine renders on its own thread while the audio thread changes every live setting: occlusion filters, positions,
  // pitches, bus volumes and every insert effect parameter. Settings reach the device thread only through atomics.
  constexpr plUInt32 uiEmitters = 96;
  constexpr plUInt32 uiUpdates = 300;

  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:DspStress", []()
    {
      plAudioEventResourceDescriptor res = MakeEvent(plAudioTestUtils::MakeCosineCycle(4800, 10, 0.1f), true, true);
      res.m_Desc.m_sOutputBus = "SFX";
      return res;
    }());

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  // master: limiter; SFX: low pass, compressor, delay, EQ, sends to an aux reverb
  plAudioMixerDesc mixer;
  mixer.MakeDefault();
  mixer.m_Buses[0].m_Effects.ExpandAndGetRef().m_eType = plAudioEffectType::Limiter;

  plAudioBusDesc& reverb = mixer.m_Buses.ExpandAndGetRef();
  reverb.m_sName = "Reverb";
  reverb.m_uiParent = 0;
  reverb.m_bIsAux = true;
  reverb.m_sEnvironmentName = "Cave";
  reverb.m_Effects.ExpandAndGetRef().m_eType = plAudioEffectType::Reverb;

  plAudioBusDesc& sfx = mixer.m_Buses.ExpandAndGetRef();
  sfx.m_sName = "SFX";
  sfx.m_uiParent = 0;
  sfx.m_Sends.ExpandAndGetRef().m_uiTargetBus = 1;
  for (plAudioEffectType::Enum eType : {plAudioEffectType::LowPass, plAudioEffectType::Compressor, plAudioEffectType::Delay, plAudioEffectType::Eq3})
  {
    sfx.m_Effects.ExpandAndGetRef().m_eType = eType;
  }

  for (plAudioBusDesc& bus : mixer.m_Buses)
  {
    for (plAudioEffectDesc& effect : bus.m_Effects)
    {
      effect.InitDefaults();
    }
  }

  plMiniAudioBusGraph* pGraph = engine.GetBusGraph();
  if (!PL_TEST_RESULT(pGraph->Build(mixer, engine.GetEngine())))
    return;

  plPlasmaAudioEventManager manager;
  BudgetScope budgets(48, 512);

  plDynamicArray<plUniquePtr<Emitter>> emitters;
  for (plUInt32 i = 0; i < uiEmitters; ++i)
  {
    emitters.ExpandAndGetRef() = PL_DEFAULT_NEW(Emitter);
    Emitter* pEmitter = emitters.PeekBack().Borrow();
    pEmitter->m_Entity.m_LastTransform.m_vPosition = plVec3(static_cast<float>(i % 12) * 4.0f, static_cast<float>(i / 12) * 4.0f, 0.0f);
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:DspStress", &pEmitter->m_Entity, &pEmitter->m_Event, pGraph, engine.GetEngine()));
  }

  plDeviceThread device(engine);
  device.Start();

  plRandom rng;
  rng.Initialize(1234);
  const plVec3 listener[] = {plVec3(20, 10, 0)};

  plUInt32 uiMaxReal = 0;
  for (plUInt32 uiUpdate = 0; uiUpdate < uiUpdates; ++uiUpdate)
  {
    for (auto& pEmitter : emitters)
    {
      pEmitter->m_Entity.m_fTargetOcclusion = static_cast<float>(rng.DoubleZeroToOneInclusive());
      pEmitter->m_Entity.m_fPitchMultiplier = static_cast<float>(rng.DoubleMinMax(0.5, 2.0));
      pEmitter->m_Entity.m_LastTransform.m_vPosition += plVec3(static_cast<float>(rng.DoubleMinMax(-1, 1)), static_cast<float>(rng.DoubleMinMax(-1, 1)), 0.0f);
    }

    for (plUInt32 uiBus = 0; uiBus < mixer.m_Buses.GetCount(); ++uiBus)
    {
      pGraph->SetBusVolume(uiBus, static_cast<float>(rng.DoubleMinMax(0.2, 1.0)));

      for (plUInt32 uiEffect = 0; uiEffect < mixer.m_Buses[uiBus].m_Effects.GetCount(); ++uiEffect)
      {
        const plAudioEffectDesc& effect = mixer.m_Buses[uiBus].m_Effects[uiEffect];
        for (plUInt32 uiParam = 0; uiParam < plAudioEffectMaxParams; ++uiParam)
        {
          // within +-50 % of the defaults; the filter orders and delay times change as well
          const float fDefault = effect.m_fParams[uiParam];
          pGraph->SetBusEffectParameter(uiBus, uiEffect, uiParam, fDefault * static_cast<float>(rng.DoubleMinMax(0.5, 1.5)));
        }
      }
    }

    manager.Update(plTime::MakeFromMilliseconds(5), listener);
    uiMaxReal = plMath::Max(uiMaxReal, manager.GetRealVoiceCount());

    plThreadUtils::Sleep(plTime::MakeFromMilliseconds(1));
  }

  device.m_bStop = true;
  device.Join();

  plStringBuilder sMsg;
  sMsg.SetFormat("Realtime/ThreadedDspStress: {} updates while the device thread rendered {} blocks, up to {} real voices", uiUpdates, static_cast<plInt32>(device.m_iBlocks), uiMaxReal);
  plTestFramework::Output(plTestOutput::Message, "%s", sMsg.GetData());

  PL_TEST_BOOL(device.m_iBlocks > 0);
  PL_TEST_BOOL(uiMaxReal > 0);
  PL_TEST_BOOL(!device.m_bNonFinite);

  manager.StopAll(true);
}
