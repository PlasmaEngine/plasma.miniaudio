#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>

/// \brief Bounded ring of the voices that ended last, with their final values, for the Voices tab of the editor's
/// Audio Profiler (plAudioProfilingSnapshot::m_Ended).
///
/// Records nothing while disabled; disabling drops the recorded voices. The ring memory is allocated when it is
/// enabled. Not thread-safe: the event manager adds and collects under its mutex.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioEndedVoices
{
public:
  static constexpr plUInt32 Capacity = 256;

  void SetEnabled(bool bEnabled);
  PL_NODISCARD bool IsEnabled() const { return m_bEnabled; }

  /// \brief Records a voice that ended at fEndTime (seconds on the event log's clock). Overwrites the oldest voice once
  /// the ring is full.
  void Add(const plAudioVoiceSnapshotEntry& voice, double fEndTime);

  /// \brief Replaces out_ended with the voices that ended within fWindow seconds before fNow, oldest first.
  void Collect(double fNow, double fWindow, plDynamicArray<plAudioEndedVoiceSnapshotEntry>& out_ended) const;

  /// \brief Voices recorded since the ring was enabled, including overwritten ones.
  PL_NODISCARD plUInt64 GetTotalAdded() const { return m_uiTotalAdded; }

private:
  plDynamicArray<plAudioEndedVoiceSnapshotEntry> m_Ring; ///< Capacity entries while enabled
  plUInt64 m_uiTotalAdded = 0;                           ///< the entry of voice n is at n % Capacity
  bool m_bEnabled = false;
};
