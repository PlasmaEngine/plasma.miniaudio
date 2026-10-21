#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/IO/CompressedStreamZstd.h>
#include <Foundation/IO/MemoryStream.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilerCapture.h>

namespace
{
  constexpr char s_szMagic[8] = {'P', 'L', 'A', 'U', 'D', 'C', 'A', 'P'};
  constexpr plUInt8 s_uiFileVersion = 1;

  /// The state of an instance from its voices: real while any voice is real, stopping while it fades out.
  plUInt8 CombineState(plUInt8 uiState, plUInt8 uiVoiceState)
  {
    if (uiState == plAudioVoiceSnapshotEntry::Stopping || uiVoiceState == plAudioVoiceSnapshotEntry::Stopping)
      return plAudioVoiceSnapshotEntry::Stopping;
    if (uiState == plAudioVoiceSnapshotEntry::Real || uiVoiceState == plAudioVoiceSnapshotEntry::Real)
      return plAudioVoiceSnapshotEntry::Real;
    return plAudioVoiceSnapshotEntry::Virtual;
  }

  plResult WriteBody(plStreamWriter& inout_stream, plArrayPtr<const double> receivedTimes, plArrayPtr<const plDynamicArray<plUInt8>* const> data)
  {
    inout_stream << data.GetCount();
    for (plUInt32 i = 0; i < data.GetCount(); ++i)
    {
      inout_stream << receivedTimes[i];
      inout_stream << data[i]->GetCount();
      PL_SUCCEED_OR_RETURN(inout_stream.WriteBytes(data[i]->GetData(), data[i]->GetCount()));
    }

    return PL_SUCCESS;
  }
} // namespace

void plAudioProfilerCapture::Clear()
{
  m_Samples.Clear();
  m_Snapshots.Clear();
  m_Instances.Clear();
  m_Log.Clear();
  m_OpenInstances.Clear();
  m_fOrigin = 0.0;
  m_fReceivedOrigin = 0.0;
  m_uiLastUnderruns = 0;
  m_uiNextLogSequence = 0;
  m_bLogSequenceKnown = false;
  m_uiLostLogLines = 0;
}

double plAudioProfilerCapture::GetDuration() const
{
  return m_Samples.IsEmpty() ? 0.0 : m_Samples.PeekBack().m_fTime - m_Samples[0].m_fTime;
}

plResult plAudioProfilerCapture::AddSnapshot(plArrayPtr<const plUInt8> data, double fReceivedTime)
{
  if (IsFull())
    return PL_FAILURE;

  plAudioProfilingSnapshot snapshot;
  {
    plRawMemoryStreamReader reader(data.GetPtr(), data.GetCount());
    PL_SUCCEED_OR_RETURN(snapshot.Read(reader));
  }

  // the first snapshot sets the origin; snapshots without a time of their own count by when they arrived
  const bool bOwnTime = snapshot.m_fTime > 0.0;
  if (m_Samples.IsEmpty())
  {
    m_fOrigin = snapshot.m_fTime;
    m_fReceivedOrigin = fReceivedTime;
    m_uiLastUnderruns = snapshot.m_uiStreamUnderruns;
  }

  double fTime = bOwnTime ? snapshot.m_fTime - m_fOrigin : fReceivedTime - m_fReceivedOrigin;

  // a restarted middleware (a new engine process) starts its clock, log and counters over; the capture goes on from
  // where it was
  const double fLastTime = m_Samples.IsEmpty() ? 0.0 : m_Samples.PeekBack().m_fTime;
  if (bOwnTime && fTime < fLastTime - 0.5)
  {
    m_fOrigin = snapshot.m_fTime - fLastTime;
    fTime = fLastTime;
    m_bLogSequenceKnown = false;
    m_uiNextLogSequence = 0;
    m_uiLastUnderruns = 0;
  }

  const double fSampleTime = plMath::Max(fTime, fLastTime);

  Sample& sample = m_Samples.ExpandAndGetRef();
  sample.m_fTime = fSampleTime;
  sample.m_uiReal = snapshot.m_uiRealVoices;
  sample.m_uiVirtual = snapshot.m_uiVirtualVoices;
  sample.m_uiMaxReal = snapshot.m_uiMaxReal;
  sample.m_uiMaxVirtual = snapshot.m_uiMaxVirtual;
  sample.m_fDspLoad = snapshot.m_fDspLoad;
  sample.m_uiStreams = snapshot.m_uiStreams;
  sample.m_uiMemoryBytes = snapshot.m_uiMemoryBytes;
  sample.m_uiBudgetBytes = snapshot.m_uiMemoryBudgetBytes;
  sample.m_uiUnderruns = snapshot.m_uiStreamUnderruns > m_uiLastUnderruns ? static_cast<plUInt32>(snapshot.m_uiStreamUnderruns - m_uiLastUnderruns) : 0;
  m_uiLastUnderruns = snapshot.m_uiStreamUnderruns;

  Stored& stored = m_Snapshots.ExpandAndGetRef();
  stored.m_fReceivedTime = fReceivedTime;
  stored.m_Data = data;

  AddInstances(snapshot, fSampleTime);
  AddLogLines(snapshot);
  return PL_SUCCESS;
}

void plAudioProfilerCapture::AddInstances(const plAudioProfilingSnapshot& snapshot, double fTime)
{
  // the voices of each instance in this snapshot
  struct Seen
  {
    plUInt8 m_uiState = plAudioVoiceSnapshotEntry::Virtual;
    const plAudioVoiceSnapshotEntry* m_pFirst = nullptr;
    float m_fAudibility = 0.0f;
  };

  plHashTable<plUInt32, Seen> seen;
  for (const plAudioVoiceSnapshotEntry& voice : snapshot.m_Voices)
  {
    Seen& entry = seen[voice.m_uiId >> 8];
    if (entry.m_pFirst == nullptr)
    {
      entry.m_pFirst = &voice;
      entry.m_uiState = voice.m_uiState;
    }
    else
    {
      entry.m_uiState = CombineState(entry.m_uiState, voice.m_uiState);
    }

    entry.m_fAudibility = plMath::Max(entry.m_fAudibility, voice.m_fAudibility);
  }

  // instances that have no voice any more end at this snapshot
  for (auto it = m_OpenInstances.GetIterator(); it.IsValid();)
  {
    if (seen.Contains(it.Key()))
    {
      ++it;
      continue;
    }

    Instance& instance = m_Instances[it.Value()];
    instance.m_fEnd = fTime;
    instance.m_Segments.PeekBack().m_fEnd = fTime;
    it = m_OpenInstances.Remove(it);
  }

  for (auto it = seen.GetIterator(); it.IsValid(); ++it)
  {
    const Seen& voices = it.Value();

    plUInt32* pIndex = nullptr;
    if (!m_OpenInstances.TryGetValue(it.Key(), pIndex))
    {
      const plAudioVoiceSnapshotEntry& first = *voices.m_pFirst;

      Instance& instance = m_Instances.ExpandAndGetRef();
      instance.m_uiSerial = it.Key();
      instance.m_sEvent = first.m_sEvent;
      instance.m_sEntity = first.m_sEntity;
      instance.m_sBus = first.m_sBus;
      instance.m_uiCodec = first.m_uiCodec;
      instance.m_uiLoadMode = first.m_uiLoadMode;
      instance.m_fStart = fTime;
      instance.m_fEnd = fTime;
      instance.m_fPeakAudibility = voices.m_fAudibility;
      instance.m_Segments.PushBack({fTime, fTime, voices.m_uiState});

      m_OpenInstances.Insert(it.Key(), m_Instances.GetCount() - 1);
      continue;
    }

    Instance& instance = m_Instances[*pIndex];
    instance.m_fEnd = fTime;
    instance.m_fPeakAudibility = plMath::Max(instance.m_fPeakAudibility, voices.m_fAudibility);

    Segment& last = instance.m_Segments.PeekBack();
    last.m_fEnd = fTime;

    if (last.m_uiState != voices.m_uiState)
    {
      if (voices.m_uiState == plAudioVoiceSnapshotEntry::Virtual)
        ++instance.m_uiVirtualisations;

      instance.m_Segments.PushBack({fTime, fTime, voices.m_uiState});
    }
  }
}

void plAudioProfilerCapture::AddLogLines(const plAudioProfilingSnapshot& snapshot)
{
  // consecutive snapshots overlap; entries already taken are skipped, overwritten ones counted as lost
  const plUInt64 uiFirst = snapshot.m_uiEventLogFirstSequence;
  if (m_bLogSequenceKnown && uiFirst > m_uiNextLogSequence)
    m_uiLostLogLines += uiFirst - m_uiNextLogSequence;

  for (plUInt32 i = 0; i < snapshot.m_EventLog.GetCount(); ++i)
  {
    const plUInt64 uiSequence = uiFirst + i;
    if (m_bLogSequenceKnown && uiSequence < m_uiNextLogSequence)
      continue;

    const plAudioEventLogSnapshotEntry& entry = snapshot.m_EventLog[i];

    // entries from before the first snapshot are kept at time 0
    LogLine& line = m_Log.ExpandAndGetRef();
    line.m_fTime = plMath::Max(entry.m_fTime - m_fOrigin, 0.0);
    line.m_uiType = entry.m_uiType;
    line.m_sName = entry.m_sName;
    line.m_sDetail = entry.m_sDetail;
  }

  m_uiNextLogSequence = plMath::Max(m_uiNextLogSequence, uiFirst + snapshot.m_EventLog.GetCount());
  m_bLogSequenceKnown = true;
}

plUInt32 plAudioProfilerCapture::FindSample(double fTime) const
{
  // the samples are in time order
  plUInt32 uiLow = 0;
  plUInt32 uiHigh = m_Samples.GetCount();
  while (uiLow < uiHigh)
  {
    const plUInt32 uiMid = (uiLow + uiHigh) / 2;
    if (m_Samples[uiMid].m_fTime <= fTime)
      uiLow = uiMid + 1;
    else
      uiHigh = uiMid;
  }

  return uiLow > 0 ? uiLow - 1 : 0;
}

plResult plAudioProfilerCapture::GetSnapshot(plUInt32 uiSample, plAudioProfilingSnapshot& out_snapshot) const
{
  if (uiSample >= m_Snapshots.GetCount())
    return PL_FAILURE;

  const plDynamicArray<plUInt8>& data = m_Snapshots[uiSample].m_Data;
  plRawMemoryStreamReader reader(data.GetData(), data.GetCount());
  return out_snapshot.Read(reader);
}

plResult plAudioProfilerCapture::Save(plStreamWriter& inout_stream) const
{
  PL_SUCCEED_OR_RETURN(inout_stream.WriteBytes(s_szMagic, sizeof(s_szMagic)));
  inout_stream << s_uiFileVersion;

  plHybridArray<double, 64> receivedTimes;
  plHybridArray<const plDynamicArray<plUInt8>*, 64> data;
  for (const Stored& stored : m_Snapshots)
  {
    receivedTimes.PushBack(stored.m_fReceivedTime);
    data.PushBack(&stored.m_Data);
  }

#ifdef BUILDSYSTEM_ENABLE_ZSTD_SUPPORT
  inout_stream << static_cast<plUInt8>(1);

  plCompressedStreamWriterZstd compressed(&inout_stream, 0, plCompressedStreamWriterZstd::Compression::Default);
  PL_SUCCEED_OR_RETURN(WriteBody(compressed, receivedTimes, data));
  return compressed.FinishCompressedStream();
#else
  inout_stream << static_cast<plUInt8>(0);
  return WriteBody(inout_stream, receivedTimes, data);
#endif
}

plResult plAudioProfilerCapture::Load(plStreamReader& inout_stream)
{
  Clear();

  char magic[8] = {};
  plUInt8 uiVersion = 0;
  plUInt8 uiCompressed = 0;
  if (inout_stream.ReadBytes(magic, sizeof(magic)) != sizeof(magic) || plMemoryUtils::Compare(magic, s_szMagic, sizeof(magic)) != 0)
    return PL_FAILURE;

  inout_stream >> uiVersion;
  inout_stream >> uiCompressed;
  if (uiVersion == 0 || uiVersion > s_uiFileVersion)
    return PL_FAILURE;

  auto ReadBody = [this](plStreamReader& inout_body) -> plResult
  {
    plUInt32 uiCount = 0;
    inout_body >> uiCount;
    if (uiCount > MaxSamples)
      return PL_FAILURE;

    plDynamicArray<plUInt8> data;
    for (plUInt32 i = 0; i < uiCount; ++i)
    {
      double fReceived = 0.0;
      plUInt32 uiBytes = 0;
      inout_body >> fReceived;
      inout_body >> uiBytes;

      data.SetCountUninitialized(uiBytes);
      if (inout_body.ReadBytes(data.GetData(), uiBytes) != uiBytes)
        return PL_FAILURE;

      PL_SUCCEED_OR_RETURN(AddSnapshot(data, fReceived));
    }

    return PL_SUCCESS;
  };

  plResult result = PL_FAILURE;
  if (uiCompressed != 0)
  {
#ifdef BUILDSYSTEM_ENABLE_ZSTD_SUPPORT
    plCompressedStreamReaderZstd compressed(&inout_stream);
    result = ReadBody(compressed);
#endif
  }
  else
  {
    result = ReadBody(inout_stream);
  }

  if (result.Failed())
    Clear();

  return result;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Profiling_AudioProfilerCapture);
