#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <AudioSystem/Core/AudioMiddleware.h>
#include <AudioSystem/Mixer/AudioMixerService.h>
#include <PlasmaAudioPlugin/Mixer/MiniAudioBusGraph.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

PL_CREATE_SIMPLE_TEST_GROUP(Mixer);

namespace
{
  /// \brief A middleware that only implements the mixer graph SPI, the same way plPlasmaAudioMiddleware does: the
  /// engine's plAudioMixerService drives a plMiniAudioBusGraph built on the offline engine.
  class plMixerTestMiddleware final : public plAudioMiddleware
  {
  public:
    explicit plMixerTestMiddleware(ma_engine* pEngine)
      : m_pEngine(pEngine)
    {
    }

    PL_NODISCARD plMiniAudioBusGraph& GetBusGraph() { return m_BusGraph; }

    plResult LoadMixer(const plAudioMixerDesc& desc) override { return m_BusGraph.Build(desc, m_pEngine); }

    plResult UnloadMixer() override
    {
      m_BusGraph.Teardown();
      return PL_SUCCESS;
    }

    plResult SetBusVolume(plUInt32 uiBusIndex, float fLinearGain) override
    {
      m_BusGraph.SetBusVolume(uiBusIndex, fLinearGain);
      return PL_SUCCESS;
    }

    plResult SetBusPitch(plUInt32 uiBusIndex, float fPitch) override
    {
      m_BusGraph.SetBusPitch(uiBusIndex, fPitch);
      return PL_SUCCESS;
    }

    plResult SetBusSendLevel(plUInt32 uiBusIndex, plUInt32 uiSendIndex, float fLinearLevel) override
    {
      m_BusGraph.SetBusSendLevel(uiBusIndex, uiSendIndex, fLinearLevel);
      return PL_SUCCESS;
    }

    plResult SetBusEffectParameter(plUInt32 uiBusIndex, plUInt32 uiEffectIndex, plUInt32 uiParamIndex, float fValue) override
    {
      m_BusGraph.SetBusEffectParameter(uiBusIndex, uiEffectIndex, uiParamIndex, fValue);
      return PL_SUCCESS;
    }

    // the rest of the middleware interface is not used by the mixer service
    plResult LoadConfiguration(const plOpenDdlReaderElement&) override { return PL_SUCCESS; }
    plResult Startup() override { return PL_SUCCESS; }
    void Update(plTime) override {}
    plResult Shutdown() override { return PL_SUCCESS; }
    plResult Release() override { return PL_SUCCESS; }
    plResult StopAllSounds() override { return PL_SUCCESS; }
    plResult AddEntity(plAudioSystemEntityData*, const char*) override { return PL_SUCCESS; }
    plResult ResetEntity(plAudioSystemEntityData*) override { return PL_SUCCESS; }
    plResult UpdateEntity(plAudioSystemEntityData*) override { return PL_SUCCESS; }
    plResult RemoveEntity(plAudioSystemEntityData*) override { return PL_SUCCESS; }
    plResult SetEntityTransform(plAudioSystemEntityData*, const plAudioSystemTransform&) override { return PL_SUCCESS; }
    plResult LoadTrigger(plAudioSystemEntityData*, const plAudioSystemTriggerData*, plAudioSystemEventData*) override { return PL_SUCCESS; }
    plResult ActivateTrigger(plAudioSystemEntityData*, const plAudioSystemTriggerData*, plAudioSystemEventData*) override { return PL_SUCCESS; }
    plResult UnloadTrigger(plAudioSystemEntityData*, const plAudioSystemTriggerData*) override { return PL_SUCCESS; }
    plResult StopEvent(plAudioSystemEntityData*, const plAudioSystemEventData*) override { return PL_SUCCESS; }
    plResult StopAllEvents(plAudioSystemEntityData*) override { return PL_SUCCESS; }
    plResult SetRtpc(plAudioSystemEntityData*, const plAudioSystemRtpcData*, float) override { return PL_SUCCESS; }
    plResult ResetRtpc(plAudioSystemEntityData*, const plAudioSystemRtpcData*) override { return PL_SUCCESS; }
    plResult SetSwitchState(plAudioSystemEntityData*, const plAudioSystemSwitchStateData*) override { return PL_SUCCESS; }
    plResult SetObstructionAndOcclusion(plAudioSystemEntityData*, float, float) override { return PL_SUCCESS; }
    plResult SetEnvironmentAmount(plAudioSystemEntityData*, const plAudioSystemEnvironmentData*, float) override { return PL_SUCCESS; }
    plResult AddListener(plAudioSystemListenerData*, const char*, bool) override { return PL_SUCCESS; }
    plResult ResetListener(plAudioSystemListenerData*) override { return PL_SUCCESS; }
    plResult RemoveListener(plAudioSystemListenerData*) override { return PL_SUCCESS; }
    plResult SetListenerTransform(plAudioSystemListenerData*, const plAudioSystemTransform&) override { return PL_SUCCESS; }
    plResult LoadBank(plAudioSystemBankData*) override { return PL_SUCCESS; }
    plResult UnloadBank(plAudioSystemBankData*) override { return PL_SUCCESS; }
    plAudioSystemEntityData* CreateWorldEntity(plAudioSystemDataID) override { return nullptr; }
    plAudioSystemEntityData* CreateEntityData(plAudioSystemDataID) override { return nullptr; }
    plResult DestroyEntityData(plAudioSystemEntityData*) override { return PL_SUCCESS; }
    plAudioSystemListenerData* CreateListenerData(plAudioSystemDataID) override { return nullptr; }
    plResult DestroyListenerData(plAudioSystemListenerData*) override { return PL_SUCCESS; }
    plAudioSystemEventData* CreateEventData(plAudioSystemDataID) override { return nullptr; }
    plResult ResetEventData(plAudioSystemEventData*) override { return PL_SUCCESS; }
    plResult DestroyEventData(plAudioSystemEventData*) override { return PL_SUCCESS; }
    plResult DestroyBank(plAudioSystemBankData*) override { return PL_SUCCESS; }
    plResult DestroyTriggerData(plAudioSystemTriggerData*) override { return PL_SUCCESS; }
    plResult DestroyRtpcData(plAudioSystemRtpcData*) override { return PL_SUCCESS; }
    plResult DestroySwitchStateData(plAudioSystemSwitchStateData*) override { return PL_SUCCESS; }
    plResult DestroyEnvironmentData(plAudioSystemEnvironmentData*) override { return PL_SUCCESS; }
    plResult SetLanguage(const char*) override { return PL_SUCCESS; }
    const char* GetMiddlewareName() const override { return "MixerTest"; }
    float GetMasterGain() const override { return 1.0f; }
    bool GetMute() const override { return false; }
    void OnMasterGainChange(float) override {}
    void OnMuteChange(bool) override {}
    void OnLoseFocus() override {}
    void OnGainFocus() override {}

  private:
    ma_engine* m_pEngine = nullptr;
    plMiniAudioBusGraph m_BusGraph;
  };

  enum Bus : plUInt32
  {
    Master = 0,
    Music = 1,
    Sfx = 2,
  };

  plAudioMixerDesc MakeBlendMixer()
  {
    plAudioMixerDesc desc;

    auto& master = desc.m_Buses.ExpandAndGetRef();
    master.m_sName = "Master";

    auto& music = desc.m_Buses.ExpandAndGetRef();
    music.m_sName = "Music";
    music.m_uiParent = Master;
    music.m_fVolumeDb = -6.0f;

    auto& sfx = desc.m_Buses.ExpandAndGetRef();
    sfx.m_sName = "SFX";
    sfx.m_uiParent = Master;
    sfx.m_fPitch = 1.0f;

    auto& game = desc.m_Vcas.ExpandAndGetRef();
    game.m_sName = "Game";
    game.m_fVolumeDb = -6.0f;
    game.m_Buses.PushBack(Music);
    game.m_Buses.PushBack(Sfx);

    auto& musicVca = desc.m_Vcas.ExpandAndGetRef();
    musicVca.m_sName = "MusicVCA";
    musicVca.m_fVolumeDb = -3.0f;
    musicVca.m_Buses.PushBack(Music);

    // listed high priority first so that the blend order has to come from sorting, not from the array order
    {
      auto& high = desc.m_Snapshots.ExpandAndGetRef();
      high.m_sName = "High";
      high.m_FadeIn = plTime::MakeZero();
      high.m_FadeOut = plTime::MakeZero();
      high.m_uiPriority = 5;
      auto& o = high.m_Overrides.ExpandAndGetRef();
      o.m_uiBus = Sfx;
      o.m_eTarget = plAudioSnapshotTarget::BusVolume;
      o.m_fValue = -40.0f;
    }
    {
      auto& low = desc.m_Snapshots.ExpandAndGetRef();
      low.m_sName = "Low";
      low.m_FadeIn = plTime::MakeZero();
      low.m_FadeOut = plTime::MakeZero();
      low.m_uiPriority = 1;
      auto& o = low.m_Overrides.ExpandAndGetRef();
      o.m_uiBus = Sfx;
      o.m_eTarget = plAudioSnapshotTarget::BusVolume;
      o.m_fValue = -20.0f;
    }
    {
      auto& fade = desc.m_Snapshots.ExpandAndGetRef();
      fade.m_sName = "Fade";
      fade.m_FadeIn = plTime::MakeFromSeconds(1.0);
      fade.m_FadeOut = plTime::MakeFromSeconds(1.0);
      fade.m_uiPriority = 0;
      auto& vol = fade.m_Overrides.ExpandAndGetRef();
      vol.m_uiBus = Sfx;
      vol.m_eTarget = plAudioSnapshotTarget::BusVolume;
      vol.m_fValue = -30.0f;
      auto& pitch = fade.m_Overrides.ExpandAndGetRef();
      pitch.m_uiBus = Sfx;
      pitch.m_eTarget = plAudioSnapshotTarget::BusPitch;
      pitch.m_fValue = 0.5f;
    }

    return desc;
  }

  float GetBusVolume(plMiniAudioBusGraph& ref_graph, plUInt32 uiBus)
  {
    return ma_sound_group_get_volume(ref_graph.GetBusInput(uiBus));
  }

  float GetBusPitch(plMiniAudioBusGraph& ref_graph, plUInt32 uiBus)
  {
    return ma_sound_group_get_pitch(ref_graph.GetBusInput(uiBus));
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Mixer, BlendMath)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "DbToLinear")
  {
    PL_TEST_FLOAT(plAudioMixerUtils::DbToLinear(0.0f), 1.0f, 1e-6f);
    PL_TEST_FLOAT(plAudioMixerUtils::DbToLinear(-6.0f), 0.501187f, 1e-5f);
    PL_TEST_BOOL(plAudioMixerUtils::DbToLinear(-60.0f) == 0.0f);
    PL_TEST_BOOL(plAudioMixerUtils::DbToLinear(-100.0f) == 0.0f);
    PL_TEST_BOOL(plAudioMixerUtils::DbToLinear(-59.9f) > 0.0f);
    PL_TEST_FLOAT(plAudioMixerUtils::LinearToDb(0.0f), -60.0f, 0.0f);
    PL_TEST_FLOAT(plAudioMixerUtils::LinearToDb(plAudioMixerUtils::DbToLinear(-12.0f)), -12.0f, 1e-4f);
  }

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  // the mixer service loads the mixer from its resource and pushes the authored values into the middleware
  plAudioMixerResourceHandle hMixer;
  {
    plAudioMixerResourceDescriptor descriptor;
    descriptor.m_Desc = MakeBlendMixer();
    hMixer = plResourceManager::GetOrCreateResource<plAudioMixerResource>("PlasmaAudioTest:BlendMixer", std::move(descriptor));
  }

  plMixerTestMiddleware middleware(engine.GetEngine());
  plMiniAudioBusGraph& graph = middleware.GetBusGraph();

  plAudioMixerService service;
  if (!PL_TEST_RESULT(service.LoadMixer("PlasmaAudioTest:BlendMixer", &middleware)))
    return;

  PL_TEST_BLOCK(plTestBlock::Enabled, "VCA dB sum")
  {
    // Music: bus -6 dB, VCA Game -6 dB, VCA MusicVCA -3 dB
    PL_TEST_FLOAT(GetBusVolume(graph, Music), plAudioMixerUtils::DbToLinear(-15.0f), 1e-5f);
    // SFX: bus 0 dB, VCA Game -6 dB
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), plAudioMixerUtils::DbToLinear(-6.0f), 1e-5f);
    // Master: no VCA
    PL_TEST_FLOAT(GetBusVolume(graph, Master), 1.0f, 1e-6f);

    // the graph keeps the pushed gains for the voice audibility: the bus times its parents
    PL_TEST_FLOAT(graph.GetBusGain(Music), plAudioMixerUtils::DbToLinear(-15.0f), 1e-5f);
    PL_TEST_FLOAT(graph.GetEffectiveBusGain(Music), plAudioMixerUtils::DbToLinear(-15.0f), 1e-5f);

    service.SetVcaVolume("Game", -12.0f);
    service.Update(plTime::MakeZero(), &middleware);
    PL_TEST_FLOAT(GetBusVolume(graph, Music), plAudioMixerUtils::DbToLinear(-21.0f), 1e-5f);
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), plAudioMixerUtils::DbToLinear(-12.0f), 1e-5f);

    // the legacy group volume maps onto the VCA
    PL_TEST_BOOL(service.SetGroupVolume("Game", 1.0f));
    service.Update(plTime::MakeZero(), &middleware);
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), 1.0f, 1e-5f);
    PL_TEST_FLOAT(GetBusVolume(graph, Music), plAudioMixerUtils::DbToLinear(-9.0f), 1e-5f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Snapshot priority")
  {
    service.SetSnapshotIntensity("Low", 1.0f);
    service.SetSnapshotIntensity("High", 1.0f);
    service.Update(plTime::MakeFromMilliseconds(10), &middleware);

    // Low (priority 1) applies first, High (priority 5) last and wins
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), plAudioMixerUtils::DbToLinear(-40.0f), 1e-6f);

    service.SetSnapshotIntensity("High", 0.0f);
    service.Update(plTime::MakeFromMilliseconds(10), &middleware);
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), plAudioMixerUtils::DbToLinear(-20.0f), 1e-6f);

    service.StopAllSnapshots();
    service.Update(plTime::MakeZero(), &middleware);
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), 1.0f, 1e-5f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Snapshot fade")
  {
    service.SetSnapshotIntensity("Fade", 1.0f);

    // fade position 0: nothing applied yet
    service.Update(plTime::MakeZero(), &middleware);
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), 1.0f, 1e-5f);
    PL_TEST_FLOAT(GetBusPitch(graph, Sfx), 1.0f, 1e-6f);

    // half of the 1 s fade-in: dB and pitch are interpolated linearly
    service.Update(plTime::MakeFromSeconds(0.5), &middleware);
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), plAudioMixerUtils::DbToLinear(-15.0f), 1e-5f);
    PL_TEST_FLOAT(GetBusPitch(graph, Sfx), 0.75f, 1e-5f);

    // full
    service.Update(plTime::MakeFromSeconds(0.5), &middleware);
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), plAudioMixerUtils::DbToLinear(-30.0f), 1e-6f);
    PL_TEST_FLOAT(GetBusPitch(graph, Sfx), 0.5f, 1e-5f);

    // an overshooting delta clamps at the target
    service.Update(plTime::MakeFromSeconds(5.0), &middleware);
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), plAudioMixerUtils::DbToLinear(-30.0f), 1e-6f);

    // fade-out back to half
    service.SetSnapshotIntensity("Fade", 0.0f);
    service.Update(plTime::MakeFromSeconds(0.5), &middleware);
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), plAudioMixerUtils::DbToLinear(-15.0f), 1e-5f);
  }

  service.UnloadMixer(&middleware);
  PL_TEST_BOOL(!graph.IsBuilt());
}
