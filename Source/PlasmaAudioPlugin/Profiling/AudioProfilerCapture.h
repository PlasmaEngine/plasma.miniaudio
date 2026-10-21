#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Types/ArrayPtr.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>

/// \brief A recording of profiling snapshots for the editor's Audio Profiler: a sample per snapshot for the graphs,
/// every event instance as a lane of real, virtual and stopping segments, the event log, and the snapshots themselves
/// so the Voices, Buses and Memory tabs can show any moment of it.
///
/// Times are seconds since the capture's first snapshot, on the clock of the middleware's event log (snapshots older
/// than version 3 have no time of their own and use the time they were received). A capture holds at most MaxSamples
/// snapshots; AddSnapshot fails once it is full.
///
/// File format of a .plAudioCapture: the 8 bytes "PLAUDCAP", u8 version (1), u8 compressed (zstd when 1), then, in
/// the compressed stream when compressed: u32 count, per snapshot: f64 received time, u32 bytes, the serialized
/// snapshot (plAudioProfilingSnapshot::Write).
class PL_PLASMAAUDIOPLUGIN_DLL plAudioProfilerCapture
{
public:
  /// \brief Ten minutes at the profiler's 10 snapshots a second.
  static constexpr plUInt32 MaxSamples = 6000;

  /// \brief What the graphs plot of one snapshot.
  struct Sample
  {
    double m_fTime = 0.0;
    plUInt32 m_uiReal = 0;
    plUInt32 m_uiVirtual = 0;
    plUInt32 m_uiMaxReal = 0;
    plUInt32 m_uiMaxVirtual = 0;
    float m_fDspLoad = 0.0f; ///< fraction of the mix period
    plUInt32 m_uiStreams = 0;
    plUInt64 m_uiMemoryBytes = 0;
    plUInt64 m_uiBudgetBytes = 0; ///< total memory budget, 0 without one
    plUInt32 m_uiUnderruns = 0;   ///< stream underruns since the previous sample
  };

  /// \brief A stretch of an instance in one state (plAudioVoiceSnapshotEntry::State).
  struct Segment
  {
    double m_fStart = 0.0;
    double m_fEnd = 0.0;
    plUInt8 m_uiState = plAudioVoiceSnapshotEntry::Real;
  };

  /// \brief An event instance from the first to the last snapshot that had a voice of it. It is real while any of
  /// its voices is real, and stopping while it fades out.
  struct Instance
  {
    plUInt32 m_uiSerial = 0; ///< the instance part of the voice ids (id >> 8)
    plString m_sEvent;
    plString m_sEntity;
    plString m_sBus;
    plUInt8 m_uiCodec = plAudioVoiceSnapshotEntry::Embedded;
    plUInt8 m_uiLoadMode = plAudioVoiceSnapshotEntry::Embedded;
    double m_fStart = 0.0;
    double m_fEnd = 0.0;
    float m_fPeakAudibility = 0.0f;
    plUInt32 m_uiVirtualisations = 0;
    plHybridArray<Segment, 2> m_Segments;
  };

  /// \brief An event log entry at its capture time.
  struct LogLine
  {
    double m_fTime = 0.0;
    plUInt8 m_uiType = 0; ///< plAudioLogEventType::Enum
    plString m_sName;
    plString m_sDetail;
  };

  void Clear();

  bool IsEmpty() const { return m_Samples.IsEmpty(); }
  bool IsFull() const { return m_Samples.GetCount() >= MaxSamples; }

  /// \brief Adds a serialized snapshot received at fReceivedTime (seconds, any clock). Fails for data that does not
  /// read as a snapshot and when the capture is full.
  plResult AddSnapshot(plArrayPtr<const plUInt8> data, double fReceivedTime);

  /// \brief Seconds from the first to the last sample.
  double GetDuration() const;

  const plDynamicArray<Sample>& GetSamples() const { return m_Samples; }
  const plDynamicArray<Instance>& GetInstances() const { return m_Instances; }
  const plDynamicArray<LogLine>& GetLog() const { return m_Log; }

  /// \brief Log entries the middleware overwrote before a snapshot carried them.
  plUInt64 GetLostLogLines() const { return m_uiLostLogLines; }

  /// \brief The index of the last sample at or before fTime (the first one before the capture starts).
  plUInt32 FindSample(double fTime) const;

  /// \brief Reads the snapshot of a sample.
  plResult GetSnapshot(plUInt32 uiSample, plAudioProfilingSnapshot& out_snapshot) const;

  plResult Save(plStreamWriter& inout_stream) const;

  /// \brief Replaces the capture with the file's; on failure the capture is empty.
  plResult Load(plStreamReader& inout_stream);

private:
  void AddInstances(const plAudioProfilingSnapshot& snapshot, double fTime);
  void AddLogLines(const plAudioProfilingSnapshot& snapshot);

  struct Stored
  {
    double m_fReceivedTime = 0.0;
    plDynamicArray<plUInt8> m_Data;
  };

  plDynamicArray<Sample> m_Samples;
  plDynamicArray<Stored> m_Snapshots;
  plDynamicArray<Instance> m_Instances;
  plDynamicArray<LogLine> m_Log;

  plHashTable<plUInt32, plUInt32> m_OpenInstances; ///< serial -> index into m_Instances, of the last snapshot's instances
  double m_fOrigin = 0.0;                           ///< snapshot time of the first sample
  double m_fReceivedOrigin = 0.0;                   ///< received time of the first sample
  plUInt64 m_uiLastUnderruns = 0;
  plUInt64 m_uiNextLogSequence = 0;
  bool m_bLogSequenceKnown = false;
  plUInt64 m_uiLostLogLines = 0;
};
