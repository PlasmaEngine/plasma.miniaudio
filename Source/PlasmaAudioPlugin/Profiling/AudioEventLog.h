#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Strings/StringView.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Time/Time.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>

/// \brief Bounded ring of the recent PlasmaAudio events (instance starts and stops, stealing, voice changes, clip
/// problems, device, mixer, bank and language changes) for the editor's Audio Profiler.
///
/// The ring holds the newest Capacity entries; each entry gets a running sequence number, so a reader can tell how many
/// entries it missed. The ring memory is allocated by the constructor and Add() never allocates: names and details are
/// stored in fixed-size buffers, and longer text keeps its end (the file name of a path).
///
/// Thread-safe: Add() and Collect() take a mutex.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioEventLog
{
  PL_DISALLOW_COPY_AND_ASSIGN(plAudioEventLog);

public:
  static constexpr plUInt32 Capacity = 256;
  static constexpr plUInt32 NameBytes = 96;
  static constexpr plUInt32 DetailBytes = 64;

  plAudioEventLog();

  void Add(plAudioLogEventType::Enum type, plStringView sName, plStringView sDetail = plStringView());

  /// \brief Replaces out_entries with the entries whose sequence number is at least uiFirstSequence and that are still
  /// in the ring, oldest first. Returns the sequence number of the first returned entry; it is above uiFirstSequence
  /// when entries were overwritten before they were collected.
  plUInt64 Collect(plUInt64 uiFirstSequence, plDynamicArray<plAudioEventLogSnapshotEntry>& out_entries) const;

  /// \brief The sequence number the next entry gets, which is the number of entries added so far.
  plUInt64 GetNextSequence() const;

private:
  struct Entry
  {
    double m_fTime = 0.0;
    plUInt8 m_uiType = 0;
    char m_szName[NameBytes] = {};
    char m_szDetail[DetailBytes] = {};
  };

  mutable plMutex m_Mutex;
  plDynamicArray<Entry> m_Entries; ///< Capacity entries; the entry of sequence number s is at s % Capacity
  plUInt64 m_uiNextSequence = 0;
  plTime m_StartTime;
};
