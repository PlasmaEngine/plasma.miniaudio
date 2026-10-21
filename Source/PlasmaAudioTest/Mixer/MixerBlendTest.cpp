#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <AudioSystem/Mixer/AudioMixerService.h>
#include <PlasmaAudioTest/Harness/MixerTestMiddleware.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

PL_CREATE_SIMPLE_TEST_GROUP(Mixer);

namespace
{
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
