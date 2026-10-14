#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <Foundation/IO/MemoryStream.h>
#include <PlasmaAudioPlugin/Events/AudioVoiceManager.h>
#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioPlugin/Profiling/AudioEventLog.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

PL_CREATE_SIMPLE_TEST_GROUP(AudioProfiling);

namespace
{
  struct SnapshotBudgetScope
  {
    SnapshotBudgetScope(plInt32 iReal, plInt32 iVirtual) { plAudioVoiceManager::SetBudgets(iReal, iVirtual); }
    ~SnapshotBudgetScope() { plAudioVoiceManager::SetBudgets(-1, -1); }
  };

  struct SnapshotEmitter
  {
    plPlasmaAudioEntityData m_Entity;
    plPlasmaAudioEventData m_Event;
  };

  plUInt32 CountState(const plAudioProfilingSnapshot& snapshot, plUInt8 uiState)
  {
    plUInt32 uiCount = 0;
    for (const plAudioVoiceSnapshotEntry& voice : snapshot.m_Voices)
    {
      if (voice.m_uiState == uiState)
        ++uiCount;
    }
    return uiCount;
  }

  plUInt32 CountLogType(const plDynamicArray<plAudioEventLogSnapshotEntry>& entries, plAudioLogEventType::Enum type, plStringView sName = plStringView())
  {
    plUInt32 uiCount = 0;
    for (const plAudioEventLogSnapshotEntry& entry : entries)
    {
      if (entry.m_uiType == type && (sName.IsEmpty() || entry.m_sName == sName))
        ++uiCount;
    }
    return uiCount;
  }

  void WriteSnapshot(const plAudioProfilingSnapshot& snapshot, plDataBuffer& out_buffer)
  {
    out_buffer.Clear();
    plMemoryStreamContainerWrapperStorage<plDataBuffer> storage(&out_buffer);
    plMemoryStreamWriter writer(&storage);
    snapshot.Write(writer);
  }
} // namespace

PL_CREATE_SIMPLE_TEST(AudioProfiling, SnapshotRoundTrip)
{
  plAudioProfilingSnapshot snapshot;
  snapshot.m_uiRealVoices = 3;
  snapshot.m_uiVirtualVoices = 7;
  snapshot.m_uiMaxReal = 64;
  snapshot.m_uiMaxVirtual = 512;
  snapshot.m_uiInstances = 9;
  snapshot.m_uiStreams = 2;
  snapshot.m_fDspLoad = 0.125f;
  snapshot.m_fUpdateMs = 0.75f;
  snapshot.m_uiMemoryBytes = 0x123456789ull;

  for (plUInt32 i = 0; i < 3; ++i)
  {
    plAudioVoiceSnapshotEntry& voice = snapshot.m_Voices.ExpandAndGetRef();
    voice.m_uiId = (i + 1) << 8 | i;
    plStringBuilder sEvent;
    sEvent.SetFormat("Sounds/Event{}.plAudioEvent", i);
    voice.m_sEvent = sEvent;
    voice.m_uiEntityId = 1000 + i;
    voice.m_uiState = static_cast<plUInt8>(i);
    voice.m_uiPriority = static_cast<plUInt8>(100 + i);
    voice.m_uiCodec = (i == 2) ? plAudioVoiceSnapshotEntry::Embedded : static_cast<plUInt8>(i);
    voice.m_uiLoadMode = static_cast<plUInt8>(i);
    voice.m_fAudibility = 0.5f + i;
    voice.m_fDistance = 10.0f * i;
    voice.m_fOcclusion = 0.25f * i;
    voice.m_fVolume = 0.9f;
    voice.m_fAge = 1.5f + i;
    voice.m_sBus = (i == 0) ? "Master" : "SFX";
  }

  plDataBuffer buffer;
  {
    plMemoryStreamContainerWrapperStorage<plDataBuffer> storage(&buffer);
    plMemoryStreamWriter writer(&storage);
    snapshot.Write(writer);
  }

  plAudioProfilingSnapshot read;
  {
    plRawMemoryStreamReader reader(buffer);
    PL_TEST_RESULT(read.Read(reader));
  }

  PL_TEST_INT(read.m_uiRealVoices, 3);
  PL_TEST_INT(read.m_uiVirtualVoices, 7);
  PL_TEST_INT(read.m_uiMaxReal, 64);
  PL_TEST_INT(read.m_uiMaxVirtual, 512);
  PL_TEST_INT(read.m_uiInstances, 9);
  PL_TEST_INT(read.m_uiStreams, 2);
  PL_TEST_FLOAT(read.m_fDspLoad, 0.125f, 0.0f);
  PL_TEST_FLOAT(read.m_fUpdateMs, 0.75f, 0.0f);
  PL_TEST_BOOL(read.m_uiMemoryBytes == 0x123456789ull);

  if (PL_TEST_INT(read.m_Voices.GetCount(), 3))
  {
    for (plUInt32 i = 0; i < 3; ++i)
    {
      const plAudioVoiceSnapshotEntry& a = snapshot.m_Voices[i];
      const plAudioVoiceSnapshotEntry& b = read.m_Voices[i];
      PL_TEST_INT(b.m_uiId, a.m_uiId);
      PL_TEST_STRING(b.m_sEvent, a.m_sEvent);
      PL_TEST_BOOL(b.m_uiEntityId == a.m_uiEntityId);
      PL_TEST_INT(b.m_uiState, a.m_uiState);
      PL_TEST_INT(b.m_uiPriority, a.m_uiPriority);
      PL_TEST_INT(b.m_uiCodec, a.m_uiCodec);
      PL_TEST_INT(b.m_uiLoadMode, a.m_uiLoadMode);
      PL_TEST_FLOAT(b.m_fAudibility, a.m_fAudibility, 0.0f);
      PL_TEST_FLOAT(b.m_fDistance, a.m_fDistance, 0.0f);
      PL_TEST_FLOAT(b.m_fOcclusion, a.m_fOcclusion, 0.0f);
      PL_TEST_FLOAT(b.m_fVolume, a.m_fVolume, 0.0f);
      PL_TEST_FLOAT(b.m_fAge, a.m_fAge, 0.0f);
      PL_TEST_STRING(b.m_sBus, a.m_sBus);
    }
  }

  // truncated data and a newer version are rejected
  {
    plRawMemoryStreamReader reader(buffer.GetData(), buffer.GetCount() - 1);
    PL_TEST_BOOL(read.Read(reader).Failed());
  }

  {
    plDataBuffer newer = buffer;
    newer[0] = plAudioProfilingSnapshot::kSnapshotVersion + 1;
    plRawMemoryStreamReader reader(newer);
    PL_TEST_BOOL(read.Read(reader).Failed());
  }
}

PL_CREATE_SIMPLE_TEST(AudioProfiling, FillSnapshot)
{
  SnapshotBudgetScope budgets(2, 512);

  constexpr plUInt32 uiEmitters = 5;

  plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(4800, 0.25f));
  res.m_Desc.m_uiMaxInstances = 0;
  res.m_Desc.m_eStealing = plAudioEventStealingMode::None;
  res.m_Desc.m_uiPriority = 77;
  res.m_Desc.m_bSpatialize = true;
  res.m_Desc.m_fMinDistance = 1.0f;
  res.m_Desc.m_fMaxDistance = 100.0f;
  res.m_Desc.m_fRolloff = 1.0f;
  res.m_Desc.m_Instruments[0].m_bLoop = true;
  res.m_Desc.m_StopFadeOut = plTime::MakeFromMilliseconds(100);
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:ProfilingSnapshot", std::move(res));

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plPlasmaAudioEventManager manager;

  plDynamicArray<plUniquePtr<SnapshotEmitter>> emitters;
  for (plUInt32 i = 0; i < uiEmitters; ++i)
  {
    plUniquePtr<SnapshotEmitter> pEmitter = PL_DEFAULT_NEW(SnapshotEmitter);
    pEmitter->m_Entity.m_uiEntityId = 500 + i;
    pEmitter->m_Entity.m_LastTransform.m_vPosition = plVec3(2.0f + 10.0f * i, 0.0f, 0.0f);
    pEmitter->m_Event.m_bLooping = true;
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:ProfilingSnapshot", &pEmitter->m_Entity, &pEmitter->m_Event, engine.GetBusGraph(), engine.GetEngine()));
    emitters.PushBack(std::move(pEmitter));
  }

  engine.RenderBlocks(10, &manager, nullptr);

  plAudioProfilingSnapshot snapshot;
  manager.FillSnapshot(snapshot);

  PL_TEST_INT(snapshot.m_uiInstances, uiEmitters);
  PL_TEST_INT(snapshot.m_uiMaxReal, 2);
  PL_TEST_INT(snapshot.m_uiMaxVirtual, 512);
  PL_TEST_INT(snapshot.m_uiRealVoices, manager.GetRealVoiceCount());
  PL_TEST_INT(snapshot.m_uiVirtualVoices, manager.GetVirtualVoiceCount());
  PL_TEST_INT(snapshot.m_uiRealVoices, 2);
  PL_TEST_INT(snapshot.m_uiVirtualVoices, 3);
  PL_TEST_INT(snapshot.m_Voices.GetCount(), 5);
  PL_TEST_INT(CountState(snapshot, plAudioVoiceSnapshotEntry::Real), 2);
  PL_TEST_INT(CountState(snapshot, plAudioVoiceSnapshotEntry::Virtual), 3);
  PL_TEST_INT(CountState(snapshot, plAudioVoiceSnapshotEntry::Stopping), 0);

  plHybridArray<plUInt32, 8> ids;
  for (const plAudioVoiceSnapshotEntry& voice : snapshot.m_Voices)
  {
    PL_TEST_STRING(voice.m_sEvent, "PlasmaAudioTest:ProfilingSnapshot");
    PL_TEST_INT(voice.m_uiPriority, 77);
    PL_TEST_INT(voice.m_uiCodec, plAudioVoiceSnapshotEntry::Embedded);
    PL_TEST_BOOL(!voice.m_sBus.IsEmpty());
    PL_TEST_BOOL(voice.m_uiEntityId >= 500 && voice.m_uiEntityId < 500 + uiEmitters);
    PL_TEST_BOOL(voice.m_fAge > 0.0f);

    // the two closest emitters are the real ones
    const float fExpectedDistance = 2.0f + 10.0f * static_cast<float>(voice.m_uiEntityId - 500);
    PL_TEST_FLOAT(voice.m_fDistance, fExpectedDistance, 0.001f);
    PL_TEST_BOOL((voice.m_uiState == plAudioVoiceSnapshotEntry::Real) == (voice.m_uiEntityId < 502));

    PL_TEST_BOOL(!ids.Contains(voice.m_uiId));
    ids.PushBack(voice.m_uiId);
  }

  // an instance that fades out reports its real voice as stopping; it still counts as real
  PL_TEST_RESULT(manager.StopEvent(&emitters[0]->m_Event, false));
  manager.FillSnapshot(snapshot);
  PL_TEST_INT(CountState(snapshot, plAudioVoiceSnapshotEntry::Stopping), 1);
  PL_TEST_INT(CountState(snapshot, plAudioVoiceSnapshotEntry::Real), 1);
  PL_TEST_INT(CountState(snapshot, plAudioVoiceSnapshotEntry::Virtual), 3);
  PL_TEST_INT(snapshot.m_uiRealVoices, 2);

  // the snapshot survives serialization
  plDataBuffer buffer;
  {
    plMemoryStreamContainerWrapperStorage<plDataBuffer> storage(&buffer);
    plMemoryStreamWriter writer(&storage);
    snapshot.Write(writer);
  }

  plAudioProfilingSnapshot read;
  plRawMemoryStreamReader reader(buffer);
  PL_TEST_RESULT(read.Read(reader));
  PL_TEST_INT(read.m_Voices.GetCount(), snapshot.m_Voices.GetCount());
  PL_TEST_INT(read.m_uiRealVoices, snapshot.m_uiRealVoices);

  manager.StopAll(true);

  for (auto& pEmitter : emitters)
  {
    manager.DetachEventData(&pEmitter->m_Event);
  }
}

PL_CREATE_SIMPLE_TEST(AudioProfiling, SnapshotRoundTripV2)
{
  plAudioProfilingSnapshot snapshot;
  snapshot.m_uiRealVoices = 1;

  snapshot.m_sMixer = "Mixers/Main.plAudioMixer";
  for (plUInt32 i = 0; i < 3; ++i)
  {
    plAudioBusSnapshotEntry& bus = snapshot.m_Buses.ExpandAndGetRef();
    bus.m_sName = (i == 0) ? "Master" : (i == 1 ? "SFX" : "Reverb");
    bus.m_uiParent = (i == 0) ? plAudioBusSnapshotEntry::NoParent : 0;
    bus.m_bIsAux = (i == 2);
    bus.m_fGainDb = -3.0f * i;
    bus.m_fPeak = 0.5f + 0.1f * i;
    bus.m_fRms = 0.25f + 0.05f * i;
  }

  {
    plAudioActiveSnapshotEntry& active = snapshot.m_ActiveSnapshots.ExpandAndGetRef();
    active.m_sName = "Combat";
    active.m_fIntensity = 0.75f;
  }

  {
    plAudioClipMemorySnapshotEntry& clip = snapshot.m_Clips.ExpandAndGetRef();
    clip.m_sName = "Sounds/Music.plBinAudioClip";
    clip.m_uiLoadMode = plAudioClipLoadMode::Streamed;
    clip.m_uiCodec = plAudioClipCodec::Opus;
    clip.m_uiResidentBytes = 0x100000001ull;
    clip.m_uiStreams = 2;
  }

  {
    plAudioEventMemorySnapshotEntry& event = snapshot.m_Events.ExpandAndGetRef();
    event.m_sName = "Sounds/Shot.plAudioEvent";
    event.m_uiSampleBytes = 12345;
  }

  {
    plAudioBankMemorySnapshotEntry& bank = snapshot.m_Banks.ExpandAndGetRef();
    bank.m_sName = "Banks/Core.plAudioBank";
    bank.m_uiEvents = 7;
    bank.m_uiClips = 11;
    bank.m_uiState = plAudioBankMemorySnapshotEntry::Missing;
  }

  snapshot.m_uiClipResidentBytes = 1000;
  snapshot.m_uiStreamRingBytes = 2000;
  snapshot.m_uiPooledStreams = 3;

  snapshot.m_uiEventLogFirstSequence = 41;
  for (plUInt32 i = 0; i < 2; ++i)
  {
    plAudioEventLogSnapshotEntry& entry = snapshot.m_EventLog.ExpandAndGetRef();
    entry.m_fTime = 1.5 + i;
    entry.m_uiType = (i == 0) ? plAudioLogEventType::Start : plAudioLogEventType::ClipMissing;
    entry.m_sName = "Sounds/Shot.plAudioEvent";
    entry.m_sDetail = (i == 0) ? "entity #5" : "";
  }

  plDataBuffer buffer;
  WriteSnapshot(snapshot, buffer);
  PL_TEST_INT(buffer[0], 2);

  plAudioProfilingSnapshot read;
  {
    plRawMemoryStreamReader reader(buffer);
    PL_TEST_RESULT(read.Read(reader));
  }

  PL_TEST_INT(read.m_uiRealVoices, 1);
  PL_TEST_STRING(read.m_sMixer, snapshot.m_sMixer);

  if (PL_TEST_INT(read.m_Buses.GetCount(), 3))
  {
    for (plUInt32 i = 0; i < 3; ++i)
    {
      PL_TEST_STRING(read.m_Buses[i].m_sName, snapshot.m_Buses[i].m_sName);
      PL_TEST_INT(read.m_Buses[i].m_uiParent, snapshot.m_Buses[i].m_uiParent);
      PL_TEST_BOOL(read.m_Buses[i].m_bIsAux == snapshot.m_Buses[i].m_bIsAux);
      PL_TEST_FLOAT(read.m_Buses[i].m_fGainDb, snapshot.m_Buses[i].m_fGainDb, 0.0f);
      PL_TEST_FLOAT(read.m_Buses[i].m_fPeak, snapshot.m_Buses[i].m_fPeak, 0.0f);
      PL_TEST_FLOAT(read.m_Buses[i].m_fRms, snapshot.m_Buses[i].m_fRms, 0.0f);
    }
  }

  if (PL_TEST_INT(read.m_ActiveSnapshots.GetCount(), 1))
  {
    PL_TEST_STRING(read.m_ActiveSnapshots[0].m_sName, "Combat");
    PL_TEST_FLOAT(read.m_ActiveSnapshots[0].m_fIntensity, 0.75f, 0.0f);
  }

  if (PL_TEST_INT(read.m_Clips.GetCount(), 1))
  {
    PL_TEST_STRING(read.m_Clips[0].m_sName, "Sounds/Music.plBinAudioClip");
    PL_TEST_INT(read.m_Clips[0].m_uiLoadMode, plAudioClipLoadMode::Streamed);
    PL_TEST_INT(read.m_Clips[0].m_uiCodec, plAudioClipCodec::Opus);
    PL_TEST_BOOL(read.m_Clips[0].m_uiResidentBytes == 0x100000001ull);
    PL_TEST_INT(read.m_Clips[0].m_uiStreams, 2);
  }

  if (PL_TEST_INT(read.m_Events.GetCount(), 1))
  {
    PL_TEST_STRING(read.m_Events[0].m_sName, "Sounds/Shot.plAudioEvent");
    PL_TEST_BOOL(read.m_Events[0].m_uiSampleBytes == 12345);
  }

  if (PL_TEST_INT(read.m_Banks.GetCount(), 1))
  {
    PL_TEST_STRING(read.m_Banks[0].m_sName, "Banks/Core.plAudioBank");
    PL_TEST_INT(read.m_Banks[0].m_uiEvents, 7);
    PL_TEST_INT(read.m_Banks[0].m_uiClips, 11);
    PL_TEST_INT(read.m_Banks[0].m_uiState, plAudioBankMemorySnapshotEntry::Missing);
  }

  PL_TEST_BOOL(read.m_uiClipResidentBytes == 1000);
  PL_TEST_BOOL(read.m_uiStreamRingBytes == 2000);
  PL_TEST_INT(read.m_uiPooledStreams, 3);

  PL_TEST_BOOL(read.m_uiEventLogFirstSequence == 41);
  if (PL_TEST_INT(read.m_EventLog.GetCount(), 2))
  {
    for (plUInt32 i = 0; i < 2; ++i)
    {
      PL_TEST_DOUBLE(read.m_EventLog[i].m_fTime, snapshot.m_EventLog[i].m_fTime, 0.0);
      PL_TEST_INT(read.m_EventLog[i].m_uiType, snapshot.m_EventLog[i].m_uiType);
      PL_TEST_STRING(read.m_EventLog[i].m_sName, snapshot.m_EventLog[i].m_sName);
      PL_TEST_STRING(read.m_EventLog[i].m_sDetail, snapshot.m_EventLog[i].m_sDetail);
    }
  }

  // every truncation of the new sections is rejected
  for (plUInt32 uiCut : {1u, 5u, 20u})
  {
    plRawMemoryStreamReader reader(buffer.GetData(), buffer.GetCount() - uiCut);
    PL_TEST_BOOL(read.Read(reader).Failed());
  }
}

PL_CREATE_SIMPLE_TEST(AudioProfiling, SnapshotReadsVersion1)
{
  // the layout written by version 1: header fields, then the voices
  plDataBuffer buffer;
  {
    plMemoryStreamContainerWrapperStorage<plDataBuffer> storage(&buffer);
    plMemoryStreamWriter writer(&storage);

    auto Write = [&](const auto& value)
    { writer.WriteBytes(&value, sizeof(value)).AssertSuccess(); };

    Write(static_cast<plUInt8>(1));
    Write(static_cast<plUInt32>(4));  // real
    Write(static_cast<plUInt32>(5));  // virtual
    Write(static_cast<plUInt32>(64)); // max real
    Write(static_cast<plUInt32>(512)); // max virtual
    Write(static_cast<plUInt32>(6));  // instances
    Write(static_cast<plUInt32>(1));  // streams
    Write(0.5f);                      // dsp load
    Write(0.25f);                     // update ms
    Write(static_cast<plUInt64>(777)); // memory

    Write(static_cast<plUInt32>(1)); // one voice
    Write(static_cast<plUInt32>(0x102));
    writer.WriteString("Sounds/Old.plAudioEvent").AssertSuccess();
    Write(static_cast<plUInt64>(9));
    Write(static_cast<plUInt8>(plAudioVoiceSnapshotEntry::Virtual));
    Write(static_cast<plUInt8>(50));
    Write(static_cast<plUInt8>(plAudioVoiceSnapshotEntry::Embedded));
    Write(static_cast<plUInt8>(plAudioVoiceSnapshotEntry::Embedded));
    Write(0.1f);
    Write(12.0f);
    Write(0.0f);
    Write(1.0f);
    Write(3.0f);
    writer.WriteString("SFX").AssertSuccess();
  }

  plAudioProfilingSnapshot read;
  read.m_Buses.ExpandAndGetRef().m_sName = "stale";
  read.m_EventLog.ExpandAndGetRef().m_sName = "stale";

  plRawMemoryStreamReader reader(buffer);
  PL_TEST_RESULT(read.Read(reader));

  PL_TEST_INT(read.m_uiRealVoices, 4);
  PL_TEST_INT(read.m_uiVirtualVoices, 5);
  PL_TEST_INT(read.m_uiInstances, 6);
  PL_TEST_BOOL(read.m_uiMemoryBytes == 777);
  if (PL_TEST_INT(read.m_Voices.GetCount(), 1))
  {
    PL_TEST_STRING(read.m_Voices[0].m_sEvent, "Sounds/Old.plAudioEvent");
    PL_TEST_INT(read.m_Voices[0].m_uiState, plAudioVoiceSnapshotEntry::Virtual);
    PL_TEST_STRING(read.m_Voices[0].m_sBus, "SFX");
  }

  // the version 2 sections are empty
  PL_TEST_BOOL(read.m_sMixer.IsEmpty());
  PL_TEST_BOOL(read.m_Buses.IsEmpty());
  PL_TEST_BOOL(read.m_ActiveSnapshots.IsEmpty());
  PL_TEST_BOOL(read.m_Clips.IsEmpty());
  PL_TEST_BOOL(read.m_Events.IsEmpty());
  PL_TEST_BOOL(read.m_Banks.IsEmpty());
  PL_TEST_BOOL(read.m_EventLog.IsEmpty());
  PL_TEST_BOOL(read.m_uiEventLogFirstSequence == 0);
}

PL_CREATE_SIMPLE_TEST(AudioProfiling, EventLogCapture)
{
  SnapshotBudgetScope budgets(1, 512);

  {
    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(4800, 0.25f));
    res.m_Desc.m_uiMaxInstances = 2;
    res.m_Desc.m_eStealing = plAudioEventStealingMode::Oldest;
    res.m_Desc.m_bSpatialize = true;
    res.m_Desc.m_fMinDistance = 1.0f;
    res.m_Desc.m_fMaxDistance = 100.0f;
    res.m_Desc.m_fRolloff = 1.0f;
    res.m_Desc.m_Instruments[0].m_bLoop = true;
    res.m_Desc.m_StopFadeOut = plTime::MakeZero();
    plAudioTestUtils::CreateEventResource("PlasmaAudioTest:LogSteal", std::move(res));
  }

  {
    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(4800, 0.25f));
    res.m_Desc.m_uiMaxInstances = 8;
    res.m_Desc.m_eStealing = plAudioEventStealingMode::None;
    res.m_Desc.m_Cooldown = plTime::MakeFromMilliseconds(50);
    res.m_Desc.m_StopFadeOut = plTime::MakeZero();
    plAudioTestUtils::CreateEventResource("PlasmaAudioTest:LogCooldown", std::move(res));
  }

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plPlasmaAudioEventManager manager;
  const plUInt64 uiFirstSequence = manager.GetEventLog().GetNextSequence();

  SnapshotEmitter emitters[5];
  for (SnapshotEmitter& emitter : emitters)
  {
    emitter.m_Event.m_bLooping = true;
  }

  // two emitters fill the voice budget of one: the far one starts virtual
  emitters[0].m_Entity.m_uiEntityId = 1;
  emitters[0].m_Entity.m_LastTransform.m_vPosition = plVec3(2.0f, 0.0f, 0.0f);
  emitters[1].m_Entity.m_uiEntityId = 2;
  emitters[1].m_Entity.m_LastTransform.m_vPosition = plVec3(20.0f, 0.0f, 0.0f);

  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:LogSteal", &emitters[0].m_Entity, &emitters[0].m_Event, engine.GetBusGraph(), engine.GetEngine()));
  engine.RenderBlocks(1, &manager, nullptr);
  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:LogSteal", &emitters[1].m_Entity, &emitters[1].m_Event, engine.GetBusGraph(), engine.GetEngine()));
  engine.RenderBlocks(1, &manager, nullptr);

  // the real voice moves beyond the max distance: it is virtualised and the other one takes the budget
  emitters[0].m_Entity.m_LastTransform.m_vPosition = plVec3(500.0f, 0.0f, 0.0f);
  engine.RenderBlocks(50, &manager, nullptr);

  // a third instance of the group steals the oldest one
  emitters[2].m_Entity.m_uiEntityId = 3;
  emitters[2].m_Entity.m_LastTransform.m_vPosition = plVec3(5.0f, 0.0f, 0.0f);
  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:LogSteal", &emitters[2].m_Entity, &emitters[2].m_Event, engine.GetBusGraph(), engine.GetEngine()));
  engine.RenderBlocks(1, &manager, nullptr);

  // a second start inside the cooldown is dropped
  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:LogCooldown", &emitters[3].m_Entity, &emitters[3].m_Event, engine.GetBusGraph(), engine.GetEngine()));
  engine.RenderBlocks(1, &manager, nullptr);
  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:LogCooldown", &emitters[4].m_Entity, &emitters[4].m_Event, engine.GetBusGraph(), engine.GetEngine()));
  PL_TEST_BOOL(!manager.IsEventPlaying(&emitters[4].m_Event));

  plDynamicArray<plAudioEventLogSnapshotEntry> entries;
  PL_TEST_BOOL(manager.GetEventLog().Collect(uiFirstSequence, entries) == uiFirstSequence);

  PL_TEST_INT(CountLogType(entries, plAudioLogEventType::Start, "PlasmaAudioTest:LogSteal"), 3);
  PL_TEST_INT(CountLogType(entries, plAudioLogEventType::Start, "PlasmaAudioTest:LogCooldown"), 1);
  PL_TEST_BOOL(CountLogType(entries, plAudioLogEventType::CulledAtStart, "PlasmaAudioTest:LogSteal") >= 1);
  PL_TEST_BOOL(CountLogType(entries, plAudioLogEventType::Virtualised, "PlasmaAudioTest:LogSteal") >= 1);
  PL_TEST_BOOL(CountLogType(entries, plAudioLogEventType::Realised, "PlasmaAudioTest:LogSteal") >= 1);
  PL_TEST_INT(CountLogType(entries, plAudioLogEventType::Stolen, "PlasmaAudioTest:LogSteal"), 1);
  PL_TEST_INT(CountLogType(entries, plAudioLogEventType::CooldownRejected, "PlasmaAudioTest:LogCooldown"), 1);

  // entries are in the order they happened, with non-decreasing times
  for (plUInt32 i = 1; i < entries.GetCount(); ++i)
  {
    PL_TEST_BOOL(entries[i].m_fTime >= entries[i - 1].m_fTime);
  }

  plUInt32 uiFirstStart = plInvalidIndex;
  plUInt32 uiFirstStolen = plInvalidIndex;
  for (plUInt32 i = 0; i < entries.GetCount(); ++i)
  {
    if (uiFirstStart == plInvalidIndex && entries[i].m_uiType == plAudioLogEventType::Start)
      uiFirstStart = i;
    if (uiFirstStolen == plInvalidIndex && entries[i].m_uiType == plAudioLogEventType::Stolen)
      uiFirstStolen = i;
  }
  PL_TEST_BOOL(uiFirstStart < uiFirstStolen);
  PL_TEST_STRING(entries[uiFirstStart].m_sDetail, "entity #1");

  // an immediate stop of everything is one entry
  const plUInt64 uiBeforeStop = manager.GetEventLog().GetNextSequence();
  manager.StopAll(true);
  PL_TEST_BOOL(manager.GetEventLog().Collect(uiBeforeStop, entries) == uiBeforeStop);
  PL_TEST_INT(entries.GetCount(), 1);
  PL_TEST_INT(CountLogType(entries, plAudioLogEventType::Stop, "<all>"), 1);

  for (SnapshotEmitter& emitter : emitters)
  {
    manager.DetachEventData(&emitter.m_Event);
  }
}

PL_CREATE_SIMPLE_TEST(AudioProfiling, EventLogOverflow)
{
  plAudioEventLog log;
  constexpr plUInt32 uiAdded = plAudioEventLog::Capacity + 44;

  for (plUInt32 i = 0; i < uiAdded; ++i)
  {
    plStringBuilder sName;
    sName.SetFormat("Event{}", i);
    log.Add(i % 2 == 0 ? plAudioLogEventType::Start : plAudioLogEventType::Finished, sName, "detail");
  }

  PL_TEST_BOOL(log.GetNextSequence() == uiAdded);

  // the oldest 44 entries were overwritten: collecting from 0 starts at 44, and the gap is visible
  plDynamicArray<plAudioEventLogSnapshotEntry> entries;
  const plUInt64 uiFirst = log.Collect(0, entries);
  PL_TEST_BOOL(uiFirst == 44);
  if (PL_TEST_INT(entries.GetCount(), plAudioEventLog::Capacity))
  {
    PL_TEST_STRING(entries[0].m_sName, "Event44");
    PL_TEST_INT(entries[0].m_uiType, plAudioLogEventType::Start);
    PL_TEST_STRING(entries.PeekBack().m_sName, "Event299");
    PL_TEST_INT(entries.PeekBack().m_uiType, plAudioLogEventType::Finished);
    PL_TEST_STRING(entries.PeekBack().m_sDetail, "detail");
  }

  // collecting from a sequence number inside the ring returns only the newer entries
  PL_TEST_BOOL(log.Collect(uiAdded - 3, entries) == uiAdded - 3);
  if (PL_TEST_INT(entries.GetCount(), 3))
  {
    PL_TEST_STRING(entries[0].m_sName, "Event297");
  }

  // nothing new
  PL_TEST_BOOL(log.Collect(uiAdded, entries) == uiAdded);
  PL_TEST_INT(entries.GetCount(), 0);

  // long names keep their end
  plStringBuilder sLong;
  for (plUInt32 i = 0; i < 20; ++i)
  {
    sLong.Append("Folder/");
  }
  sLong.Append("Sound.plAudioEvent");
  log.Add(plAudioLogEventType::Start, sLong);
  log.Collect(uiAdded, entries);
  if (PL_TEST_INT(entries.GetCount(), 1))
  {
    PL_TEST_INT(entries[0].m_sName.GetElementCount(), plAudioEventLog::NameBytes - 1);
    PL_TEST_BOOL(entries[0].m_sName.EndsWith("/Sound.plAudioEvent"));
  }
}
