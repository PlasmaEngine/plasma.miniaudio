#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/IO/Stream.h>
#include <Foundation/Strings/String.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

/// \brief One active voice of an event instance, as shown by the editor's Audio Profiler.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioVoiceSnapshotEntry
{
  enum State : plUInt8
  {
    Real = 0,
    Virtual = 1,
    Stopping = 2, ///< the voice's instance fades out (the voice itself may be real or virtual)
  };

  /// \brief m_uiCodec and m_uiLoadMode of a voice that decodes a sample embedded in the event (no audio clip).
  static constexpr plUInt8 Embedded = 0xFF;

  plUInt32 m_uiId = 0;         ///< instance serial << 8 | voice slot; stable while the voice plays
  plString m_sEvent;           ///< resource description of the event (its file path) or, without one, the resource id
  plUInt64 m_uiEntityId = 0;   ///< audio system entity id; 0 without an entity
  plUInt8 m_uiState = Real;    ///< State
  plUInt8 m_uiPriority = 0;
  plUInt8 m_uiCodec = Embedded;    ///< plAudioClipCodec::Enum or Embedded
  plUInt8 m_uiLoadMode = Embedded; ///< plAudioClipLoadMode::Enum or Embedded
  float m_fAudibility = 0.0f;  ///< the instance audibility the voice manager ranks by
  float m_fDistance = 0.0f;    ///< entity to listener distance in meters
  float m_fOcclusion = 0.0f;   ///< current occlusion of the entity, 0..1
  float m_fVolume = 0.0f;      ///< linear voice volume (instance volume times the voice's base volume)
  float m_fAge = 0.0f;         ///< seconds since the instance started
  plString m_sBus;             ///< name of the mixer bus the instance plays on
};

/// \brief One bus of the loaded mixer, as shown on the Buses tab of the editor's Audio Profiler.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioBusSnapshotEntry
{
  static constexpr plUInt8 NoParent = 0xFF;

  plString m_sName;
  plUInt8 m_uiParent = NoParent; ///< index of the parent bus in plAudioProfilingSnapshot::m_Buses, NoParent for the master bus
  bool m_bIsAux = false;
  float m_fGainDb = 0.0f; ///< the bus's own gain: bus volume with VCAs and snapshots applied
  float m_fPeak = 0.0f;   ///< linear peak of the bus output
  float m_fRms = 0.0f;    ///< linear RMS of the bus output
};

/// \brief One active mixer snapshot (intensity above 0).
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioActiveSnapshotEntry
{
  plString m_sName;
  float m_fIntensity = 0.0f;
};

/// \brief One loaded audio clip resource.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioClipMemorySnapshotEntry
{
  plString m_sName;      ///< resource description or, without one, the resource id
  plUInt8 m_uiLoadMode = 0; ///< plAudioClipLoadMode::Enum
  plUInt8 m_uiCodec = 0;    ///< plAudioClipCodec::Enum
  plUInt64 m_uiResidentBytes = 0;
  plUInt32 m_uiStreams = 0; ///< open disk streams reading the clip
};

/// \brief One loaded audio event resource.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEventMemorySnapshotEntry
{
  plString m_sName; ///< resource description or, without one, the resource id
  plUInt64 m_uiSampleBytes = 0; ///< bytes of the embedded samples
};

/// \brief One bank loaded through the middleware.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioBankMemorySnapshotEntry
{
  enum State : plUInt8
  {
    Loading = 0, ///< the bank resource is still loading
    Loaded = 1,  ///< the loads of its events and clips were started
    Missing = 2, ///< the bank resource could not be loaded
  };

  plString m_sName;
  plUInt32 m_uiEvents = 0;
  plUInt32 m_uiClips = 0;
  plUInt8 m_uiState = Loading; ///< State
};

/// \brief What an entry of the PlasmaAudio event log records.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioLogEventType
{
  using StorageType = plUInt8;

  enum Enum : plUInt8
  {
    Start,            ///< an event instance started
    Stop,             ///< an event instance was asked to stop
    Finished,         ///< an event instance ended
    Stolen,           ///< an event instance was stopped for a new instance of its polyphony group
    CooldownRejected, ///< a start was dropped because the event's cooldown had not passed
    CulledAtStart,    ///< a voice started virtual (not admitted by the voice budget or beyond the max distance)
    Virtualised,      ///< a real voice became virtual
    Realised,         ///< a virtual voice became real
    Killed,           ///< a voice or instance was stopped by the voice budget
    ClipMissing,      ///< a clip could not be loaded or cannot play; its playlist entry plays silence
    ClipLoadWait,     ///< a voice waits for its clip to finish loading
    DeviceLost,       ///< the output device changed and the audio engine is recreated
    DeviceRecovered,  ///< the audio engine runs on the new output device
    MixerLoaded,
    BankLoaded,
    BankUnloaded,
    LanguageChanged,
    LimitRejected,    ///< a start was dropped because the event's max instances were playing and it does not steal
    EventLoadWait,    ///< a start waits for its event to finish loading
    EventDropped,     ///< a waiting start was dropped: the event is missing, or a one-shot did not load in time
    BudgetExceeded,   ///< a bank finished loading with the loaded content over a memory budget
    Count
  };

  /// \brief Short lowercase name of the type, e.g. "start".
  static const char* GetName(Enum type);

  /// \brief Whether the type is highlighted as a warning.
  static bool IsWarning(Enum type);
};

/// \brief One entry of the PlasmaAudio event log.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEventLogSnapshotEntry
{
  double m_fTime = 0.0; ///< seconds since the log was created
  plUInt8 m_uiType = 0; ///< plAudioLogEventType::Enum
  plString m_sName;     ///< event, bank, mixer or language the entry is about
  plString m_sDetail;
};

/// \brief State of the PlasmaAudio voices, buses, memory and event log at one audio update, serialized for the
/// editor's Audio Profiler.
///
/// The middleware produces it on the audio thread while PlasmaAudio.ProfilerSnapshotHz is above 0 and the game
/// application publishes the serialized form as the stat "PlasmaAudio/VoiceSnapshot" (a plDataBuffer).
///
/// Version 2 adds the buses, memory and event log sections; data of version 1 reads with those sections empty.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioProfilingSnapshot
{
  static constexpr plUInt8 kSnapshotVersion = 2;

  plUInt32 m_uiRealVoices = 0;
  plUInt32 m_uiVirtualVoices = 0;
  plUInt32 m_uiMaxReal = 0;
  plUInt32 m_uiMaxVirtual = 0;
  plUInt32 m_uiInstances = 0;
  plUInt32 m_uiStreams = 0;     ///< streamed clip voices reading from disk
  float m_fDspLoad = 0.0f;      ///< fraction of the mix period spent mixing
  float m_fUpdateMs = 0.0f;     ///< duration of the last audio update
  plUInt64 m_uiMemoryBytes = 0; ///< resident clip sample data plus stream rings
  plDynamicArray<plAudioVoiceSnapshotEntry> m_Voices;

  /// \name Buses
  /// Empty while no mixer is loaded.
  ///@{
  plString m_sMixer; ///< resource id of the loaded mixer
  plDynamicArray<plAudioBusSnapshotEntry> m_Buses;
  plDynamicArray<plAudioActiveSnapshotEntry> m_ActiveSnapshots;
  ///@}

  /// \name Memory
  ///@{
  plDynamicArray<plAudioClipMemorySnapshotEntry> m_Clips;
  plDynamicArray<plAudioEventMemorySnapshotEntry> m_Events;
  plDynamicArray<plAudioBankMemorySnapshotEntry> m_Banks;
  plUInt64 m_uiClipResidentBytes = 0; ///< sample data of all loaded clips
  plUInt64 m_uiStreamRingBytes = 0;   ///< rings of the open and pooled streams
  plUInt32 m_uiPooledStreams = 0;
  ///@}

  /// \name Event log
  /// The log entries the snapshot carries, oldest first, with consecutive sequence numbers starting at
  /// m_uiEventLogFirstSequence. Consecutive snapshots overlap; a reader skips the sequence numbers it has already seen.
  /// A first sequence number above the next one the reader expects means that entries were lost.
  ///@{
  plUInt64 m_uiEventLogFirstSequence = 0;
  plDynamicArray<plAudioEventLogSnapshotEntry> m_EventLog;
  ///@}

  void Clear();

  void Write(plStreamWriter& inout_stream) const;

  /// \brief Fails for data of a newer version or when the stream ends early.
  plResult Read(plStreamReader& inout_stream);
};
