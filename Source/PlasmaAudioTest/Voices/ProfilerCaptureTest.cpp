#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <Foundation/IO/MemoryStream.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilerCapture.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>

namespace
{
  plAudioVoiceSnapshotEntry MakeVoice(plUInt32 uiSerial, plUInt32 uiSlot, plUInt8 uiState, plStringView sEvent, plStringView sEntity)
  {
    plAudioVoiceSnapshotEntry voice;
    voice.m_uiId = (uiSerial << 8) | uiSlot;
    voice.m_sEvent = sEvent;
    voice.m_sEntity = sEntity;
    voice.m_uiState = uiState;
    voice.m_sBus = "SFX";
    voice.m_fAudibility = 0.25f;
    return voice;
  }

  plDynamicArray<plUInt8> Serialize(const plAudioProfilingSnapshot& snapshot)
  {
    plDynamicArray<plUInt8> data;
    plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&data);
    plMemoryStreamWriter writer(&storage);
    snapshot.Write(writer);
    return data;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(AudioProfiling, SnapshotVersion3)
{
  plAudioProfilingSnapshot snapshot;
  snapshot.m_Voices.PushBack(MakeVoice(7, 0, plAudioVoiceSnapshotEntry::Real, "Hum110", "HumEmitter"));
  snapshot.m_Voices.PushBack(MakeVoice(8, 1, plAudioVoiceSnapshotEntry::Virtual, "Tone440", ""));
  snapshot.m_fTime = 12.5;
  snapshot.m_uiStreamUnderruns = 3;
  snapshot.m_uiMemoryBudgetBytes = 64 * 1024 * 1024;

  const plDynamicArray<plUInt8> data = Serialize(snapshot);

  plAudioProfilingSnapshot read;
  plRawMemoryStreamReader reader(data.GetData(), data.GetCount());
  if (!PL_TEST_RESULT(read.Read(reader)))
    return;

  PL_TEST_DOUBLE(read.m_fTime, 12.5, 0.0);
  PL_TEST_INT(read.m_uiStreamUnderruns, 3);
  PL_TEST_INT(read.m_uiMemoryBudgetBytes, 64 * 1024 * 1024);
  if (PL_TEST_INT(read.m_Voices.GetCount(), 2))
  {
    PL_TEST_STRING(read.m_Voices[0].m_sEntity, "HumEmitter");
    PL_TEST_STRING(read.m_Voices[1].m_sEntity, "");
  }
}

PL_CREATE_SIMPLE_TEST(AudioProfiling, Capture)
{
  // five snapshots, 0.1 s apart: instance 1 plays throughout and is virtual in the middle, instance 2 plays twice
  // (two snapshots), the log runs alongside and the streams run dry twice
  plAudioProfilerCapture capture;

  auto Snapshot = [&](plUInt32 uiStep)
  {
    plAudioProfilingSnapshot snapshot;
    snapshot.m_fTime = 100.0 + uiStep * 0.1;
    snapshot.m_uiRealVoices = 1;
    snapshot.m_uiStreamUnderruns = uiStep >= 3 ? 7 : 5;
    snapshot.m_uiMemoryBudgetBytes = 1024;

    const plUInt8 uiState = (uiStep == 2 || uiStep == 3) ? plAudioVoiceSnapshotEntry::Virtual : plAudioVoiceSnapshotEntry::Real;
    snapshot.m_Voices.PushBack(MakeVoice(1, 0, uiState, "Ambience", "Campfire"));
    snapshot.m_Voices.PushBack(MakeVoice(1, 1, plAudioVoiceSnapshotEntry::Virtual, "Ambience", "Campfire"));
    if (uiStep == 1 || uiStep == 2)
      snapshot.m_Voices.PushBack(MakeVoice(2, 0, plAudioVoiceSnapshotEntry::Real, "Footstep", "Player"));

    // the log entries overlap from one snapshot to the next
    snapshot.m_uiEventLogFirstSequence = uiStep;
    for (plUInt32 i = 0; i < 2; ++i)
    {
      plAudioEventLogSnapshotEntry& entry = snapshot.m_EventLog.ExpandAndGetRef();
      entry.m_fTime = 100.0 + (uiStep + i) * 0.1;
      entry.m_uiType = plAudioLogEventType::Start;
      plStringBuilder sName;
      sName.SetFormat("entry {}", uiStep + i);
      entry.m_sName = sName;
    }

    return Serialize(snapshot);
  };

  for (plUInt32 uiStep = 0; uiStep < 5; ++uiStep)
  {
    PL_TEST_RESULT(capture.AddSnapshot(Snapshot(uiStep), 0.0));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Samples")
  {
    PL_TEST_INT(capture.GetSamples().GetCount(), 5);
    PL_TEST_DOUBLE(capture.GetDuration(), 0.4, 0.0001);
    PL_TEST_INT(capture.GetSamples()[0].m_uiUnderruns, 0);
    PL_TEST_INT(capture.GetSamples()[3].m_uiUnderruns, 2);
    PL_TEST_INT(capture.GetSamples()[4].m_uiUnderruns, 0);
    PL_TEST_INT(capture.GetSamples()[2].m_uiBudgetBytes, 1024);
    PL_TEST_INT(capture.FindSample(0.25), 2);
    PL_TEST_INT(capture.FindSample(-1.0), 0);
    PL_TEST_INT(capture.FindSample(9.0), 4);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Instances")
  {
    const auto& instances = capture.GetInstances();
    if (!PL_TEST_INT(instances.GetCount(), 2))
      return;

    // real while any voice is real: real, virtual from 0.2, real again from 0.4
    const plAudioProfilerCapture::Instance& ambience = instances[0];
    PL_TEST_STRING(ambience.m_sEvent, "Ambience");
    PL_TEST_STRING(ambience.m_sEntity, "Campfire");
    PL_TEST_DOUBLE(ambience.m_fStart, 0.0, 0.0001);
    PL_TEST_DOUBLE(ambience.m_fEnd, 0.4, 0.0001);
    PL_TEST_INT(ambience.m_uiVirtualisations, 1);
    if (PL_TEST_INT(ambience.m_Segments.GetCount(), 3))
    {
      PL_TEST_INT(ambience.m_Segments[1].m_uiState, plAudioVoiceSnapshotEntry::Virtual);
      PL_TEST_DOUBLE(ambience.m_Segments[1].m_fStart, 0.2, 0.0001);
      PL_TEST_DOUBLE(ambience.m_Segments[1].m_fEnd, 0.4, 0.0001);
      PL_TEST_INT(ambience.m_Segments[2].m_uiState, plAudioVoiceSnapshotEntry::Real);
    }

    // seen at 0.1 and 0.2, gone at 0.3
    const plAudioProfilerCapture::Instance& footstep = instances[1];
    PL_TEST_STRING(footstep.m_sEntity, "Player");
    PL_TEST_DOUBLE(footstep.m_fStart, 0.1, 0.0001);
    PL_TEST_DOUBLE(footstep.m_fEnd, 0.3, 0.0001);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Log")
  {
    // entries 0 to 5, each once, at their capture time
    const auto& log = capture.GetLog();
    if (PL_TEST_INT(log.GetCount(), 6))
    {
      PL_TEST_STRING(log[5].m_sName, "entry 5");
      PL_TEST_DOUBLE(log[5].m_fTime, 0.5, 0.0001);
    }
    PL_TEST_INT(capture.GetLostLogLines(), 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Save and load")
  {
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    PL_TEST_RESULT(capture.Save(writer));

    plAudioProfilerCapture loaded;
    plMemoryStreamReader reader(&storage);
    if (!PL_TEST_RESULT(loaded.Load(reader)))
      return;

    PL_TEST_INT(loaded.GetSamples().GetCount(), 5);
    PL_TEST_INT(loaded.GetInstances().GetCount(), 2);
    PL_TEST_INT(loaded.GetLog().GetCount(), 6);

    plAudioProfilingSnapshot snapshot;
    if (PL_TEST_RESULT(loaded.GetSnapshot(2, snapshot)))
      PL_TEST_INT(snapshot.m_Voices.GetCount(), 3);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Not a capture")
  {
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    writer << plUInt64(12345);

    plAudioProfilerCapture loaded;
    plMemoryStreamReader reader(&storage);
    PL_TEST_BOOL(loaded.Load(reader).Failed());
    PL_TEST_BOOL(loaded.IsEmpty());
  }
}
