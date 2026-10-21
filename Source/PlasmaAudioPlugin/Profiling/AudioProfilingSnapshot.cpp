#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>

namespace
{
  /// Upper bound for the voice count read from a stream, far above any voice budget.
  constexpr plUInt32 MaxSnapshotVoices = 1u << 16;

  /// Upper bound for a string read from a stream.
  constexpr plUInt32 MaxSnapshotStringBytes = 4096;

  /// Upper bound for the element count of the bus, memory and log sections read from a stream.
  constexpr plUInt32 MaxSnapshotSectionEntries = 1u << 16;

  template <typename T>
  plResult ReadValue(plStreamReader& inout_stream, T& out_value)
  {
    return inout_stream.ReadBytes(&out_value, sizeof(T)) == sizeof(T) ? PL_SUCCESS : PL_FAILURE;
  }

  template <typename T>
  void WriteValue(plStreamWriter& inout_stream, const T& value)
  {
    inout_stream.WriteBytes(&value, sizeof(T)).AssertSuccess();
  }

  plResult ReadCount(plStreamReader& inout_stream, plUInt32& out_uiCount)
  {
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, out_uiCount));
    return out_uiCount <= MaxSnapshotSectionEntries ? PL_SUCCESS : PL_FAILURE;
  }
} // namespace

const char* plAudioLogEventType::GetName(Enum type)
{
  switch (type)
  {
    case Start:
      return "start";
    case Stop:
      return "stop";
    case Finished:
      return "finished";
    case Stolen:
      return "stolen";
    case CooldownRejected:
      return "cooldown";
    case CulledAtStart:
      return "culled";
    case Virtualised:
      return "virtualise";
    case Realised:
      return "realise";
    case Killed:
      return "killed";
    case ClipMissing:
      return "clip missing";
    case ClipLoadWait:
      return "clip wait";
    case DeviceLost:
      return "device lost";
    case DeviceRecovered:
      return "device ok";
    case MixerLoaded:
      return "mixer";
    case BankLoaded:
      return "bank loaded";
    case BankUnloaded:
      return "bank unloaded";
    case LanguageChanged:
      return "language";
    case LimitRejected:
      return "limit";
    case EventLoadWait:
      return "event wait";
    case EventDropped:
      return "event dropped";
    case BudgetExceeded:
      return "over budget";
    case StreamUnderrun:
      return "underrun";
    case KeyOff:
      return "key off";
    case Marker:
      return "marker";
    case OutputState:
      return "output";
    default:
      return "?";
  }
}

bool plAudioLogEventType::IsWarning(Enum type)
{
  switch (type)
  {
    case ClipMissing:
    case Stolen:
    case CooldownRejected:
    case DeviceLost:
    case Killed:
    case LimitRejected:
    case EventDropped:
    case BudgetExceeded:
    case StreamUnderrun:
      return true;
    default:
      return false;
  }
}

void plAudioProfilingSnapshot::Clear()
{
  m_uiRealVoices = 0;
  m_uiVirtualVoices = 0;
  m_uiMaxReal = 0;
  m_uiMaxVirtual = 0;
  m_uiInstances = 0;
  m_uiStreams = 0;
  m_fDspLoad = 0.0f;
  m_fUpdateMs = 0.0f;
  m_uiMemoryBytes = 0;
  m_Voices.Clear();
  m_sMixer.Clear();
  m_Buses.Clear();
  m_ActiveSnapshots.Clear();
  m_Clips.Clear();
  m_Events.Clear();
  m_Banks.Clear();
  m_uiClipResidentBytes = 0;
  m_uiStreamRingBytes = 0;
  m_uiPooledStreams = 0;
  m_uiEventLogFirstSequence = 0;
  m_EventLog.Clear();
  m_fTime = 0.0;
  m_uiStreamUnderruns = 0;
  m_uiMemoryBudgetBytes = 0;
}

void plAudioProfilingSnapshot::Write(plStreamWriter& inout_stream) const
{
  WriteValue(inout_stream, kSnapshotVersion);

  WriteValue(inout_stream, m_uiRealVoices);
  WriteValue(inout_stream, m_uiVirtualVoices);
  WriteValue(inout_stream, m_uiMaxReal);
  WriteValue(inout_stream, m_uiMaxVirtual);
  WriteValue(inout_stream, m_uiInstances);
  WriteValue(inout_stream, m_uiStreams);
  WriteValue(inout_stream, m_fDspLoad);
  WriteValue(inout_stream, m_fUpdateMs);
  WriteValue(inout_stream, m_uiMemoryBytes);

  WriteValue(inout_stream, m_Voices.GetCount());

  for (const plAudioVoiceSnapshotEntry& voice : m_Voices)
  {
    WriteValue(inout_stream, voice.m_uiId);
    inout_stream.WriteString(voice.m_sEvent).AssertSuccess();
    WriteValue(inout_stream, voice.m_uiEntityId);
    WriteValue(inout_stream, voice.m_uiState);
    WriteValue(inout_stream, voice.m_uiPriority);
    WriteValue(inout_stream, voice.m_uiCodec);
    WriteValue(inout_stream, voice.m_uiLoadMode);
    WriteValue(inout_stream, voice.m_fAudibility);
    WriteValue(inout_stream, voice.m_fDistance);
    WriteValue(inout_stream, voice.m_fOcclusion);
    WriteValue(inout_stream, voice.m_fVolume);
    WriteValue(inout_stream, voice.m_fAge);
    inout_stream.WriteString(voice.m_sBus).AssertSuccess();
  }

  // version 2: buses
  inout_stream.WriteString(m_sMixer).AssertSuccess();

  WriteValue(inout_stream, m_Buses.GetCount());
  for (const plAudioBusSnapshotEntry& bus : m_Buses)
  {
    inout_stream.WriteString(bus.m_sName).AssertSuccess();
    WriteValue(inout_stream, bus.m_uiParent);
    WriteValue(inout_stream, static_cast<plUInt8>(bus.m_bIsAux ? 1 : 0));
    WriteValue(inout_stream, bus.m_fGainDb);
    WriteValue(inout_stream, bus.m_fPeak);
    WriteValue(inout_stream, bus.m_fRms);
  }

  WriteValue(inout_stream, m_ActiveSnapshots.GetCount());
  for (const plAudioActiveSnapshotEntry& snapshot : m_ActiveSnapshots)
  {
    inout_stream.WriteString(snapshot.m_sName).AssertSuccess();
    WriteValue(inout_stream, snapshot.m_fIntensity);
  }

  // version 2: memory
  WriteValue(inout_stream, m_Clips.GetCount());
  for (const plAudioClipMemorySnapshotEntry& clip : m_Clips)
  {
    inout_stream.WriteString(clip.m_sName).AssertSuccess();
    WriteValue(inout_stream, clip.m_uiLoadMode);
    WriteValue(inout_stream, clip.m_uiCodec);
    WriteValue(inout_stream, clip.m_uiResidentBytes);
    WriteValue(inout_stream, clip.m_uiStreams);
  }

  WriteValue(inout_stream, m_Events.GetCount());
  for (const plAudioEventMemorySnapshotEntry& event : m_Events)
  {
    inout_stream.WriteString(event.m_sName).AssertSuccess();
    WriteValue(inout_stream, event.m_uiSampleBytes);
  }

  WriteValue(inout_stream, m_Banks.GetCount());
  for (const plAudioBankMemorySnapshotEntry& bank : m_Banks)
  {
    inout_stream.WriteString(bank.m_sName).AssertSuccess();
    WriteValue(inout_stream, bank.m_uiEvents);
    WriteValue(inout_stream, bank.m_uiClips);
    WriteValue(inout_stream, bank.m_uiState);
  }

  WriteValue(inout_stream, m_uiClipResidentBytes);
  WriteValue(inout_stream, m_uiStreamRingBytes);
  WriteValue(inout_stream, m_uiPooledStreams);

  // version 2: event log
  WriteValue(inout_stream, m_uiEventLogFirstSequence);
  WriteValue(inout_stream, m_EventLog.GetCount());
  for (const plAudioEventLogSnapshotEntry& entry : m_EventLog)
  {
    WriteValue(inout_stream, entry.m_fTime);
    WriteValue(inout_stream, entry.m_uiType);
    inout_stream.WriteString(entry.m_sName).AssertSuccess();
    inout_stream.WriteString(entry.m_sDetail).AssertSuccess();
  }

  // version 3: capture
  WriteValue(inout_stream, m_fTime);
  WriteValue(inout_stream, m_uiStreamUnderruns);
  WriteValue(inout_stream, m_uiMemoryBudgetBytes);

  for (const plAudioVoiceSnapshotEntry& voice : m_Voices)
  {
    inout_stream.WriteString(voice.m_sEntity).AssertSuccess();
  }
}

plResult plAudioProfilingSnapshot::Read(plStreamReader& inout_stream)
{
  Clear();

  plUInt8 uiVersion = 0;
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, uiVersion));

  if (uiVersion == 0 || uiVersion > kSnapshotVersion)
    return PL_FAILURE;

  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_uiRealVoices));
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_uiVirtualVoices));
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_uiMaxReal));
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_uiMaxVirtual));
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_uiInstances));
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_uiStreams));
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_fDspLoad));
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_fUpdateMs));
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_uiMemoryBytes));

  plUInt32 uiVoices = 0;
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, uiVoices));

  if (uiVoices > MaxSnapshotVoices)
    return PL_FAILURE;

  m_Voices.SetCount(uiVoices);

  for (plAudioVoiceSnapshotEntry& voice : m_Voices)
  {
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, voice.m_uiId));
    PL_SUCCEED_OR_RETURN(inout_stream.ReadString(voice.m_sEvent, MaxSnapshotStringBytes));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, voice.m_uiEntityId));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, voice.m_uiState));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, voice.m_uiPriority));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, voice.m_uiCodec));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, voice.m_uiLoadMode));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, voice.m_fAudibility));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, voice.m_fDistance));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, voice.m_fOcclusion));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, voice.m_fVolume));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, voice.m_fAge));
    PL_SUCCEED_OR_RETURN(inout_stream.ReadString(voice.m_sBus, MaxSnapshotStringBytes));
  }

  if (uiVersion < 2)
    return PL_SUCCESS;

  plUInt32 uiCount = 0;

  // buses
  PL_SUCCEED_OR_RETURN(inout_stream.ReadString(m_sMixer, MaxSnapshotStringBytes));

  PL_SUCCEED_OR_RETURN(ReadCount(inout_stream, uiCount));
  m_Buses.SetCount(uiCount);
  for (plAudioBusSnapshotEntry& bus : m_Buses)
  {
    plUInt8 uiIsAux = 0;
    PL_SUCCEED_OR_RETURN(inout_stream.ReadString(bus.m_sName, MaxSnapshotStringBytes));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, bus.m_uiParent));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, uiIsAux));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, bus.m_fGainDb));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, bus.m_fPeak));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, bus.m_fRms));
    bus.m_bIsAux = uiIsAux != 0;
  }

  PL_SUCCEED_OR_RETURN(ReadCount(inout_stream, uiCount));
  m_ActiveSnapshots.SetCount(uiCount);
  for (plAudioActiveSnapshotEntry& snapshot : m_ActiveSnapshots)
  {
    PL_SUCCEED_OR_RETURN(inout_stream.ReadString(snapshot.m_sName, MaxSnapshotStringBytes));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, snapshot.m_fIntensity));
  }

  // memory
  PL_SUCCEED_OR_RETURN(ReadCount(inout_stream, uiCount));
  m_Clips.SetCount(uiCount);
  for (plAudioClipMemorySnapshotEntry& clip : m_Clips)
  {
    PL_SUCCEED_OR_RETURN(inout_stream.ReadString(clip.m_sName, MaxSnapshotStringBytes));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, clip.m_uiLoadMode));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, clip.m_uiCodec));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, clip.m_uiResidentBytes));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, clip.m_uiStreams));
  }

  PL_SUCCEED_OR_RETURN(ReadCount(inout_stream, uiCount));
  m_Events.SetCount(uiCount);
  for (plAudioEventMemorySnapshotEntry& event : m_Events)
  {
    PL_SUCCEED_OR_RETURN(inout_stream.ReadString(event.m_sName, MaxSnapshotStringBytes));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, event.m_uiSampleBytes));
  }

  PL_SUCCEED_OR_RETURN(ReadCount(inout_stream, uiCount));
  m_Banks.SetCount(uiCount);
  for (plAudioBankMemorySnapshotEntry& bank : m_Banks)
  {
    PL_SUCCEED_OR_RETURN(inout_stream.ReadString(bank.m_sName, MaxSnapshotStringBytes));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, bank.m_uiEvents));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, bank.m_uiClips));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, bank.m_uiState));
  }

  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_uiClipResidentBytes));
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_uiStreamRingBytes));
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_uiPooledStreams));

  // event log
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_uiEventLogFirstSequence));
  PL_SUCCEED_OR_RETURN(ReadCount(inout_stream, uiCount));
  m_EventLog.SetCount(uiCount);
  for (plAudioEventLogSnapshotEntry& entry : m_EventLog)
  {
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, entry.m_fTime));
    PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, entry.m_uiType));
    PL_SUCCEED_OR_RETURN(inout_stream.ReadString(entry.m_sName, MaxSnapshotStringBytes));
    PL_SUCCEED_OR_RETURN(inout_stream.ReadString(entry.m_sDetail, MaxSnapshotStringBytes));
  }

  if (uiVersion < 3)
    return PL_SUCCESS;

  // capture
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_fTime));
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_uiStreamUnderruns));
  PL_SUCCEED_OR_RETURN(ReadValue(inout_stream, m_uiMemoryBudgetBytes));

  for (plAudioVoiceSnapshotEntry& voice : m_Voices)
  {
    PL_SUCCEED_OR_RETURN(inout_stream.ReadString(voice.m_sEntity, MaxSnapshotStringBytes));
  }

  return PL_SUCCESS;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Profiling_AudioProfilingSnapshot);
