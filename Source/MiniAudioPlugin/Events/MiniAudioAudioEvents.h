#pragma once

#include <AudioSystem/Events/AudioEventResource.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Types/UniquePtr.h>
#include <MiniAudio/miniaudio.h>
#include <MiniAudioPlugin/MiniAudioPluginDLL.h>

class plMiniAudioBusGraph;
class plMiniAudioEntityData;
class plMiniAudioEventData;

/// \brief One playing occurrence of an audio event on an entity.
///
/// Owns one voice slot per instrument (decoder + ma_sound + occlusion LPF + optional
/// environment-send splitter) and performs instrument scheduling (start delays, playlist
/// advance, loop re-triggering), parameter automation and occlusion/environment application.
///
/// Audio-thread only.
class plMiniAudioEventInstance
{
public:
  ~plMiniAudioEventInstance();

  plResult Start(const plAudioEventResourceHandle& hEvent, plMiniAudioEntityData* pEntity, plMiniAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine, plRandom& ref_rng);

  /// \brief Begins the authored fade-out (or stops immediately). The instance finishes in a later Update.
  void Stop(bool bImmediate);

  /// \brief Advances scheduling/automation. Returns false when the instance has fully finished.
  bool Update(plTime delta, const plVec3& vListenerPos, plRandom& ref_rng);

  void SetParameter(plStringView sName, float fValue);

  PL_NODISCARD bool IsStopping() const { return m_bStopping; }
  PL_NODISCARD plUInt32 GetActiveVoiceCount() const;
  PL_NODISCARD float GetEffectiveVolume() const { return m_fCurrentVolumeMultiplier; }
  PL_NODISCARD plTime GetAge() const { return m_Elapsed; }

  plMiniAudioEntityData* m_pEntity = nullptr;
  plMiniAudioEventData* m_pEventData = nullptr;
  plUInt64 m_uiEventGroupHash = 0; ///< hash of the event resource id, for polyphony grouping

private:
  struct Voice
  {
    ma_decoder m_Decoder = {};
    ma_sound m_Sound = {};
    ma_lpf_node* m_pOcclusionLpf = nullptr;
    ma_splitter_node* m_pEnvSplitter = nullptr;
    float m_fBaseVolume = 1.0f;
    float m_fBasePitch = 1.0f;
    bool m_bActive = false;
    bool m_bDecoderValid = false;
  };

  struct InstrumentState
  {
    plTime m_NextTriggerTime;  ///< relative to instance clock; only valid when m_bPendingTrigger
    plUInt32 m_uiCursor = 0;   ///< sequential playlist cursor
    plUInt16 m_uiLastEntry = 0xFFFF;
    bool m_bPendingTrigger = true;
    bool m_bDone = false;
  };

  void StartVoice(plUInt32 uiInstrument, plRandom& ref_rng);
  void StopVoice(Voice& voice);
  plUInt16 PickPlaylistEntry(const plAudioEventInstrumentDesc& instrument, InstrumentState& state, plRandom& ref_rng);
  void ApplyModulation();
  float GetOcclusionCutoff() const;

  const plAudioEventResource* m_pResource = nullptr; // acquired (ref-counted) until destruction
  plMiniAudioBusGraph* m_pBusGraph = nullptr;
  ma_engine* m_pEngine = nullptr;

  plHybridArray<Voice, 2> m_Voices;           // one per instrument
  plHybridArray<InstrumentState, 2> m_Instruments;
  plHybridArray<float, 4> m_ParameterValues;  // one per desc parameter

  plTime m_Elapsed;
  bool m_bStopping = false;
  float m_fCurrentVolumeMultiplier = 1.0f;
  float m_fCurrentPitchMultiplier = 1.0f;
};

/// \brief Owns all playing event instances of the MiniAudio backend: polyphony groups
/// (max instances, stealing, cooldown), per-entity lookup and the per-update tick.
///
/// Audio-thread only.
class plMiniAudioEventManager
{
public:
  plResult ActivateEvent(plStringView sEventResourceId, plMiniAudioEntityData* pEntity, plMiniAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine);

  /// \brief Stops the instance bound to the given event data. Returns PL_FAILURE if none is playing.
  plResult StopEvent(const plMiniAudioEventData* pEventData, bool bImmediate);

  void StopAllOnEntity(const plMiniAudioEntityData* pEntity, bool bImmediate);
  void StopAll(bool bImmediate);

  void SetEventParameter(const plMiniAudioEntityData* pEntity, plStringView sParamName, float fValue);

  PL_NODISCARD bool IsEventPlaying(const plMiniAudioEventData* pEventData) const;

  void Update(plTime delta, const plVec3& vListenerPos);

  PL_NODISCARD plUInt32 GetInstanceCount() const { return m_Instances.GetCount(); }
  PL_NODISCARD plUInt32 GetActiveVoiceCount() const;

private:
  struct EventGroup
  {
    plTime m_LastStartTime;
    bool m_bHasStarted = false;
  };

  plDynamicArray<plUniquePtr<plMiniAudioEventInstance>> m_Instances;
  plHashTable<plUInt64, EventGroup> m_EventGroups;
  plRandom m_Rng;
  plTime m_Now;
  bool m_bRngInitialized = false;
};
