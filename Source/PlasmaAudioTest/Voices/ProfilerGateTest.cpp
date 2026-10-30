#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <AudioSystem/Core/AudioSystemData.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <Foundation/Types/ScopeExit.h>
#include <Foundation/Utilities/Stats.h>
#include <PlasmaAudioPlugin/Events/AudioVoiceManager.h>
#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioPlugin/MiniAudioMiddleware.h>
#include <PlasmaAudioPlugin/Profiling/AudioEndedVoices.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilerCapture.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilerVoiceQuery.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

/// The AU-F27 gate: what the Audio Profiler shows reconciles with the runtime's counters and with saved captures.
PL_CREATE_SIMPLE_TEST_GROUP(AudioProfilerGate);

namespace
{
  struct GateBudgetScope
  {
    GateBudgetScope(plInt32 iReal, plInt32 iVirtual) { plAudioVoiceManager::SetBudgets(iReal, iVirtual); }
    ~GateBudgetScope() { plAudioVoiceManager::SetBudgets(-1, -1); }
  };

  struct GateEmitter
  {
    plPlasmaAudioEntityData m_Entity;
    plPlasmaAudioEventData m_Event;
  };

  /// A spatialized single-instrument event without instance limit. Its one-second sample keeps the stops of the tests
  /// away from the end of a pass, where a stop ends the voice without its fade.
  plAudioEventResourceDescriptor MakeGateEvent(bool bLoop, plTime stopFadeOut)
  {
    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(plOfflineAudioEngine::SampleRate, 0.25f));
    res.m_Desc.m_uiMaxInstances = 0;
    res.m_Desc.m_eStealing = plAudioEventStealingMode::None;
    res.m_Desc.m_bSpatialize = true;
    res.m_Desc.m_fMinDistance = 1.0f;
    res.m_Desc.m_fMaxDistance = 100.0f;
    res.m_Desc.m_fRolloff = 1.0f;
    res.m_Desc.m_Instruments[0].m_bLoop = bLoop;
    res.m_Desc.m_StopFadeOut = stopFadeOut;
    return res;
  }

  plDynamicArray<plUInt8> Serialize(const plAudioProfilingSnapshot& snapshot)
  {
    plDynamicArray<plUInt8> data;
    plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&data);
    plMemoryStreamWriter writer(&storage);
    snapshot.Write(writer);
    return data;
  }

  plResult Deserialize(plArrayPtr<const plUInt8> data, plAudioProfilingSnapshot& out_snapshot)
  {
    plRawMemoryStreamReader reader(data.GetPtr(), data.GetCount());
    return out_snapshot.Read(reader);
  }

  /// Real and virtual voices as the rows show them: a stopping voice counts by its own state.
  void CountRows(const plAudioProfilingSnapshot& snapshot, plUInt32& out_uiReal, plUInt32& out_uiVirtual, plUInt32& out_uiBadState)
  {
    out_uiReal = 0;
    out_uiVirtual = 0;
    out_uiBadState = 0;

    for (const plAudioVoiceSnapshotEntry& voice : snapshot.m_Voices)
    {
      switch (voice.m_uiState)
      {
        case plAudioVoiceSnapshotEntry::Real:
          out_uiBadState += voice.m_bVirtual ? 1 : 0;
          ++out_uiReal;
          break;
        case plAudioVoiceSnapshotEntry::Virtual:
          out_uiBadState += voice.m_bVirtual ? 0 : 1;
          ++out_uiVirtual;
          break;
        case plAudioVoiceSnapshotEntry::Stopping:
          ++(voice.m_bVirtual ? out_uiVirtual : out_uiReal);
          break;
        default:
          ++out_uiBadState;
          break;
      }
    }
  }

  /// The first way the snapshot disagrees with the manager's counters, or an empty string.
  plString FindMismatch(const plAudioProfilingSnapshot& snapshot, const plPlasmaAudioEventManager& manager)
  {
    plUInt32 uiReal = 0, uiVirtual = 0, uiBadState = 0;
    CountRows(snapshot, uiReal, uiVirtual, uiBadState);

    plStringBuilder s;
    if (uiBadState != 0)
      s.SetFormat("{} rows with a state that contradicts their virtual flag", uiBadState);
    else if (uiReal != snapshot.m_uiRealVoices || uiVirtual != snapshot.m_uiVirtualVoices)
      s.SetFormat("rows {} real / {} virtual, counters {} / {}", uiReal, uiVirtual, snapshot.m_uiRealVoices, snapshot.m_uiVirtualVoices);
    else if (snapshot.m_uiRealVoices != manager.GetRealVoiceCount() || snapshot.m_uiVirtualVoices != manager.GetVirtualVoiceCount())
      s.SetFormat("snapshot {} / {}, manager {} / {}", snapshot.m_uiRealVoices, snapshot.m_uiVirtualVoices, manager.GetRealVoiceCount(), manager.GetVirtualVoiceCount());
    else if (snapshot.m_uiInstances != manager.GetInstanceCount())
      s.SetFormat("snapshot {} instances, manager {}", snapshot.m_uiInstances, manager.GetInstanceCount());
    else if (snapshot.m_uiRealVoices > snapshot.m_uiMaxReal || snapshot.m_uiVirtualVoices > snapshot.m_uiMaxVirtual)
      s.SetFormat("{} real of {}, {} virtual of {}", snapshot.m_uiRealVoices, snapshot.m_uiMaxReal, snapshot.m_uiVirtualVoices, snapshot.m_uiMaxVirtual);

    return s;
  }

  plUInt32 CountEnded(const plAudioProfilingSnapshot& snapshot, plAudioStopReason::Enum reason)
  {
    plUInt32 uiCount = 0;
    for (const plAudioEndedVoiceSnapshotEntry& ended : snapshot.m_Ended)
    {
      if (ended.m_Voice.m_uiStopReason == reason)
        ++uiCount;
    }
    return uiCount;
  }

  plUInt32 CountLog(const plPlasmaAudioEventManager& manager, plAudioLogEventType::Enum type, plStringView sDetail)
  {
    plDynamicArray<plAudioEventLogSnapshotEntry> entries;
    manager.GetEventLog().Collect(0, entries);

    plUInt32 uiCount = 0;
    for (const plAudioEventLogSnapshotEntry& entry : entries)
    {
      if (entry.m_uiType == type && (sDetail.IsEmpty() || entry.m_sDetail == sDetail))
        ++uiCount;
    }
    return uiCount;
  }

  plAudioVoiceSnapshotEntry MakeRow(plUInt32 uiId, plUInt8 uiState, plStringView sEvent, plStringView sEntity, plStringView sBus, float fAudibility)
  {
    plAudioVoiceSnapshotEntry voice;
    voice.m_uiId = uiId;
    voice.m_uiState = uiState;
    voice.m_bVirtual = uiState == plAudioVoiceSnapshotEntry::Virtual;
    voice.m_sEvent = sEvent;
    voice.m_sEntity = sEntity;
    voice.m_sBus = sBus;
    voice.m_fAudibility = fAudibility;
    return voice;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(AudioProfilerGate, VoicesMatchCounters)
{
  // 20 looping emitters on a line under a 4 real / 16 virtual budget, a listener that walks along the line, and every
  // fourth emitter stopped with a fade half-way: at every update the rows, the snapshot's counters and the manager
  // agree, and a capture of the run gives back every snapshot
  GateBudgetScope budgets(4, 16);

  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:GateCounters", MakeGateEvent(true, plTime::MakeFromMilliseconds(60)));

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plPlasmaAudioEventManager manager;
  manager.SetEndedVoicesEnabled(true);

  constexpr plUInt32 uiEmitters = 20;
  plDynamicArray<plUniquePtr<GateEmitter>> emitters;
  for (plUInt32 i = 0; i < uiEmitters; ++i)
  {
    plUniquePtr<GateEmitter> pEmitter = PL_DEFAULT_NEW(GateEmitter);
    pEmitter->m_Entity.m_uiEntityId = 900 + i;
    pEmitter->m_Entity.m_LastTransform.m_vPosition = plVec3(4.0f * i, 0.0f, 0.0f);
    pEmitter->m_Event.m_bLooping = true;
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:GateCounters", &pEmitter->m_Entity, &pEmitter->m_Event, engine.GetBusGraph(), engine.GetEngine()));
    emitters.PushBack(std::move(pEmitter));
  }

  constexpr plUInt32 uiBlocks = 200;
  plAudioProfilerCapture capture;
  plDynamicArray<plDynamicArray<plUInt8>> recorded;
  plAudioProfilingSnapshot snapshot;
  plUInt32 uiMismatches = 0;
  plStringBuilder sFirstMismatch;
  plUInt32 uiMaxRealSeen = 0;
  plUInt32 uiMaxVirtualSeen = 0;
  bool bSawStopping = false;

  for (plUInt32 uiBlock = 0; uiBlock < uiBlocks; ++uiBlock)
  {
    // half a second into the second pass of the one-second loops
    if (uiBlock == 150)
    {
      for (plUInt32 i = 0; i < uiEmitters; i += 4)
        PL_TEST_RESULT(manager.StopEvent(&emitters[i]->m_Event, false));
    }

    const plVec3 vListener(0.5f * uiBlock, 0.0f, 0.0f);
    engine.Render(plOfflineAudioEngine::BlockFrames, nullptr);
    manager.Update(plOfflineAudioEngine::GetBlockDuration(), vListener);

    manager.FillSnapshot(snapshot);
    snapshot.m_fTime = uiBlock * plOfflineAudioEngine::GetBlockDuration().GetSeconds();

    const plString sMismatch = FindMismatch(snapshot, manager);
    if (!sMismatch.IsEmpty())
    {
      if (uiMismatches == 0)
        sFirstMismatch.SetFormat("update {}: {}", uiBlock, sMismatch);
      ++uiMismatches;
    }

    uiMaxRealSeen = plMath::Max(uiMaxRealSeen, snapshot.m_uiRealVoices);
    uiMaxVirtualSeen = plMath::Max(uiMaxVirtualSeen, snapshot.m_uiVirtualVoices);
    for (const plAudioVoiceSnapshotEntry& voice : snapshot.m_Voices)
      bSawStopping |= voice.m_uiState == plAudioVoiceSnapshotEntry::Stopping;

    recorded.PushBack(Serialize(snapshot));
    PL_TEST_RESULT(capture.AddSnapshot(recorded.PeekBack(), 0.0));
  }

  PL_TEST_INT(uiMismatches, 0);
  if (uiMismatches != 0)
    plTestFramework::Output(plTestOutput::Error, "first mismatch at %s", sFirstMismatch.GetData());

  // the run exercised what it is meant to: a full real budget, virtual voices and fading instances
  PL_TEST_INT(uiMaxRealSeen, 4);
  PL_TEST_BOOL(uiMaxVirtualSeen > 0);
  PL_TEST_BOOL(bSawStopping);

  // the stopped instances' voices ended with the reason the game gave
  PL_TEST_INT(CountEnded(snapshot, plAudioStopReason::Requested), uiEmitters / 4);

  PL_TEST_BLOCK(plTestBlock::Enabled, "Capture reconciles")
  {
    plDynamicArray<plUInt8> file;
    {
      plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&file);
      plMemoryStreamWriter writer(&storage);
      PL_TEST_RESULT(capture.Save(writer));
    }

    plAudioProfilerCapture loaded;
    {
      plRawMemoryStreamReader reader(file.GetData(), file.GetCount());
      PL_TEST_RESULT(loaded.Load(reader));
    }

    if (!PL_TEST_INT(loaded.GetSamples().GetCount(), uiBlocks))
      return;

    plUInt32 uiDifferences = 0;
    plAudioProfilingSnapshot live, replayed;
    for (plUInt32 i = 0; i < uiBlocks; ++i)
    {
      PL_TEST_RESULT(Deserialize(recorded[i], live));
      if (loaded.GetSnapshot(i, replayed).Failed())
      {
        ++uiDifferences;
        continue;
      }

      const plAudioProfilerCapture::Sample& sample = loaded.GetSamples()[i];
      bool bSame = replayed.m_uiRealVoices == live.m_uiRealVoices && replayed.m_uiVirtualVoices == live.m_uiVirtualVoices && replayed.m_uiInstances == live.m_uiInstances;
      bSame &= sample.m_uiReal == live.m_uiRealVoices && sample.m_uiVirtual == live.m_uiVirtualVoices && sample.m_uiMemoryBytes == live.m_uiMemoryBytes;
      bSame &= replayed.m_Voices.GetCount() == live.m_Voices.GetCount() && replayed.m_Ended.GetCount() == live.m_Ended.GetCount();

      for (plUInt32 v = 0; bSame && v < live.m_Voices.GetCount(); ++v)
      {
        const plAudioVoiceSnapshotEntry& a = live.m_Voices[v];
        const plAudioVoiceSnapshotEntry& b = replayed.m_Voices[v];
        bSame &= a.m_uiId == b.m_uiId && a.m_uiState == b.m_uiState && a.m_bVirtual == b.m_bVirtual && a.m_uiStopReason == b.m_uiStopReason && a.m_fVolume == b.m_fVolume;
      }

      for (plUInt32 e = 0; bSame && e < live.m_Ended.GetCount(); ++e)
      {
        bSame &= live.m_Ended[e].m_Voice.m_uiId == replayed.m_Ended[e].m_Voice.m_uiId && live.m_Ended[e].m_Voice.m_uiStopReason == replayed.m_Ended[e].m_Voice.m_uiStopReason;
      }

      uiDifferences += bSame ? 0 : 1;
    }

    PL_TEST_INT(uiDifferences, 0);
  }

  manager.StopAll(true);
  for (auto& pEmitter : emitters)
    manager.DetachEventData(&pEmitter->m_Event);
}

PL_CREATE_SIMPLE_TEST(AudioProfilerGate, StolenVoices)
{
  GateBudgetScope budgets(64, 512);

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  constexpr plTime fadeOut = plTime::MakeFromMilliseconds(50);

  // fades take 50 ms = 5 blocks; the polyphony group holds two instances
  for (const char* szMode : {"Oldest", "Quietest"})
  {
    const bool bOldest = plStringUtils::IsEqual(szMode, "Oldest");
    const plAudioStopReason::Enum expected = bOldest ? plAudioStopReason::StolenOldest : plAudioStopReason::StolenQuietest;

    plStringBuilder sEvent;
    sEvent.SetFormat("PlasmaAudioTest:GateSteal{}", szMode);

    plAudioEventResourceDescriptor res = MakeGateEvent(true, fadeOut);
    res.m_Desc.m_uiMaxInstances = 2;
    res.m_Desc.m_eStealing = bOldest ? plAudioEventStealingMode::Oldest : plAudioEventStealingMode::Quietest;
    plAudioTestUtils::CreateEventResource(sEvent, std::move(res));

    plPlasmaAudioEventManager manager;
    manager.SetEndedVoicesEnabled(true);

    // later emitters are closer, so the quietest instance is always the farthest one still playing
    GateEmitter emitters[5];
    plAudioProfilingSnapshot snapshot;
    plUInt32 uiStoppingStolen = 0;

    for (plUInt32 i = 0; i < 5; ++i)
    {
      emitters[i].m_Entity.m_uiEntityId = 700 + i;
      emitters[i].m_Entity.m_LastTransform.m_vPosition = plVec3(50.0f - 10.0f * i, 0.0f, 0.0f);
      emitters[i].m_Event.m_bLooping = true;
      PL_TEST_RESULT(manager.ActivateEvent(sEvent, &emitters[i].m_Entity, &emitters[i].m_Event, engine.GetBusGraph(), engine.GetEngine()));

      engine.RenderBlocks(1, &manager, nullptr);
      manager.FillSnapshot(snapshot);

      for (const plAudioVoiceSnapshotEntry& voice : snapshot.m_Voices)
      {
        if (voice.m_uiState == plAudioVoiceSnapshotEntry::Stopping && voice.m_uiStopReason == expected)
          ++uiStoppingStolen;
      }

      engine.RenderBlocks(9, &manager, nullptr);
    }

    manager.FillSnapshot(snapshot);

    PL_TEST_INT(uiStoppingStolen, 3);
    PL_TEST_INT(CountLog(manager, plAudioLogEventType::Stolen, bOldest ? "oldest" : "quietest"), 3);
    PL_TEST_INT(CountEnded(snapshot, expected), 3);
    PL_TEST_INT(snapshot.m_Ended.GetCount(), 3);

    // the stolen ones are the first three emitters, oldest and farthest alike
    for (const plAudioEndedVoiceSnapshotEntry& ended : snapshot.m_Ended)
    {
      PL_TEST_INT(ended.m_Voice.m_uiState, plAudioVoiceSnapshotEntry::Ended);
      PL_TEST_BOOL(ended.m_Voice.m_uiEntityId >= 700 && ended.m_Voice.m_uiEntityId < 703);
      PL_TEST_STRING(ended.m_Voice.m_sEvent, sEvent);
      PL_TEST_BOOL(plAudioStopReason::IsForced(static_cast<plAudioStopReason::Enum>(ended.m_Voice.m_uiStopReason)));
    }

    manager.StopAll(true);
    for (GateEmitter& emitter : emitters)
      manager.DetachEventData(&emitter.m_Event);
  }
}

PL_CREATE_SIMPLE_TEST(AudioProfilerGate, VirtualBudgetKills)
{
  // 8 looping emitters under a 2 real / 2 virtual budget: the four quietest voices are killed by the virtual budget,
  // logged as such, and listed as ended with that reason
  GateBudgetScope budgets(2, 2);

  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:GateBudgetKill", MakeGateEvent(true, plTime::MakeZero()));

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plPlasmaAudioEventManager manager;
  manager.SetEndedVoicesEnabled(true);

  GateEmitter emitters[8];
  for (plUInt32 i = 0; i < 8; ++i)
  {
    emitters[i].m_Entity.m_uiEntityId = 800 + i;
    emitters[i].m_Entity.m_LastTransform.m_vPosition = plVec3(2.0f + 5.0f * i, 0.0f, 0.0f);
    emitters[i].m_Event.m_bLooping = true;
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:GateBudgetKill", &emitters[i].m_Entity, &emitters[i].m_Event, engine.GetBusGraph(), engine.GetEngine()));
  }

  engine.RenderBlocks(20, &manager, nullptr);

  plAudioProfilingSnapshot snapshot;
  manager.FillSnapshot(snapshot);

  PL_TEST_STRING(FindMismatch(snapshot, manager), "");
  PL_TEST_INT(snapshot.m_uiRealVoices, 2);
  PL_TEST_INT(snapshot.m_uiVirtualVoices, 2);
  PL_TEST_INT(CountLog(manager, plAudioLogEventType::Killed, "virtual voice budget"), 4);
  PL_TEST_INT(CountEnded(snapshot, plAudioStopReason::KilledVirtualBudget), 4);

  for (const plAudioEndedVoiceSnapshotEntry& ended : snapshot.m_Ended)
  {
    PL_TEST_BOOL(ended.m_Voice.m_uiEntityId >= 804);
  }

  manager.StopAll(true);
  for (GateEmitter& emitter : emitters)
    manager.DetachEventData(&emitter.m_Event);
}

PL_CREATE_SIMPLE_TEST(AudioProfilerGate, EndedWindow)
{
  plAudioEndedVoices ended;
  plDynamicArray<plAudioEndedVoiceSnapshotEntry> collected;

  PL_TEST_BLOCK(plTestBlock::Enabled, "Disabled records nothing")
  {
    ended.Add(MakeRow(1, plAudioVoiceSnapshotEntry::Real, "A", "", "SFX", 1.0f), 1.0);
    ended.Collect(1.0, plAudioProfilingSnapshot::EndedWindow, collected);
    PL_TEST_BOOL(collected.IsEmpty());
    PL_TEST_INT(ended.GetTotalAdded(), 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Window")
  {
    ended.SetEnabled(true);
    for (plUInt32 i = 0; i < 10; ++i)
    {
      ended.Add(MakeRow(i, plAudioVoiceSnapshotEntry::Real, "A", "", "SFX", 1.0f), static_cast<double>(i));
    }

    // at 12 s the voices that ended at 7, 8 and 9 s are within 5 s; oldest first, marked as ended
    ended.Collect(12.0, plAudioProfilingSnapshot::EndedWindow, collected);
    if (PL_TEST_INT(collected.GetCount(), 3))
    {
      PL_TEST_INT(collected[0].m_Voice.m_uiId, 7);
      PL_TEST_INT(collected[2].m_Voice.m_uiId, 9);
      PL_TEST_INT(collected[0].m_Voice.m_uiState, plAudioVoiceSnapshotEntry::Ended);
      PL_TEST_DOUBLE(collected[0].m_fEndTime, 7.0, 0.0);
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Overflow keeps the newest")
  {
    for (plUInt32 i = 0; i < plAudioEndedVoices::Capacity + 50; ++i)
    {
      ended.Add(MakeRow(1000 + i, plAudioVoiceSnapshotEntry::Virtual, "B", "", "SFX", 1.0f), 100.0);
    }

    ended.Collect(100.0, plAudioProfilingSnapshot::EndedWindow, collected);
    if (PL_TEST_INT(collected.GetCount(), plAudioEndedVoices::Capacity))
    {
      PL_TEST_INT(collected[0].m_Voice.m_uiId, 1050);
      PL_TEST_INT(collected.PeekBack().m_Voice.m_uiId, 1000 + plAudioEndedVoices::Capacity + 49);
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Disabling drops the record")
  {
    ended.SetEnabled(false);
    ended.SetEnabled(true);
    ended.Collect(100.0, plAudioProfilingSnapshot::EndedWindow, collected);
    PL_TEST_BOOL(collected.IsEmpty());
  }
}

PL_CREATE_SIMPLE_TEST(AudioProfilerGate, ParametersInSnapshot)
{
  plAudioEventResourceDescriptor res = MakeGateEvent(true, plTime::MakeZero());
  for (plUInt32 i = 0; i < 10; ++i)
  {
    plStringBuilder sName;
    sName.SetFormat("Param{}", i);

    plAudioEventParameterDesc& param = res.m_Desc.m_Parameters.ExpandAndGetRef();
    param.m_sName = sName;
    param.m_fMin = 0.0f;
    param.m_fMax = 10.0f;
    param.m_fDefault = static_cast<float>(i);
  }

  // a reserved name is not shown
  res.m_Desc.m_Parameters[1].m_sName = "AudioSystem.Hidden";
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:GateParameters", std::move(res));

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plPlasmaAudioEventManager manager;
  GateEmitter emitter;
  emitter.m_Event.m_bLooping = true;
  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:GateParameters", &emitter.m_Entity, &emitter.m_Event, engine.GetBusGraph(), engine.GetEngine()));
  engine.RenderBlocks(2, &manager, nullptr);

  manager.SetEventParameter(&emitter.m_Entity, "Param0", 7.5f);
  engine.RenderBlocks(2, &manager, nullptr);

  plAudioProfilingSnapshot snapshot;
  manager.FillSnapshot(snapshot);

  if (PL_TEST_INT(snapshot.m_Voices.GetCount(), 1))
  {
    const plAudioVoiceSnapshotEntry& voice = snapshot.m_Voices[0];
    if (PL_TEST_INT(voice.m_Parameters.GetCount(), plAudioVoiceSnapshotEntry::MaxParameters))
    {
      PL_TEST_STRING(voice.m_Parameters[0].m_sName, "Param0");
      PL_TEST_FLOAT(voice.m_Parameters[0].m_fValue, 7.5f, 0.0001f);

      // Param1 is the hidden one: the list goes on with Param2
      PL_TEST_STRING(voice.m_Parameters[1].m_sName, "Param2");
      PL_TEST_FLOAT(voice.m_Parameters[1].m_fValue, 2.0f, 0.0001f);

      for (const plAudioVoiceParameterSnapshotEntry& parameter : voice.m_Parameters)
        PL_TEST_BOOL(!parameter.m_sName.StartsWith("AudioSystem."));
    }

    // the parameters survive serialization
    plAudioProfilingSnapshot read;
    if (PL_TEST_RESULT(Deserialize(Serialize(snapshot), read)) && PL_TEST_INT(read.m_Voices.GetCount(), 1))
    {
      PL_TEST_INT(read.m_Voices[0].m_Parameters.GetCount(), voice.m_Parameters.GetCount());
      PL_TEST_FLOAT(read.m_Voices[0].m_Parameters[0].m_fValue, 7.5f, 0.0f);
    }
  }

  manager.StopAll(true);
  manager.DetachEventData(&emitter.m_Event);
}

PL_CREATE_SIMPLE_TEST(AudioProfilerGate, MiddlewareReconciles)
{
  // the real middleware: the published snapshot, GetProfilingInfo and the memory rows tell the same story
  plCVarInt* pSnapshotHz = static_cast<plCVarInt*>(plCVar::FindCVarByName("PlasmaAudio.ProfilerSnapshotHz"));
  if (!PL_TEST_BOOL(pSnapshotHz != nullptr))
    return;

  const plInt32 iOldHz = pSnapshotHz->GetValue();
  *pSnapshotHz = 1000;
  PL_SCOPE_EXIT(*pSnapshotHz = iOldHz);

  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:GateMiddleware", MakeGateEvent(true, plTime::MakeZero()));

  plUniquePtr<plPlasmaAudioMiddleware> pMiddleware = PL_DEFAULT_NEW(plPlasmaAudioMiddleware);
  if (!PL_TEST_RESULT(pMiddleware->Startup()))
    return;

  PL_SCOPE_EXIT(pMiddleware->Shutdown().IgnoreResult());

  plHybridArray<plAudioSystemEntityData*, 3> entities;
  plHybridArray<plAudioSystemEventData*, 3> events;
  for (plUInt32 i = 0; i < 3; ++i)
  {
    entities.PushBack(pMiddleware->CreateEntityData(10 + i));
    events.PushBack(pMiddleware->CreateEventData(10 + i));
    plAudioSystemStringTriggerData trigger(20 + i, "PlasmaAudioTest:GateMiddleware");
    PL_TEST_RESULT(pMiddleware->ActivateTrigger(entities[i], &trigger, events[i]));
  }

  // snapshots are taken at most every millisecond
  for (plUInt32 i = 0; i < 10; ++i)
  {
    plThreadUtils::Sleep(plTime::MakeFromMilliseconds(2));
    pMiddleware->Update(plTime::MakeFromMilliseconds(10));
  }

  plAudioSystemProfilingInfo info;
  pMiddleware->GetProfilingInfo(info);
  pMiddleware->PublishStats();

  const plVariant& stat = plStats::GetStat("PlasmaAudio/VoiceSnapshot");
  if (!PL_TEST_BOOL(stat.IsA<plDataBuffer>()))
    return;

  const plDataBuffer& data = stat.Get<plDataBuffer>();
  plAudioProfilingSnapshot snapshot;
  if (!PL_TEST_RESULT(Deserialize(data, snapshot)))
    return;

  plUInt32 uiReal = 0, uiVirtual = 0, uiBadState = 0;
  CountRows(snapshot, uiReal, uiVirtual, uiBadState);

  PL_TEST_INT(uiBadState, 0);
  PL_TEST_INT(snapshot.m_uiInstances, 3);
  PL_TEST_INT(uiReal + uiVirtual, 3);
  PL_TEST_INT(uiReal, snapshot.m_uiRealVoices);
  PL_TEST_INT(uiVirtual, snapshot.m_uiVirtualVoices);
  PL_TEST_INT(snapshot.m_uiRealVoices, info.m_uiActiveVoices);
  PL_TEST_INT(snapshot.m_uiInstances, info.m_uiEventInstances);

  // one memory total: the rows of the memory section add up to it, and the middleware reports the same
  plUInt64 uiClipRows = 0, uiEventRows = 0;
  for (const plAudioClipMemorySnapshotEntry& clip : snapshot.m_Clips)
    uiClipRows += clip.m_uiResidentBytes;
  for (const plAudioEventMemorySnapshotEntry& event : snapshot.m_Events)
    uiEventRows += event.m_uiSampleBytes;

  PL_TEST_BOOL(snapshot.m_uiEmbeddedSampleBytes > 0);
  PL_TEST_BOOL(snapshot.m_uiEmbeddedSampleBytes == uiEventRows);
  PL_TEST_BOOL(snapshot.m_uiClipResidentBytes == uiClipRows);
  PL_TEST_BOOL(snapshot.m_uiMemoryBytes == snapshot.m_uiClipResidentBytes + snapshot.m_uiStreamRingBytes + snapshot.m_uiEmbeddedSampleBytes);
  PL_TEST_BOOL(snapshot.m_uiMemoryBytes == info.m_uiMemoryUsed);

  // the timings are measured, not made up: within their range
  PL_TEST_BOOL(snapshot.m_fUpdateMs >= 0.0f && snapshot.m_fUpdateMs < 1000.0f);
  PL_TEST_BOOL(snapshot.m_fDspLoad >= 0.0f && snapshot.m_fDspLoad <= 1.0f);

  for (plUInt32 i = 0; i < 3; ++i)
  {
    pMiddleware->StopEvent(entities[i], events[i]).IgnoreResult();
    pMiddleware->DestroyEventData(events[i]).IgnoreResult();
    pMiddleware->DestroyEntityData(entities[i]).IgnoreResult();
  }

  pMiddleware->RemoveProfilingStats();
}

PL_CREATE_SIMPLE_TEST(AudioProfilerGate, VoiceQuery)
{
  plAudioProfilingSnapshot snapshot;
  snapshot.m_Voices.PushBack(MakeRow(0x100, plAudioVoiceSnapshotEntry::Virtual, "Sfx/Footsteps/Concrete", "Player", "SFX", 0.9f));
  snapshot.m_Voices.PushBack(MakeRow(0x200, plAudioVoiceSnapshotEntry::Real, "Music/Combat", "", "Music", 0.5f));
  snapshot.m_Voices.PushBack(MakeRow(0x300, plAudioVoiceSnapshotEntry::Real, "Sfx/Impacts/Metal", "Crate_12", "SFX", 0.7f));
  snapshot.m_Voices.PushBack(MakeRow(0x400, plAudioVoiceSnapshotEntry::Stopping, "Amb/Wind", "WindEmitter", "Ambience", 0.7f));

  plAudioEndedVoiceSnapshotEntry& ended = snapshot.m_Ended.ExpandAndGetRef();
  ended.m_Voice = MakeRow(0x500, plAudioVoiceSnapshotEntry::Ended, "Sfx/Footsteps/Concrete", "Enemy_03", "SFX", 0.95f);
  ended.m_Voice.m_uiStopReason = plAudioStopReason::StolenOldest;

  plDynamicArray<const plAudioVoiceSnapshotEntry*> rows;

  auto Ids = [&]()
  {
    plStringBuilder s;
    for (const plAudioVoiceSnapshotEntry* pRow : rows)
      s.AppendFormat("{} ", plArgU(pRow->m_uiId, 1, false, 16));
    s.Trim(" ");
    return plString(s);
  };

  PL_TEST_BLOCK(plTestBlock::Enabled, "Default order, ended hidden")
  {
    plAudioProfilerVoiceQuery query;
    PL_TEST_INT(query.Run(snapshot, rows), 4);
    PL_TEST_STRING(Ids(), "300 200 100 400");
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Ended shown last")
  {
    plAudioProfilerVoiceQuery query;
    query.m_StateFilter[plAudioVoiceSnapshotEntry::Ended] = true;
    PL_TEST_INT(query.Run(snapshot, rows), 5);
    PL_TEST_STRING(Ids(), "300 200 100 400 500");
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "State filter")
  {
    plAudioProfilerVoiceQuery query;
    query.m_StateFilter[plAudioVoiceSnapshotEntry::Real] = false;
    query.m_StateFilter[plAudioVoiceSnapshotEntry::Stopping] = false;
    PL_TEST_INT(query.Run(snapshot, rows), 1);
    PL_TEST_STRING(Ids(), "100");
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Bus filter")
  {
    plAudioProfilerVoiceQuery query;
    query.m_sBus = "sfx";
    query.m_StateFilter[plAudioVoiceSnapshotEntry::Ended] = true;
    PL_TEST_INT(query.Run(snapshot, rows), 3);
    PL_TEST_STRING(Ids(), "300 100 500");
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Text matches event, owner and bus")
  {
    plAudioProfilerVoiceQuery query;
    query.m_sText = "foot";
    PL_TEST_INT(query.Run(snapshot, rows), 1);

    query.m_sText = "crate";
    PL_TEST_INT(query.Run(snapshot, rows), 1);
    PL_TEST_STRING(Ids(), "300");

    query.m_sText = "AMBIENCE";
    PL_TEST_INT(query.Run(snapshot, rows), 1);
    PL_TEST_STRING(Ids(), "400");
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Sort keys")
  {
    plAudioProfilerVoiceQuery query;
    query.m_SortKey = plAudioProfilerVoiceQuery::SortKey::Event;
    query.m_bAscending = true;
    query.Run(snapshot, rows);
    PL_TEST_STRING(Ids(), "400 200 100 300");

    query.m_bAscending = false;
    query.Run(snapshot, rows);
    PL_TEST_STRING(Ids(), "300 100 200 400");

    // equal audibility keeps the id order either way
    query.m_SortKey = plAudioProfilerVoiceQuery::SortKey::Audibility;
    query.m_bAscending = true;
    query.Run(snapshot, rows);
    PL_TEST_STRING(Ids(), "200 300 400 100");

    query.m_SortKey = plAudioProfilerVoiceQuery::SortKey::Owner;
    query.Run(snapshot, rows);
    PL_TEST_STRING(Ids(), "200 300 100 400");
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Cap reports the full count")
  {
    plAudioProfilerVoiceQuery query;
    query.m_uiMaxRows = 2;
    PL_TEST_INT(query.Run(snapshot, rows), 4);
    PL_TEST_INT(rows.GetCount(), 2);
    PL_TEST_STRING(Ids(), "300 200");
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "State counts")
  {
    plUInt32 counts[4] = {};
    plAudioProfilerVoiceQuery::CountStates(snapshot, counts);
    PL_TEST_INT(counts[0], 2);
    PL_TEST_INT(counts[1], 1);
    PL_TEST_INT(counts[2], 1);
    PL_TEST_INT(counts[3], 1);
  }
}

PL_CREATE_SIMPLE_TEST(AudioProfilerGate, SnapshotVersion4)
{
  plAudioProfilingSnapshot snapshot;
  snapshot.m_uiRealVoices = 1;
  snapshot.m_uiVirtualVoices = 1;
  snapshot.m_uiMemoryBytes = 1000 + 300;
  snapshot.m_uiEmbeddedSampleBytes = 300;
  snapshot.m_fTime = 4.0;

  plAudioVoiceSnapshotEntry& real = snapshot.m_Voices.ExpandAndGetRef();
  real = MakeRow(0x100, plAudioVoiceSnapshotEntry::Stopping, "Rifle", "Player", "Weapons", 0.8f);
  real.m_bVirtual = false;
  real.m_uiStopReason = plAudioStopReason::StolenQuietest;
  real.m_Parameters.PushBack({"Speed", 0.25f});

  plAudioVoiceSnapshotEntry& virt = snapshot.m_Voices.ExpandAndGetRef();
  virt = MakeRow(0x200, plAudioVoiceSnapshotEntry::Stopping, "Wind", "", "Ambience", 0.1f);
  virt.m_bVirtual = true;
  virt.m_uiStopReason = plAudioStopReason::Requested;

  plAudioEndedVoiceSnapshotEntry& ended = snapshot.m_Ended.ExpandAndGetRef();
  ended.m_Voice = MakeRow(0x300, plAudioVoiceSnapshotEntry::Ended, "Glass", "GlassPane_14", "SFX", 0.4f);
  ended.m_Voice.m_uiStopReason = plAudioStopReason::KilledVirtualBudget;
  ended.m_Voice.m_Parameters.PushBack({"Force", 0.9f});
  ended.m_fEndTime = 3.5;

  plAudioEventMemorySnapshotEntry& event = snapshot.m_Events.ExpandAndGetRef();
  event.m_sName = "Glass";
  event.m_uiSampleBytes = 300;

  const plDynamicArray<plUInt8> data = Serialize(snapshot);

  PL_TEST_BLOCK(plTestBlock::Enabled, "Round trip")
  {
    PL_TEST_INT(data[0], 4);

    plAudioProfilingSnapshot read;
    if (!PL_TEST_RESULT(Deserialize(data, read)))
      return;

    PL_TEST_BOOL(read.m_uiMemoryBytes == 1300);
    PL_TEST_BOOL(read.m_uiEmbeddedSampleBytes == 300);
    if (PL_TEST_INT(read.m_Voices.GetCount(), 2))
    {
      PL_TEST_BOOL(!read.m_Voices[0].m_bVirtual);
      PL_TEST_INT(read.m_Voices[0].m_uiStopReason, plAudioStopReason::StolenQuietest);
      if (PL_TEST_INT(read.m_Voices[0].m_Parameters.GetCount(), 1))
      {
        PL_TEST_STRING(read.m_Voices[0].m_Parameters[0].m_sName, "Speed");
        PL_TEST_FLOAT(read.m_Voices[0].m_Parameters[0].m_fValue, 0.25f, 0.0f);
      }
      PL_TEST_BOOL(read.m_Voices[1].m_bVirtual);
      PL_TEST_INT(read.m_Voices[1].m_uiStopReason, plAudioStopReason::Requested);
    }

    if (PL_TEST_INT(read.m_Ended.GetCount(), 1))
    {
      PL_TEST_INT(read.m_Ended[0].m_Voice.m_uiId, 0x300);
      PL_TEST_INT(read.m_Ended[0].m_Voice.m_uiState, plAudioVoiceSnapshotEntry::Ended);
      PL_TEST_STRING(read.m_Ended[0].m_Voice.m_sEntity, "GlassPane_14");
      PL_TEST_INT(read.m_Ended[0].m_Voice.m_uiStopReason, plAudioStopReason::KilledVirtualBudget);
      PL_TEST_DOUBLE(read.m_Ended[0].m_fEndTime, 3.5, 0.0);
      PL_TEST_INT(read.m_Ended[0].m_Voice.m_Parameters.GetCount(), 1);
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Version 3 data")
  {
    // version 4 appends its fields: per voice the virtual flag, the stop reason and the parameters, then the ended
    // voices and the embedded sample bytes. Without them (and with version byte 3) the data is version 3.
    plAudioProfilingSnapshot v3 = snapshot;
    v3.m_Ended.Clear();
    for (plAudioVoiceSnapshotEntry& voice : v3.m_Voices)
      voice.m_Parameters.Clear();

    // the version 3 writer counted only clip data and stream rings
    v3.m_uiMemoryBytes = 1000;

    plDynamicArray<plUInt8> v3Data = Serialize(v3);
    const plUInt32 uiTail = v3.m_Voices.GetCount() * 3 + sizeof(plUInt32) + sizeof(plUInt64);
    v3Data.SetCount(v3Data.GetCount() - uiTail);
    v3Data[0] = 3;

    plAudioProfilingSnapshot read;
    if (!PL_TEST_RESULT(Deserialize(v3Data, read)))
      return;

    // the memory total is raised by the events' embedded samples, so it means what version 4 means
    PL_TEST_BOOL(read.m_uiEmbeddedSampleBytes == 300);
    PL_TEST_BOOL(read.m_uiMemoryBytes == 1300);
    PL_TEST_BOOL(read.m_Ended.IsEmpty());

    // version 3 does not know whether a stopping voice is virtual
    if (PL_TEST_INT(read.m_Voices.GetCount(), 2))
    {
      PL_TEST_BOOL(!read.m_Voices[1].m_bVirtual);
      PL_TEST_INT(read.m_Voices[1].m_uiStopReason, plAudioStopReason::None);
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Size of a full default budget")
  {
    // 64 real and 512 virtual voices with path-like names and two parameters each, plus 256 ended voices: what a
    // snapshot costs the IPC poll and a capture sample at the default budgets
    plAudioProfilingSnapshot full;
    for (plUInt32 i = 0; i < 64 + 512 + 256; ++i)
    {
      plAudioVoiceSnapshotEntry voice = MakeRow(i, i < 64 ? plAudioVoiceSnapshotEntry::Real : plAudioVoiceSnapshotEntry::Virtual, "Sounds/Weapons/Rifle_Fire_Close.plAudioEventAsset", "Enemy_Soldier_07", "Weapons", 0.5f);
      voice.m_Parameters.PushBack({"Distance", 12.0f});
      voice.m_Parameters.PushBack({"Intensity", 0.5f});

      if (i < 64 + 512)
      {
        full.m_Voices.PushBack(voice);
      }
      else
      {
        plAudioEndedVoiceSnapshotEntry& ended = full.m_Ended.ExpandAndGetRef();
        ended.m_Voice = voice;
        ended.m_Voice.m_uiState = plAudioVoiceSnapshotEntry::Ended;
      }
    }

    const plUInt32 uiBytes = Serialize(full).GetCount();
    plTestFramework::Output(plTestOutput::Message, "Snapshot of 576 voices and 256 ended voices: %u bytes", uiBytes);
    PL_TEST_BOOL(uiBytes < 256 * 1024);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Truncated data fails")
  {
    plDynamicArray<plUInt8> truncated = data;
    truncated.SetCount(truncated.GetCount() - 4);

    plAudioProfilingSnapshot read;
    PL_TEST_BOOL(Deserialize(truncated, read).Failed());
  }
}
