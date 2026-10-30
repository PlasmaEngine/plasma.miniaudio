#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Strings/String.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>

/// \brief The rows the Voices tab of the editor's Audio Profiler shows for a snapshot: its voices and ended voices,
/// filtered by state, bus and text, sorted and capped.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioProfilerVoiceQuery
{
  enum class SortKey : plUInt8
  {
    Default, ///< real voices first, then virtual and stopping, then ended; each by audibility, loudest first
    Event,
    Owner,
    State,
    Priority,
    Audibility,
    Volume,
    Distance,
    Bus,
    Codec,
  };

  /// Which states are shown, indexed by plAudioVoiceSnapshotEntry::State (real, virtual, stopping, ended).
  bool m_StateFilter[4] = {true, true, true, false};

  plString m_sBus;  ///< only voices on this bus; empty shows every bus
  plString m_sText; ///< only voices whose event, owner (entity name) or bus contains it, ignoring case

  SortKey m_SortKey = SortKey::Default;
  bool m_bAscending = false; ///< ignored for SortKey::Default
  plUInt32 m_uiMaxRows = 1000;

  /// \brief Replaces out_rows with the matching rows, at most m_uiMaxRows. The rows point into the snapshot's m_Voices
  /// and the m_Voice of its m_Ended. Returns how many rows matched before the cap.
  plUInt32 Run(const plAudioProfilingSnapshot& snapshot, plDynamicArray<const plAudioVoiceSnapshotEntry*>& out_rows) const;

  /// \brief Whether the voice passes the state, bus and text filters.
  bool Matches(const plAudioVoiceSnapshotEntry& voice) const;

  /// \brief Voices of the snapshot per state (real, virtual, stopping, ended), unfiltered.
  static void CountStates(const plAudioProfilingSnapshot& snapshot, plUInt32 out_counts[4]);
};
