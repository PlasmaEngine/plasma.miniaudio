#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Types/UniquePtr.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

class plMiniAudioBusGraph;
class plPlasmaAudioEventManager;
struct ma_engine;

/// \brief What an audition plays: an event resource, with switch states, a seed and a start offset.
///
/// The parameters start at their defaults; the editor writes the values it auditions into the defaults of the event it
/// sends, and SetParameters changes them while the event plays.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioAuditionRequest
{
  plString m_sEventResourceId;

  /// \brief Mixer resource loaded before the event starts when no mixer is loaded yet. A loaded mixer is kept, since
  /// loading another one stops everything that plays.
  plString m_sMixerResourceId;

  plTime m_StartOffset = plTime::MakeZero();

  bool m_bSeeded = false;
  plUInt32 m_uiSeed = 0;

  /// \brief Switch states as "Group/State".
  plDynamicArray<plString> m_SwitchStates;
};

/// \brief State of an audition, refreshed by every audio update.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioAuditionStatus
{
  bool m_bPlaying = false;
  bool m_bWaiting = false;  ///< the event or the mixer is still loading
  bool m_bHasMixer = false; ///< a mixer is loaded; without one the event plays straight to the output
  double m_fPosition = 0.0; ///< timeline position in seconds
  float m_fPeak = 0.0f;     ///< peak of the mixer's master bus (0 without a mixer)

  plDynamicArray<bool> m_InstrumentSounding;
  plDynamicArray<float> m_InstrumentRangeGains;
};

/// \brief Plays events for the editor's audition through the real event manager and mixer.
///
/// Each audition is addressed by a key (the editor document) and plays on its own global entity, so the event is not
/// spatialized. The requests are queued and carried out by the next audio update; GetStatus reports what the last
/// update saw. Any thread.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioAudition
{
  PL_DISALLOW_COPY_AND_ASSIGN(plAudioAudition);

public:
  plAudioAudition();
  ~plAudioAudition();

  /// \brief The audition of the running PlasmaAudio middleware; null without one.
  static plAudioAudition* GetSingleton() { return s_pSingleton; }

  /// \brief Stops what the key plays right away and starts the request.
  void Play(plUInt64 uiKey, const plAudioAuditionRequest& request);

  /// \brief Stops the key's audition with the event's stop fade.
  void Stop(plUInt64 uiKey);

  /// \brief Sets parameter values of the key's playing audition.
  void SetParameters(plUInt64 uiKey, plArrayPtr<const plString> names, plArrayPtr<const float> values);

  /// \brief Stops the key's audition right away and forgets the key.
  void Remove(plUInt64 uiKey);

  /// \brief State of the key's audition. Returns false for an unknown key.
  bool GetStatus(plUInt64 uiKey, plAudioAuditionStatus& out_status) const;

private:
  friend class plPlasmaAudioMiddleware;

  struct Command;
  struct Slot;

  /// \brief Carries out the queued requests and refreshes the status. Audio thread.
  void Update(plPlasmaAudioEventManager& ref_events, plMiniAudioBusGraph& ref_busGraph, ma_engine* pEngine, float fMasterPeak);

  /// \brief Stops every audition. Audio thread, or once the audio thread has stopped.
  void StopAll(plPlasmaAudioEventManager& ref_events);

  void Activate(Slot& ref_slot, plPlasmaAudioEventManager& ref_events, plMiniAudioBusGraph& ref_busGraph, ma_engine* pEngine);

  static plAudioAudition* s_pSingleton;

  mutable plMutex m_Mutex;
  plDynamicArray<Command> m_Commands;
  plHashTable<plUInt64, plAudioAuditionStatus> m_Status;

  // audio thread only
  plHashTable<plUInt64, plUniquePtr<Slot>> m_Slots;
  plUInt64 m_uiNextEntityId = 0xA0D1710000000000ull;
};
