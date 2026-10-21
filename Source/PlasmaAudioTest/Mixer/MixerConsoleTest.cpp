#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <AudioSystem/Mixer/AudioMixerService.h>
#include <Core/ResourceManager/ResourceTypeLoader.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioTest/Harness/MixerTestMiddleware.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

PL_CREATE_SIMPLE_TEST_GROUP(MixerConsole);

namespace
{
  enum Bus : plUInt32
  {
    Master = 0,
    Music = 1,
    Sfx = 2,
    Foley = 3,
    Reverb = 4,
  };

  /// Master, Music, SFX with its child Foley, and the aux bus Reverb that SFX sends to.
  plAudioMixerDesc MakeConsoleMixer()
  {
    plAudioMixerDesc desc;

    desc.m_Buses.ExpandAndGetRef().m_sName = "Master";

    plAudioBusDesc& music = desc.m_Buses.ExpandAndGetRef();
    music.m_sName = "Music";
    music.m_uiParent = Master;
    music.m_fVolumeDb = -6.0f;

    plAudioBusDesc& sfx = desc.m_Buses.ExpandAndGetRef();
    sfx.m_sName = "SFX";
    sfx.m_uiParent = Master;
    sfx.m_Sends.ExpandAndGetRef().m_uiTargetBus = Reverb;

    plAudioBusDesc& foley = desc.m_Buses.ExpandAndGetRef();
    foley.m_sName = "Foley";
    foley.m_uiParent = Sfx;

    plAudioBusDesc& reverb = desc.m_Buses.ExpandAndGetRef();
    reverb.m_sName = "Reverb";
    reverb.m_uiParent = Master;
    reverb.m_bIsAux = true;

    plAudioSnapshotDesc& snapshot = desc.m_Snapshots.ExpandAndGetRef();
    snapshot.m_sName = "Combat";
    snapshot.m_FadeIn = plTime::MakeFromSeconds(2.0);
    plAudioSnapshotOverrideDesc& ovr = snapshot.m_Overrides.ExpandAndGetRef();
    ovr.m_uiBus = Music;
    ovr.m_eTarget = plAudioSnapshotTarget::BusVolume;
    ovr.m_fValue = -20.0f;

    return desc;
  }

  float GetBusVolume(plMiniAudioBusGraph& ref_graph, plUInt32 uiBus)
  {
    return ma_sound_group_get_volume(ref_graph.GetBusInput(uiBus));
  }

  /// Makes the desc the content of the resource, as the editor's mixer console does with unsaved edits.
  void UpdateMixerResource(const plAudioMixerResourceHandle& hMixer, const plAudioMixerDesc& desc)
  {
    plUniquePtr<plResourceLoaderFromMemory> pLoader(PL_DEFAULT_NEW(plResourceLoaderFromMemory));
    pLoader->m_ModificationTimestamp = plTimestamp::CurrentTimestamp();
    pLoader->m_sResourceDescription = "MixerConsoleTest";

    plMemoryStreamWriter writer(&pLoader->m_CustomData);
    writer << plString("MixerConsoleTest.plAudioMixer");
    plAssetFileHeader header;
    header.SetFileHashAndVersion(1, 1);
    header.Write(writer).IgnoreResult();
    desc.Save(writer).IgnoreResult();

    plResourceManager::UpdateResourceWithCustomLoader(hMixer, std::move(pLoader));
    plResourceManager::ForceLoadResourceNow(hMixer);
  }

  /// Noise without pauses, fed into a bus for the dynamics tests.
  bool InitSteadyNoise(ma_engine* pEngine, plMaTestSignalNode& ref_node, plUInt32 uiSeed)
  {
    ref_node.m_fPeriodSeconds = 1000.0f;
    ref_node.m_fBurstSeconds = 1000.0f;
    ref_node.m_fLevel = 0.5f;
    ref_node.m_uiNoiseState = uiSeed;
    return plMiniAudioDsp::InitTestSignalNode(pEngine, &ref_node).Succeeded();
  }

  void RenderSeconds(plOfflineAudioEngine& ref_engine, double fSeconds)
  {
    const plUInt32 uiBlocks = static_cast<plUInt32>(fSeconds * plOfflineAudioEngine::SampleRate / plOfflineAudioEngine::BlockFrames);
    for (plUInt32 i = 0; i < uiBlocks; ++i)
    {
      ref_engine.Render(plOfflineAudioEngine::BlockFrames, nullptr);
    }
  }
} // namespace

PL_CREATE_SIMPLE_TEST(MixerConsole, EffectRegistry)
{
  PL_TEST_INT(plAudioEffectRegistry::GetTypeCount(), 8);

  for (plUInt32 i = 0; i < plAudioEffectRegistry::GetTypeCount(); ++i)
  {
    const plAudioEffectTypeInfo& info = plAudioEffectRegistry::GetTypeInfo(static_cast<plAudioEffectType::Enum>(i));
    PL_TEST_BOOL(info.m_Parameters.GetCount() <= plAudioEffectMaxParams);

    // every default lies in its range, and InitDefaults writes the registry defaults
    plAudioEffectDesc effect;
    effect.m_eType = static_cast<plAudioEffectType::Enum>(i);
    effect.InitDefaults();

    for (plUInt32 p = 0; p < info.m_Parameters.GetCount(); ++p)
    {
      const plAudioEffectParameterInfo& param = info.m_Parameters[p];
      PL_TEST_BOOL(param.m_fDefault >= param.m_fMin && param.m_fDefault <= param.m_fMax);
      PL_TEST_FLOAT(effect.m_fParams[p], param.m_fDefault, 0.0f);
      PL_TEST_INT(plAudioEffectRegistry::FindParameter(effect.m_eType, param.m_szName), p);
    }
  }

  PL_TEST_INT(plAudioEffectRegistry::FindParameter(plAudioEffectType::Compressor, "threshold"), 0);
  PL_TEST_INT(plAudioEffectRegistry::FindParameter(plAudioEffectType::Compressor, "Depth"), plInvalidIndex);
  PL_TEST_BOOL(plAudioEffectRegistry::GetTypeInfo(plAudioEffectType::Ducker).m_bNeedsSidechain);

  plAudioEffectType::Enum eType = plAudioEffectType::Default;
  PL_TEST_BOOL(plAudioEffectRegistry::FindType("Comp", eType) && eType == plAudioEffectType::Compressor);
  PL_TEST_BOOL(plAudioEffectRegistry::FindType("Ducker", eType) && eType == plAudioEffectType::Ducker);
  PL_TEST_BOOL(!plAudioEffectRegistry::FindType("Flanger", eType));
}

PL_CREATE_SIMPLE_TEST(MixerConsole, DescVersion2)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Sidechain and bypass round trip")
  {
    plAudioMixerDesc desc = MakeConsoleMixer();
    plAudioEffectDesc& duck = desc.m_Buses[Music].m_Effects.ExpandAndGetRef();
    duck.m_eType = plAudioEffectType::Ducker;
    duck.InitDefaults();
    duck.m_uiSidechainBus = Sfx;
    duck.m_bBypass = true;

    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    PL_TEST_RESULT(desc.Save(writer));

    plMemoryStreamReader reader(&storage);
    plAudioMixerDesc loaded;
    PL_TEST_RESULT(loaded.Load(reader));

    PL_TEST_INT(loaded.m_Buses[Music].m_Effects.GetCount(), 1);
    PL_TEST_INT(loaded.m_Buses[Music].m_Effects[0].m_uiSidechainBus, Sfx);
    PL_TEST_BOOL(loaded.m_Buses[Music].m_Effects[0].m_bBypass);
    PL_TEST_BOOL(loaded.HasSameLayout(desc));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Version 1 loads without sidechain and bypass")
  {
    // what a mixer transformed before version 2 holds: one master bus with one compressor
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    writer.WriteVersion(1);
    writer << plUInt32(1);
    writer << plString("Master") << plUInt8(plAudioNoBus) << 0.0f << 1.0f << false << plString();
    writer << plUInt32(1);
    writer << plUInt8(plAudioEffectType::Compressor);
    for (plUInt32 p = 0; p < plAudioEffectMaxParams; ++p)
      writer << float(p);
    writer << plUInt32(0); // sends
    writer << plUInt32(0); // VCAs
    writer << plUInt32(0); // snapshots

    plMemoryStreamReader reader(&storage);
    plAudioMixerDesc loaded;
    PL_TEST_RESULT(loaded.Load(reader));

    PL_TEST_INT(loaded.m_Buses.GetCount(), 1);
    PL_TEST_INT(loaded.m_Buses[0].m_Effects.GetCount(), 1);
    PL_TEST_INT(loaded.m_Buses[0].m_Effects[0].m_uiSidechainBus, plAudioNoBus);
    PL_TEST_BOOL(!loaded.m_Buses[0].m_Effects[0].m_bBypass);
    PL_TEST_FLOAT(loaded.m_Buses[0].m_Effects[0].m_fParams[3], 3.0f, 0.0f);
  }
}

PL_CREATE_SIMPLE_TEST(MixerConsole, Validation)
{
  plStringBuilder sErrors;
  plDynamicArray<plAudioMixerIssue> issues;

  PL_TEST_BLOCK(plTestBlock::Enabled, "Both ends of a routing cycle")
  {
    plAudioMixerDesc desc = MakeConsoleMixer();
    desc.m_Buses[Sfx].m_uiParent = Foley; // SFX -> Foley -> SFX

    sErrors.Clear();
    issues.Clear();
    PL_TEST_BOOL(desc.Validate(sErrors, &issues).Failed());

    bool bFound = false;
    for (const plAudioMixerIssue& issue : issues)
    {
      if (issue.m_sMessage.FindSubString("cycle") != nullptr)
        bFound |= (issue.m_uiBus == Sfx && issue.m_uiOtherBus == Foley) || (issue.m_uiBus == Foley && issue.m_uiOtherBus == Sfx);
    }
    PL_TEST_BOOL(bFound);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Sidechain checks")
  {
    plAudioMixerDesc desc = MakeConsoleMixer();
    plAudioEffectDesc& duck = desc.m_Buses[Music].m_Effects.ExpandAndGetRef();
    duck.m_eType = plAudioEffectType::Ducker;
    duck.InitDefaults();

    // keyed by a sibling: fine
    duck.m_uiSidechainBus = Sfx;
    sErrors.Clear();
    PL_TEST_RESULT(desc.Validate(sErrors));

    // keyed by its own parent, which it feeds: a cycle with both buses named
    duck.m_uiSidechainBus = Master;
    sErrors.Clear();
    issues.Clear();
    PL_TEST_BOOL(desc.Validate(sErrors, &issues).Failed());
    PL_TEST_BOOL(!issues.IsEmpty() && issues[0].m_uiBus == Music && issues[0].m_uiOtherBus == Master);
    PL_TEST_BOOL(desc.IsRoutedTo(Master, Music, true));
    PL_TEST_BOOL(!desc.IsRoutedTo(Master, Music, false));

    // keyed by itself, or by a bus that does not exist
    duck.m_uiSidechainBus = Music;
    sErrors.Clear();
    PL_TEST_BOOL(desc.Validate(sErrors).Failed());
    duck.m_uiSidechainBus = 42;
    sErrors.Clear();
    PL_TEST_BOOL(desc.Validate(sErrors).Failed());
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Same layout")
  {
    const plAudioMixerDesc a = MakeConsoleMixer();

    plAudioMixerDesc b = a;
    b.m_Buses[Music].m_fVolumeDb = -1.0f;
    b.m_Buses[Sfx].m_Sends[0].m_fLevelDb = -12.0f;
    b.m_Snapshots.Clear();
    PL_TEST_BOOL(a.HasSameLayout(b));

    plAudioMixerDesc c = a;
    c.m_Buses[Music].m_Effects.ExpandAndGetRef().InitDefaults();
    PL_TEST_BOOL(!a.HasSameLayout(c));

    plAudioMixerDesc d = c;
    d.m_Buses[Music].m_Effects[0].m_uiSidechainBus = Sfx;
    PL_TEST_BOOL(!c.HasSameLayout(d));

    plAudioMixerDesc e = a;
    e.m_Buses[Foley].m_uiParent = Master;
    PL_TEST_BOOL(!a.HasSameLayout(e));
  }
}

PL_CREATE_SIMPLE_TEST(MixerConsole, MuteSolo)
{
  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  {
    plAudioMixerResourceDescriptor descriptor;
    descriptor.m_Desc = MakeConsoleMixer();
    plResourceManager::GetOrCreateResource<plAudioMixerResource>("PlasmaAudioTest:ConsoleMixer", std::move(descriptor));
  }

  plMixerTestMiddleware middleware(engine.GetEngine());
  plMiniAudioBusGraph& graph = middleware.GetBusGraph();

  plAudioMixerService service;
  if (!PL_TEST_RESULT(service.LoadMixer("PlasmaAudioTest:ConsoleMixer", &middleware)))
    return;

  const float fMusic = plAudioMixerUtils::DbToLinear(-6.0f);

  PL_TEST_BLOCK(plTestBlock::Enabled, "Mute")
  {
    service.SetBusMute("Music", true);
    service.Update(plTime::MakeZero(), &middleware);
    PL_TEST_BOOL(service.IsBusMuted(Music));
    PL_TEST_FLOAT(GetBusVolume(graph, Music), 0.0f, 0.0f);
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), 1.0f, 1e-6f);

    service.SetBusMute("Music", false);
    service.Update(plTime::MakeZero(), &middleware);
    PL_TEST_FLOAT(GetBusVolume(graph, Music), fMusic, 1e-5f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Solo")
  {
    // Foley plays with what it feeds (SFX, Master and the Reverb that SFX sends to); Music is silenced
    service.SetBusSolo("Foley", true);
    service.Update(plTime::MakeZero(), &middleware);
    PL_TEST_FLOAT(GetBusVolume(graph, Music), 0.0f, 0.0f);
    PL_TEST_FLOAT(GetBusVolume(graph, Foley), 1.0f, 1e-6f);
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), 1.0f, 1e-6f);
    PL_TEST_FLOAT(GetBusVolume(graph, Master), 1.0f, 1e-6f);
    PL_TEST_FLOAT(GetBusVolume(graph, Reverb), 1.0f, 1e-6f);

    // soloing SFX as well keeps Foley, which feeds it
    service.SetBusSolo("SFX", true);
    service.Update(plTime::MakeZero(), &middleware);
    PL_TEST_FLOAT(GetBusVolume(graph, Foley), 1.0f, 1e-6f);
    PL_TEST_FLOAT(GetBusVolume(graph, Music), 0.0f, 0.0f);

    service.ClearMuteAndSolo();
    service.Update(plTime::MakeZero(), &middleware);
    PL_TEST_FLOAT(GetBusVolume(graph, Music), fMusic, 1e-5f);
    PL_TEST_BOOL(!service.IsBusSoloed(Foley));
  }

  service.UnloadMixer(&middleware);
}

PL_CREATE_SIMPLE_TEST(MixerConsole, HotApply)
{
  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  // a resource loaded through a custom loader, like the mixer the editor's engine process gets from the console
  plAudioMixerResourceHandle hMixer = plResourceManager::LoadResource<plAudioMixerResource>("PlasmaAudioTest:HotMixer");
  UpdateMixerResource(hMixer, MakeConsoleMixer());

  plMixerTestMiddleware middleware(engine.GetEngine());
  plMiniAudioBusGraph& graph = middleware.GetBusGraph();

  plAudioMixerService service;
  if (!PL_TEST_RESULT(service.LoadMixer("PlasmaAudioTest:HotMixer", &middleware)))
    return;

  PL_TEST_INT(middleware.m_uiLoads, 1);

  // what the game set: SFX lowered, Combat snapshot playing
  service.SetBusVolume("SFX", -10.0f);
  service.SetSnapshotIntensityNow("Combat", 0.5f);
  service.Update(plTime::MakeZero(), &middleware);

  PL_TEST_BLOCK(plTestBlock::Enabled, "Same layout")
  {
    plAudioMixerDesc desc = MakeConsoleMixer();
    desc.m_Buses[Music].m_fVolumeDb = -2.0f;
    PL_TEST_RESULT(service.ApplyDesc(desc, &middleware));

    // nothing was rebuilt; the changed authored volume applies, the game's SFX volume and the snapshot stay
    PL_TEST_INT(middleware.m_uiLoads, 1);
    PL_TEST_INT(service.GetLayoutRebuildCount(), 0);
    PL_TEST_FLOAT(GetBusVolume(graph, Music), plAudioMixerUtils::DbToLinear(plMath::Lerp(-2.0f, -20.0f, 0.5f)), 1e-5f);
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), plAudioMixerUtils::DbToLinear(-10.0f), 1e-5f);
    PL_TEST_FLOAT(service.GetSnapshotIntensity(0), 0.5f, 0.0f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Changed layout")
  {
    plAudioMixerDesc desc = MakeConsoleMixer();
    desc.m_Buses[Music].m_fVolumeDb = -2.0f;
    plAudioBusDesc& ui = desc.m_Buses.ExpandAndGetRef();
    ui.m_sName = "UI";
    ui.m_uiParent = Master;
    PL_TEST_RESULT(service.ApplyDesc(desc, &middleware));

    // the graph was rebuilt and got the carried-over values again
    PL_TEST_INT(middleware.m_uiLoads, 2);
    PL_TEST_INT(service.GetLayoutRebuildCount(), 1);
    PL_TEST_INT(graph.GetDesc().m_Buses.GetCount(), 6);
    PL_TEST_FLOAT(GetBusVolume(graph, Sfx), plAudioMixerUtils::DbToLinear(-10.0f), 1e-5f);
    PL_TEST_FLOAT(GetBusVolume(graph, 5), 1.0f, 1e-6f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Reloaded resource")
  {
    // the console writes a new desc into the resource; the next service update applies it in place
    plAudioMixerDesc desc = service.GetDesc();
    desc.m_Buses[Music].m_fVolumeDb = -1.0f;
    UpdateMixerResource(hMixer, desc);

    service.Update(plTime::MakeZero(), &middleware);
    PL_TEST_INT(middleware.m_uiLoads, 2);
    PL_TEST_FLOAT(service.GetDesc().m_Buses[Music].m_fVolumeDb, -1.0f, 0.0f);
    PL_TEST_FLOAT(GetBusVolume(graph, Music), plAudioMixerUtils::DbToLinear(plMath::Lerp(-1.0f, -20.0f, 0.5f)), 1e-5f);
  }

  service.UnloadMixer(&middleware);
}

PL_CREATE_SIMPLE_TEST(MixerConsole, SidechainDucker)
{
  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  // Music ducks by 12 dB while Dialogue is above -40 dBFS
  plAudioMixerDesc desc;
  desc.m_Buses.ExpandAndGetRef().m_sName = "Master";
  plAudioBusDesc& music = desc.m_Buses.ExpandAndGetRef();
  music.m_sName = "Music";
  music.m_uiParent = 0;
  plAudioEffectDesc& duck = music.m_Effects.ExpandAndGetRef();
  duck.m_eType = plAudioEffectType::Ducker;
  duck.InitDefaults();
  duck.m_fParams[0] = -12.0f; // depth
  duck.m_fParams[1] = -40.0f; // threshold
  duck.m_fParams[2] = 5.0f;   // attack ms
  duck.m_fParams[3] = 50.0f;  // release ms
  duck.m_uiSidechainBus = 2;
  plAudioBusDesc& dialogue = desc.m_Buses.ExpandAndGetRef();
  dialogue.m_sName = "Dialogue";
  dialogue.m_uiParent = 0;

  plMiniAudioBusGraph graph;
  if (!PL_TEST_RESULT(graph.Build(desc, engine.GetEngine())))
    return;

  plMaTestSignalNode musicSignal;
  plMaTestSignalNode dialogueSignal;
  if (!PL_TEST_BOOL(InitSteadyNoise(engine.GetEngine(), musicSignal, 0x1234u) && InitSteadyNoise(engine.GetEngine(), dialogueSignal, 0x9876u)))
    return;

  ma_node_attach_output_bus(&musicSignal.m_Base, 0, graph.GetBusInput(1), 0);

  PL_TEST_BLOCK(plTestBlock::Enabled, "Silent key")
  {
    RenderSeconds(engine, 0.3);
    PL_TEST_BOOL(graph.GetEffectGainReductionDb(1, 0) > -0.5f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Key above the threshold")
  {
    ma_node_attach_output_bus(&dialogueSignal.m_Base, 0, graph.GetBusInput(2), 0);
    RenderSeconds(engine, 0.3);
    PL_TEST_FLOAT(graph.GetEffectGainReductionDb(1, 0), -12.0f, 0.5f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Release")
  {
    ma_node_detach_output_bus(&dialogueSignal.m_Base, 0);
    RenderSeconds(engine, 0.6);
    PL_TEST_BOOL(graph.GetEffectGainReductionDb(1, 0) > -0.5f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Bypass")
  {
    ma_node_attach_output_bus(&dialogueSignal.m_Base, 0, graph.GetBusInput(2), 0);
    graph.SetBusEffectBypass(1, 0, true);
    RenderSeconds(engine, 0.3);
    PL_TEST_FLOAT(graph.GetEffectGainReductionDb(1, 0), 0.0f, 0.0f);
    PL_TEST_BOOL(graph.GetBusValues()[1].m_EffectBypass[0]);

    graph.SetBusEffectBypass(1, 0, false);
    RenderSeconds(engine, 0.3);
    PL_TEST_FLOAT(graph.GetEffectGainReductionDb(1, 0), -12.0f, 0.5f);
  }

  plMiniAudioDsp::UninitTestSignalNode(&dialogueSignal);
  plMiniAudioDsp::UninitTestSignalNode(&musicSignal);
  graph.Teardown();
}

PL_CREATE_SIMPLE_TEST(MixerConsole, RebuildKeepsVoices)
{
  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  constexpr plUInt32 uiCycleFrames = 4800;
  plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeCosineCycle(uiCycleFrames, 48, 0.5f));
  res.m_Desc.m_Instruments[0].m_bLoop = true;
  const plAudioEventResourceHandle hEvent = plAudioTestUtils::CreateEventResource("PlasmaAudioTest:RebuildLoop", std::move(res));

  plPlasmaAudioEventData eventData;
  eventData.m_bLooping = true;
  plPlasmaAudioEventManager manager;

  plMiniAudioBusGraph* pGraph = engine.GetBusGraph();
  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:RebuildLoop", nullptr, &eventData, pGraph, engine.GetEngine()));

  plDynamicArray<float> before;
  engine.RenderBlocks(20, &manager, &before);
  PL_TEST_INT(manager.GetRealVoiceCount(), 1);
  PL_TEST_BOOL(plAudioTestUtils::FindFirstAbove(before, 0.1f) != plInvalidIndex);

  // the graph is rebuilt with another layout underneath the playing voice, as the middleware does for a changed mixer
  plAudioMixerDesc desc;
  desc.m_Buses.ExpandAndGetRef().m_sName = "Master";
  plAudioBusDesc& extra = desc.m_Buses.ExpandAndGetRef();
  extra.m_sName = "Extra";
  extra.m_uiParent = 0;

  manager.DetachFromBusGraph(pGraph);
  PL_TEST_RESULT(pGraph->Build(desc, engine.GetEngine()));
  manager.ReattachToBusGraph(pGraph);

  // the same instance and voice play on, through the new master bus
  plDynamicArray<float> after;
  engine.RenderBlocks(20, &manager, &after);
  PL_TEST_INT(manager.GetInstanceCount(), 1);
  PL_TEST_INT(manager.GetRealVoiceCount(), 1);
  PL_TEST_BOOL(manager.IsEventPlaying(&eventData));
  // no gap: the cosine (100 frames per period) is there within one period of the rebuild
  PL_TEST_BOOL(plAudioTestUtils::FindFirstAbove(after, 0.1f) < 100);

  manager.StopAll(true);
}
