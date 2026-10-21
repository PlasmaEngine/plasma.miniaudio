#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <PlasmaAudioPlugin/Events/AudioSpatializer.h>
#include <PlasmaAudioPlugin/Events/AudioVoiceManager.h>
#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioPlugin/Profiling/AudioDebugSnapshot.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

// P4 spatial: the attenuation curves, cones and occlusion of the event description, the audition's distance, the
// spatializer interface and the Audio view mode's snapshot.
PL_CREATE_SIMPLE_TEST_GROUP(Spatial);

namespace
{
  plAudioEventDesc MakeCurve(plAudioAttenuationCurve::Enum curve, float fMin, float fMax)
  {
    plAudioEventDesc desc;
    desc.m_bSpatialize = true;
    desc.m_eAttenuationCurve = curve;
    desc.m_fMinDistance = fMin;
    desc.m_fMaxDistance = fMax;
    desc.m_fRolloff = 1.0f;
    return desc;
  }

  plAudioEventResourceDescriptor MakeSpatialEvent(float fValue)
  {
    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(plOfflineAudioEngine::SampleRate, fValue));
    res.m_Desc.m_uiMaxInstances = 0;
    res.m_Desc.m_eStealing = plAudioEventStealingMode::None;
    res.m_Desc.m_bSpatialize = true;
    res.m_Desc.m_eAttenuationCurve = plAudioAttenuationCurve::Linear;
    res.m_Desc.m_fMinDistance = 1.0f;
    res.m_Desc.m_fMaxDistance = 101.0f;
    res.m_Desc.m_Instruments[0].m_bLoop = true;
    return res;
  }

  /// Mean magnitude of the last uiFrames samples.
  float TailLevel(const plDynamicArray<float>& samples, plUInt32 uiFrames)
  {
    const plUInt32 uiStart = samples.GetCount() > uiFrames ? samples.GetCount() - uiFrames : 0;
    double fSum = 0.0;
    for (plUInt32 i = uiStart; i < samples.GetCount(); ++i)
      fSum += plMath::Abs(samples[i]);

    return samples.GetCount() > uiStart ? static_cast<float>(fSum / (samples.GetCount() - uiStart)) : 0.0f;
  }

  /// Passes the voice through unchanged and records what the event system tells it.
  class TestSpatializer : public plAudioSpatializer
  {
  public:
    plStringView GetName() const override { return "Test"; }

    ma_node* CreateVoiceNode(ma_engine* pEngine) override
    {
      ma_biquad_node* pNode = static_cast<ma_biquad_node*>(ma_malloc(sizeof(ma_biquad_node), nullptr));
      const ma_biquad_node_config config = ma_biquad_node_config_init(ma_engine_get_channels(pEngine), 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
      if (ma_biquad_node_init(ma_engine_get_node_graph(pEngine), &config, nullptr, pNode) != MA_SUCCESS)
      {
        ma_free(pNode, nullptr);
        return nullptr;
      }

      ++m_uiCreated;
      return pNode;
    }

    void DestroyVoiceNode(ma_node* pNode) override
    {
      ma_biquad_node_uninit(static_cast<ma_biquad_node*>(pNode), nullptr);
      ma_free(pNode, nullptr);
      ++m_uiDestroyed;
    }

    void UpdateVoice(ma_node* pNode, const plAudioSpatialVoiceState& state) override
    {
      ++m_uiUpdates;
      m_LastState = state;
    }

    plUInt32 m_uiCreated = 0;
    plUInt32 m_uiDestroyed = 0;
    plUInt32 m_uiUpdates = 0;
    plAudioSpatialVoiceState m_LastState;
  };
} // namespace

PL_CREATE_SIMPLE_TEST(Spatial, AttenuationCurves)
{
  // full volume up to the min distance and silence from the max distance on, whatever the curve
  for (plUInt32 uiCurve = 0; uiCurve <= plAudioAttenuationCurve::None; ++uiCurve)
  {
    const plAudioEventDesc desc = MakeCurve(static_cast<plAudioAttenuationCurve::Enum>(uiCurve), 2.0f, 102.0f);
    PL_TEST_FLOAT(desc.GetDistanceGain(0.0f), 1.0f, 0.0f);
    PL_TEST_FLOAT(desc.GetDistanceGain(2.0f), 1.0f, 0.0f);
    PL_TEST_FLOAT(desc.GetDistanceGain(102.0f), 0.0f, 0.0f);
    PL_TEST_FLOAT(desc.GetDistanceGain(500.0f), 0.0f, 0.0f);
  }

  // halfway between min and max, before the fade
  PL_TEST_FLOAT(MakeCurve(plAudioAttenuationCurve::Inverse, 2.0f, 102.0f).GetDistanceGain(4.0f), 0.5f, 0.0001f);
  PL_TEST_FLOAT(MakeCurve(plAudioAttenuationCurve::Linear, 2.0f, 102.0f).GetDistanceGain(52.0f), 0.5f, 0.0001f);
  PL_TEST_FLOAT(MakeCurve(plAudioAttenuationCurve::None, 2.0f, 102.0f).GetDistanceGain(52.0f), 1.0f, 0.0001f);

  // logarithmic: halfway in log distance is half the gain
  PL_TEST_FLOAT(MakeCurve(plAudioAttenuationCurve::Logarithmic, 1.0f, 100.0f).GetDistanceGain(10.0f), 0.5f, 0.0001f);

  // the last 15 % fade to silence: at 92.5 % of the way the linear 0.075 is halved
  PL_TEST_FLOAT(MakeCurve(plAudioAttenuationCurve::Linear, 0.0f, 100.0f).GetDistanceGain(92.5f), 0.075f * 0.5f, 0.0001f);

  // custom: straight lines from (0, 1) through the points, holding the last gain
  {
    plAudioEventDesc desc = MakeCurve(plAudioAttenuationCurve::Custom, 0.0f, 100.0f);
    desc.m_AttenuationPoints.PushBack(plVec2(0.2f, 0.8f));
    desc.m_AttenuationPoints.PushBack(plVec2(0.5f, 0.2f));
    PL_TEST_FLOAT(desc.GetDistanceGain(10.0f), 0.9f, 0.0001f);
    PL_TEST_FLOAT(desc.GetDistanceGain(35.0f), 0.5f, 0.0001f);
    PL_TEST_FLOAT(desc.GetDistanceGain(70.0f), 0.2f, 0.0001f);
  }

  // a 2D event is never attenuated
  {
    plAudioEventDesc desc = MakeCurve(plAudioAttenuationCurve::Linear, 1.0f, 10.0f);
    desc.m_bSpatialize = false;
    PL_TEST_FLOAT(desc.GetDistanceGain(50.0f), 1.0f, 0.0f);
  }
}

PL_CREATE_SIMPLE_TEST(Spatial, ConeAndOcclusion)
{
  plAudioEventDesc desc;
  desc.m_fConeInnerAngle = 90.0f;
  desc.m_fConeOuterAngle = 180.0f;
  desc.m_fConeOuterGainDb = -12.0f;

  const float fOuter = plMath::Pow(10.0f, -12.0f / 20.0f);
  PL_TEST_FLOAT(desc.GetConeGain(0.0f), 1.0f, 0.0f);
  PL_TEST_FLOAT(desc.GetConeGain(45.0f), 1.0f, 0.0f);
  PL_TEST_FLOAT(desc.GetConeGain(67.5f), plMath::Lerp(1.0f, fOuter, 0.5f), 0.0001f);
  PL_TEST_FLOAT(desc.GetConeGain(90.0f), fOuter, 0.0001f);
  PL_TEST_FLOAT(desc.GetConeGain(-170.0f), fOuter, 0.0001f);

  // the occlusion filter sweeps from 22 kHz down to the cutoff, and the volume to the occlusion volume
  desc.m_fOcclusionCutoffHz = 400.0f;
  desc.m_fOcclusionVolumeDb = -10.0f;
  PL_TEST_FLOAT(desc.GetOcclusionCutoff(0.0f), 22000.0f, 0.01f);
  PL_TEST_FLOAT(desc.GetOcclusionCutoff(1.0f), 400.0f, 0.01f);
  PL_TEST_BOOL(desc.GetOcclusionCutoff(0.5f) < 22000.0f && desc.GetOcclusionCutoff(0.5f) > 400.0f);
  PL_TEST_FLOAT(desc.GetOcclusionGain(0.0f), 1.0f, 0.0f);
  PL_TEST_FLOAT(desc.GetOcclusionGain(1.0f), plMath::Pow(10.0f, -0.5f), 0.0001f);
}

PL_CREATE_SIMPLE_TEST(Spatial, AuditionDistance)
{
  // The editor's audition plays on a global entity; with an audition distance it is attenuated like an emitter that
  // far away, and the built-in Distance parameter follows.
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:AuditionDistance", MakeSpatialEvent(0.5f));

  auto Render = [](float fDistance) -> float
  {
    plOfflineAudioEngine engine;
    if (!engine.IsValid())
      return -1.0f;

    plPlasmaAudioEventManager manager;
    plPlasmaAudioEntityData entity(7, true);
    entity.m_fAuditionDistance = fDistance;
    plPlasmaAudioEventData event;

    plDynamicArray<float> output;
    if (manager.ActivateEvent("PlasmaAudioTest:AuditionDistance", &entity, &event, engine.GetBusGraph(), engine.GetEngine()).Failed())
      return -1.0f;

    engine.RenderBlocks(40, &manager, &output);
    manager.StopAll(true);
    return TailLevel(output, 10 * plOfflineAudioEngine::BlockFrames);
  };

  const float fNear = Render(-1.0f);
  const float fHalf = Render(51.0f); // halfway on the linear curve from 1 to 101 m
  const float fFar = Render(150.0f);

  plTestFramework::Output(plTestOutput::Message, "Spatial/AuditionDistance: unspatialized %.4f, 51 m %.4f, 150 m %.4f", fNear, fHalf, fFar);

  if (!PL_TEST_BOOL(fNear > 0.01f))
    return;

  PL_TEST_FLOAT(fHalf / fNear, 0.5f, 0.02f);
  PL_TEST_BOOL(fFar < fNear * 0.001f);
}

PL_CREATE_SIMPLE_TEST(Spatial, SpatializerInterface)
{
  // An active spatializer gets a node for every spatialized voice, its updates while the voice plays, and the node back
  // when the voice ends. The sound passes through the node.
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:Spatializer", MakeSpatialEvent(0.5f));

  TestSpatializer spatializer;
  plAudioSpatializer::SetActive(&spatializer);
  PL_SCOPE_EXIT(plAudioSpatializer::SetActive(nullptr));

  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    plPlasmaAudioEventManager manager;
    plPlasmaAudioEntityData entity(8);
    entity.m_LastTransform.m_vPosition = plVec3(3, 0, 0);
    plPlasmaAudioEventData event;

    plDynamicArray<float> output;
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:Spatializer", &entity, &event, engine.GetBusGraph(), engine.GetEngine()));
    engine.RenderBlocks(20, &manager, &output);

    PL_TEST_INT(spatializer.m_uiCreated, 1);
    PL_TEST_BOOL(spatializer.m_uiUpdates >= 2);
    PL_TEST_VEC3(spatializer.m_LastState.m_vEmitterPosition, plVec3(3, 0, 0), 0.0001f);
    PL_TEST_BOOL(TailLevel(output, 5 * plOfflineAudioEngine::BlockFrames) > 0.01f);

    manager.StopAll(true);
    engine.RenderBlocks(4, &manager, nullptr);
  }

  PL_TEST_INT(spatializer.m_uiDestroyed, spatializer.m_uiCreated);
}

PL_CREATE_SIMPLE_TEST(Spatial, DebugSnapshot)
{
  // The Audio view mode's snapshot lists spatialized emitters with their distance and gain, and leaves 2D ones out.
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:Snapshot", MakeSpatialEvent(0.5f));

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plPlasmaAudioEventManager manager;

  plPlasmaAudioEntityData emitter(9);
  emitter.m_LastTransform.m_vPosition = plVec3(26, 0, 0);
  plPlasmaAudioEventData emitterEvent;

  plPlasmaAudioEntityData global(10, true);
  plPlasmaAudioEventData globalEvent;

  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:Snapshot", &emitter, &emitterEvent, engine.GetBusGraph(), engine.GetEngine()));
  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:Snapshot", &global, &globalEvent, engine.GetBusGraph(), engine.GetEngine()));
  engine.RenderBlocks(10, &manager, nullptr);

  plAudioDebugSnapshot snapshot;
  manager.GetDebugSnapshot(snapshot);

  if (PL_TEST_INT(snapshot.m_Emitters.GetCount(), 1))
  {
    const plAudioDebugEmitter& entry = snapshot.m_Emitters[0];
    PL_TEST_FLOAT(entry.m_fDistance, 26.0f, 0.001f);
    PL_TEST_FLOAT(entry.m_fGain, 0.75f, 0.001f); // linear from 1 to 101 m
    PL_TEST_FLOAT(entry.m_fMaxDistance, 101.0f, 0.0f);
    PL_TEST_BOOL(entry.m_bReal);
  }

  manager.StopAll(true);
}
