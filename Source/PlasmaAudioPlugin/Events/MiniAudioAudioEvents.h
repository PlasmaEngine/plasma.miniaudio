#pragma once

#include <PlasmaAudioPlugin/Events/AudioEventResource.h>
#include <PlasmaAudioPlugin/Events/AudioSpatializer.h>
#include <PlasmaAudioPlugin/Profiling/AudioDebugSnapshot.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Types/UniquePtr.h>
#include <MiniAudio/miniaudio.h>
#include <PlasmaAudioPlugin/Clips/AudioClipVoiceSource.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <Foundation/Threading/Mutex.h>
#include <PlasmaAudioPlugin/Events/AudioVoiceManager.h>
#include <PlasmaAudioPlugin/Mixer/MiniAudioDspNodes.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>
#include <PlasmaAudioPlugin/Profiling/AudioEventLog.h>
#include <PlasmaAudioPlugin/Resources/MiniAudioSoundResource.h>

class plAudioStreamService;
class plAudioVoiceManager;
class plMiniAudioBusGraph;
class plPlasmaAudioEntityData;
class plPlasmaAudioEventData;
struct plAudioProfilingSnapshot;
struct plAudioVoiceSnapshotEntry;

/// \brief The data source of a voice: silence up to the voice's start frame, the inner source's data (clip source or
/// decoder), then one silent frame.
///
/// miniaudio starts a sound only at the beginning of a processing chunk (the first chunk that begins at or after
/// its start time), so a start time inside a chunk would be rounded up to the next chunk. The voice's sound is
/// instead started right away, and this source emits silence until the start frame: on its first read, the
/// engine time is the beginning of the chunk being mixed, and the distance to the start frame is converted to
/// source frames with the voice's playback rate.
///
/// A pitched ma_sound runs its data through a linear resampler that holds back one frame. When the data source
/// ends, that held frame is never emitted, so a voice would lose its final frame and a follow-up voice scheduled
/// at the voice's end frame would join with a one-frame hole. The appended silent frame pushes the real last
/// frame out and is itself the frame that stays behind.
///
/// A start fade (a fade-in from silence at the frame where the data begins) is applied by the source as well: the data
/// begins at the start frame or, when the engine is already past it, at the beginning of the first chunk the sound is
/// mixed in. Only the first read knows which, so the first read sets the sound's fader, which the sound applies to the
/// frames of that same read. A fade anchored at an engine frame from outside the device thread could instead start
/// behind the chunk the sound is first mixed in and be skipped in part or whole.
struct plMaVoiceDataSource
{
  ma_data_source_base m_Base;
  ma_data_source* m_pInner = nullptr;
  ma_engine* m_pEngine = nullptr;
  ma_format m_eFormat = ma_format_unknown;
  ma_uint32 m_uiChannels = 0;
  ma_uint64 m_uiStartFrame = 0;                ///< absolute engine frame at which the inner data starts
  double m_fSourceFramesPerEngineFrame = 1.0;  ///< source rate / engine rate * pitch, set before the sound starts
  ma_uint64 m_uiLeadFramesRemaining = 0;       ///< source frames of silence still to emit
  ma_fader* m_pStartFader = nullptr;           ///< fader of the sound that reads the source; set before the sound starts
  ma_uint64 m_uiStartFadeFrames = 0;           ///< length of the start fade in engine frames; 0 for none
  float m_fStartFadeVolume = 1.0f;             ///< fader volume at the end of the start fade
  bool m_bLeadResolved = false;
  bool m_bStartFadeApplied = false;
  bool m_bPadded = false;

  /// \name Published by the device thread after every read, for the audio thread
  /// The audio thread never reads the inner source's cursor itself: the device thread advances it concurrently.
  ///@{
  plAtomicInteger64 m_iPublishedCursor; ///< inner source cursor (source frames) after the last read
  plAtomicInteger64 m_iStarvedFrames;   ///< source frames of silence the inner source returned without advancing (stream underruns)
  ///@}
};

/// \brief Clip loads an event instance asks for while the event manager's mutex is held.
///
/// Loading or reloading a resource takes the resource manager's mutex, which must never be acquired while the event
/// manager's mutex is held (see plPlasmaAudioEventManager). The manager issues these requests once it released its mutex.
struct plPlasmaAudioDeferredLoads
{
  plHybridArray<plAudioClipResourceHandle, 8> m_Preload;
  plHybridArray<plAudioClipResourceHandle, 4> m_Reload; ///< reloaded (for a language switch), then preloaded

  void Preload(const plAudioClipResourceHandle& hClip);
  void Reload(const plAudioClipResourceHandle& hClip);
  PL_NODISCARD bool IsEmpty() const { return m_Preload.IsEmpty() && m_Reload.IsEmpty(); }
};

/// \brief One playing occurrence of an audio event on an entity.
///
/// Owns two voice slots per instrument (clip source or decoder + ma_sound + occlusion LPF + optional
/// environment-send splitter) and performs instrument scheduling (start delays, playlist
/// advance, loop re-triggering), parameter automation and occlusion/environment application.
///
/// A playlist entry that references an audio clip plays through a plAudioClipVoiceSource; entries without a clip
/// decode their embedded sample with an ma_decoder. A clip that is not loaded yet when its trigger comes up
/// postpones the trigger by one look-ahead span.
///
/// Scheduling is sample-accurate: every voice gets an absolute engine start frame, realized by the
/// silence its plMaVoiceDataSource emits before the sample data, and triggers are committed once they fall inside the
/// look-ahead window (now + plPlasmaAudioEventManager::GetLookAheadFrames). A looping instrument
/// schedules its follow-up voice on the second slot while the current voice still plays, at the
/// estimated end frame of the current voice plus the retrigger gap, so both slots overlap only
/// for the look-ahead span. Single-entry gapless loops use miniaudio's native data source looping.
///
/// A voice is either real (it owns the decoding chain and is mixed) or virtual (see plAudioVoiceManager): a virtual
/// voice keeps its start frame, pitch, length and loop info and runs out at its estimated end frame. It becomes real
/// again with its data source sought to where its timeline is at that point (or to the start, for
/// plAudioEventVirtualMode::Restart). A voice that starts while it is not admitted by the voice manager, or beyond the
/// event's max distance, starts virtual; the follow-up voice of a looping instrument inherits the state of the
/// instrument's current voice. An event with plAudioEventVirtualMode::Kill has no virtual voices: the whole instance
/// is stopped immediately when one of its voices is not admitted or is virtualized.
///
/// Changes between real and virtual are faded over VirtualFadeDuration. A voice that goes virtual counts as virtual
/// right away, but its chain keeps playing a fade-out and is torn down (releasing its decoder, clip source and stream)
/// by the first Update after the fade ended and the chain was mixed silently; the fade starts with the next chunk the
/// sound is mixed in, so the teardown waits one look-ahead span plus the fade length. A voice realized in the middle
/// of its timeline fades in from its resume frame. A voice of a streamed clip stays virtual while every stream is taken,
/// including streams still held by fading-out voices, and is realized by a later voice manager Update once one is
/// released. Resource unloading and instance stops still cut voices immediately.
///
/// A virtual voice keeps its timeline position as a source-frame cursor that every Update advances by the elapsed
/// engine frames times the pitch of that moment, so a voice whose pitch changed resumes where it would have been. A
/// real voice's position is the cursor its data source publishes from the device thread.
///
/// Used by plPlasmaAudioEventManager, under the manager's mutex (see there). The manager pools finished
/// instances; Reset() returns an instance to its unstarted state and keeps the capacity of its arrays and its clip
/// sources, so a reused instance starts without heap allocations of its own.
class plPlasmaAudioEventInstance
{
public:
  ~plPlasmaAudioEventInstance();

  /// \brief Duration of the fades that hide a voice's changes between real and virtual.
  static constexpr plTime VirtualFadeDuration = plTime::MakeFromMilliseconds(10);

  /// \brief How many environments a voice sends to at once: the ones with the largest amounts, blended by amount.
  static constexpr plUInt32 MaxEnvironmentSends = 3;

  /// \brief Stops every voice, releases the event resource and unbinds the event data, then returns all members to
  /// their defaults. Array capacities and allocated clip sources are kept for the next Start().
  void Reset();

  /// \param pGlobalParameters Manager-owned snapshot of the backend's global parameters (name hash -> value); may be null.
  plResult Start(const plAudioEventResourceHandle& hEvent, plPlasmaAudioEntityData* pEntity, plPlasmaAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine, const plHashTable<plUInt64, float>* pGlobalParameters, plRandom& ref_rng);

  /// \brief Begins the authored fade-out (or stops immediately). The instance finishes in a later Update.
  void Stop(bool bImmediate);

  /// \brief Advances scheduling/automation. Returns false when the instance has fully finished.
  ///
  /// The listener closest to the entity is the one the voices are spatialized against; distance, the Distance
  /// parameter and the audibility use it. An empty array keeps the last listener position.
  bool Update(plTime delta, plArrayPtr<const plVec3> listeners, plRandom& ref_rng);

  /// \brief Sets an instance-local parameter value. Once set, it takes precedence over a global parameter of the same name.
  void SetParameter(plStringView sName, float fValue);

  /// \brief Drops the instance-local value of a parameter: it follows the global parameter of the same name again, or
  /// goes back to its default without one.
  void ResetParameter(plStringView sName);

  /// \brief Releases the next sustain point: the instruments behind it start, as far behind the moment of the key-off
  /// as they are behind the sustain point. Before the timeline reaches the sustain point, it just does not stop there.
  void KeyOff();

  /// \brief How many sustain points of the event were released (or lay before the start offset).
  PL_NODISCARD plUInt32 GetReleasedSustainCount() const { return m_uiReleasedSustains; }

  /// \brief The sustain points not released yet, and whether the playhead waits at one.
  void GetSustainState(plUInt32& out_uiSustainsLeft, bool& out_bHolding) const;

  /// \brief Sets m_vListenerPos to the listener closest to the entity (or the first one for a global entity).
  void SelectListener(plArrayPtr<const plVec3> listeners);

  PL_NODISCARD bool IsStopping() const { return m_bStopping; }

  /// \brief Whether an audible instrument of the event loops (re-triggers until the instance is stopped).
  PL_NODISCARD bool HasLoopingInstrument() const;

  /// \brief Real and virtual voices.
  PL_NODISCARD plUInt32 GetActiveVoiceCount() const;
  PL_NODISCARD plUInt32 GetRealVoiceCount() const;
  PL_NODISCARD plUInt32 GetVirtualVoiceCount() const;
  PL_NODISCARD float GetEffectiveVolume() const { return m_fCurrentVolumeMultiplier * m_fCurrentSpatialGain; }

  /// \brief The distance and occlusion gain of the last update (see ComputeSpatialGain).
  PL_NODISCARD float GetSpatialGain() const { return m_fCurrentSpatialGain; }

  /// \brief Fills the emitter the Audio view mode draws for this instance. Returns false for an event without a place
  /// (global entity, 2D event, unloaded resource).
  bool GetDebugEmitter(plAudioDebugEmitter& out_emitter) const;
  PL_NODISCARD plTime GetAge() const { return m_Elapsed; }

  /// \brief Position on the event's timeline in seconds: time since the start (plus the start offset), restarting at
  /// the loop start with every pass of a looping region.
  PL_NODISCARD double GetTimelinePosition() const;

  /// \brief Number of instruments of the playing event.
  PL_NODISCARD plUInt32 GetInstrumentCount() const { return m_Instruments.GetCount(); }

  /// \brief Whether the instrument has an active voice or a nested event that plays, and the volume factor of its
  /// parameter range.
  PL_NODISCARD bool IsInstrumentSounding(plUInt32 uiInstrument) const;
  PL_NODISCARD float GetInstrumentRangeGain(plUInt32 uiInstrument) const { return m_Instruments[uiInstrument].m_fRangeGain; }

  /// \brief The event resource the instance plays; null once that resource was unloaded.
  PL_NODISCARD const plAudioEventResource* GetResource() const { return m_pResource; }

  /// \brief Resource description of the event (its file path) or, without one, the resource id; "<unloaded>" once
  /// the resource was unloaded.
  PL_NODISCARD plStringView GetDisplayName() const;

  /// \brief The clip resource is about to be unloaded: stops every voice that reads it. A looping instrument whose
  /// current voice was stopped re-triggers right away (and plays again once the clip is reloaded).
  void OnClipUnloading(const plAudioClipResource* pClip);

  /// \brief The event resource of the instance is about to be unloaded: stops all voices and releases the resource.
  /// The instance no longer reads it and finishes in the next Update.
  void OnEventUnloading();

  /// \brief The bus graph the instance outputs to is about to be rebuilt: detaches the voices' outputs from its buses.
  /// The voices keep playing into nothing until ReattachToBusGraph.
  void DetachFromBusGraph();

  /// \brief The bus graph was rebuilt: resolves the output bus again and attaches the voices to it. Environment sends
  /// are attached again by the next Update.
  void ReattachToBusGraph();

  /// \brief The bus graph the voices attach to; null without one.
  PL_NODISCARD const plMiniAudioBusGraph* GetBusGraph() const { return m_pBusGraph; }
  PL_NODISCARD plMiniAudioBusGraph* GetBusGraph() { return m_pBusGraph; }

  plPlasmaAudioEntityData* m_pEntity = nullptr;
  plPlasmaAudioEventData* m_pEventData = nullptr;
  plUInt64 m_uiEventGroupHash = 0; ///< hash of the event resource id, for polyphony grouping

  /// \brief Stream service for streamed clips; null while streaming is unavailable (streamed clips then fail to start).
  plAudioStreamService* m_pStreams = nullptr;

  /// \brief Admits new voices as real or virtual; null makes every voice real.
  plAudioVoiceManager* m_pVoiceManager = nullptr;

  /// \brief Receives the voice changes and clip problems of the instance; null logs nothing.
  plAudioEventLog* m_pEventLog = nullptr;

  /// \brief Collects the clip loads the instance needs; the manager issues them outside its mutex. Null issues none.
  plPlasmaAudioDeferredLoads* m_pDeferredLoads = nullptr;

  /// \brief Listener position for the audibility; updated by every Update.
  plVec3 m_vListenerPos = plVec3::MakeZero();

  plUInt64 m_uiSerial = 0; ///< start order among the manager's instances, the last tie-break of the voice ranking

  /// \name Nested events
  /// A playlist entry that plays an event asks the manager for a child instance on the same entity. The parent lives
  /// while its children do, and stopping it stops them.
  ///@{

  /// \brief A child instance the instance needs; the manager starts it after the instance's Update.
  struct ChildRequest
  {
    plAudioEventResourceHandle m_hEvent;
    ma_uint64 m_uiOriginFrame = 0; ///< engine frame the child's timeline starts at
    plUInt8 m_uiInstrument = 0;
    bool m_bLimited = false;       ///< the instrument does not loop: the child stops after m_Length
    plTime m_Length;               ///< zero: one pass of the child's timeline
    bool m_bWaiting = false;       ///< the event was still loading when the manager first tried to start it
    plTime m_WaitStart;            ///< manager time of that first try
  };

  /// \brief The deepest nesting the manager starts; deeper requests (e.g. an event that nests itself) are dropped.
  static constexpr plUInt8 MaxNestingDepth = 4;

  plHybridArray<ChildRequest, 1> m_ChildRequests;
  plUInt64 m_uiParentSerial = 0;          ///< serial of the parent instance; 0 for an instance the game started
  plUInt8 m_uiParentInstrument = 0;       ///< the instrument of the parent that started this instance
  plUInt8 m_uiNestingDepth = 0;           ///< 0 for an instance the game started
  plUInt32 m_uiLiveChildren = 0;          ///< set by the manager before every Update
  ma_uint64 m_uiRequestedOriginFrame = 0; ///< set by the manager before Start: the timeline origin of a child; 0 for none
  bool m_bRequestedLimit = false;         ///< set by the manager before Start: the child stops after m_RequestedLength
  plTime m_RequestedLength;               ///< zero: one pass of the timeline (see GetTimelineLength)

  /// \brief One pass of the event's timeline: where its last sound ends, from the start delays and the lengths of the
  /// instruments' longest entries. Entries whose length is not known yet (a clip still loading, a nested event) count
  /// as zero; zero when nothing is known.
  PL_NODISCARD plTime GetTimelineLength() const;

  /// \brief A child instance started by the instrument ended: a looping instrument triggers again.
  void OnChildFinished(plUInt32 uiInstrument, plRandom& ref_rng);

  ///@}

  /// \name Voice manager interface
  /// Voices are addressed by their index in [0, GetVoiceSlotCount()).
  ///@{

  PL_NODISCARD plUInt32 GetVoiceSlotCount() const { return m_Voices.GetCount(); }
  PL_NODISCARD bool IsVoiceActive(plUInt32 uiVoice) const { return m_Voices[uiVoice].m_bActive; }
  PL_NODISCARD bool IsVoiceVirtual(plUInt32 uiVoice) const { return m_Voices[uiVoice].m_bVirtual; }

  /// \brief Whether the voice's instrument loops.
  PL_NODISCARD bool IsVoiceLooping(plUInt32 uiVoice) const;

  /// \brief Whether the voice has not changed between real and virtual within the last uiDwellFrames frames.
  PL_NODISCARD bool CanVoiceFlip(plUInt32 uiVoice, ma_uint64 uiNow, ma_uint64 uiDwellFrames) const;

  /// \brief Estimated loudness of the instance at the listener, computed by Update.
  PL_NODISCARD float GetAudibility() const { return m_fAudibility; }
  PL_NODISCARD plUInt8 GetPriority() const;
  PL_NODISCARD ma_uint64 GetStartFrame() const { return m_uiStartFrame; }
  PL_NODISCARD ma_engine* GetEngine() const { return m_pEngine; }

  /// \brief Makes a real voice virtual; its chain fades out over VirtualFadeDuration and is torn down by a later Update.
  /// With plAudioEventVirtualMode::Kill the whole instance is stopped immediately instead.
  void VirtualizeVoice(plUInt32 uiVoice);

  /// \brief Makes a virtual voice real. Returns false when it stays virtual (clip not loaded, every stream taken,
  /// or the voice already ran out).
  bool RealizeVoice(plUInt32 uiVoice);

  /// \brief Stops a virtual voice of a non-looping instrument; the instrument is done.
  void KillVirtualVoice(plUInt32 uiVoice);

  ///@}

  /// \brief Appends a plAudioVoiceSnapshotEntry per active voice. Called under the resource manager mutex.
  void AppendSnapshotVoices(plDynamicArray<plAudioVoiceSnapshotEntry>& inout_voices) const;

private:
  static constexpr ma_uint64 UnknownFrame = ~static_cast<ma_uint64>(0);

  struct Voice
  {
    ma_decoder m_Decoder = {};
    /// \brief Clip source of a clip voice, one of the instance's m_ClipSources. Assigned on the first clip start of
    /// the slot and kept until the instance is reset; it stays at its heap address while miniaudio references it.
    plAudioClipVoiceSource* m_pClipSource = nullptr;
    plAudioClipResourceHandle m_hClip;       ///< the clip the voice plays; keeps the clip resource alive
    plAudioClipResource* m_pClip = nullptr;  ///< acquired from m_hClip while m_bClipSourceValid
    ma_data_source* m_pInner = nullptr;      ///< the source the voice reads: m_Decoder or m_pClipSource->m_pSource
    plMaVoiceDataSource m_Source = {};       ///< feeds m_Sound from m_pInner
    ma_sound m_Sound = {};
    plMaFilterNode m_OcclusionFilter;        ///< low pass, initialized with the engine's allocation callbacks while m_bOcclusionLpfValid
    ma_splitter_node m_EnvSplitter = {};     ///< initialized with the engine's allocation callbacks while m_bEnvSplitterValid
    ma_sound_group* m_EnvAux[MaxEnvironmentSends] = {}; ///< aux bus each wet output (splitter output 1 + i) is attached to; null while unattached
    ma_node* m_pSpatializerNode = nullptr;              ///< the active spatializer's node after the sound, or null (miniaudio pans)
    plAudioSpatializer* m_pSpatializer = nullptr;       ///< the spatializer that created m_pSpatializerNode
    double m_fVirtualCursor = 0.0;         ///< virtual voices: timeline position in source frames at m_uiVirtualCursorFrame
    ma_uint64 m_uiVirtualCursorFrame = 0;  ///< virtual voices: engine frame m_fVirtualCursor belongs to
    ma_uint64 m_uiStartFrame = 0;          ///< absolute engine frame at which the voice's timeline starts
    ma_uint64 m_uiLengthSourceFrames = 0;  ///< inner source length in source frames; 0 when unknown
    ma_uint64 m_uiLastFlipFrame = UnknownFrame; ///< engine frame of the last real/virtual change; UnknownFrame when there was none
    ma_uint64 m_uiTeardownFrame = 0;       ///< engine frame at which the virtualization fade-out ends; only valid when m_bTeardownPending
    ma_uint64 m_uiLoopStart = 0;           ///< loop start in source frames; only valid when m_bLoopPoints
    ma_uint64 m_uiLoopEnd = 0;             ///< loop end in source frames; only valid when m_bLoopPoints
    ma_uint32 m_uiSourceSampleRate = 0;    ///< inner source sample rate (clip header rate or decoder output rate)
    plUInt32 m_uiEntry = 0;                ///< playlist entry the voice plays
    float m_fBaseVolume = 1.0f;
    float m_fBasePitch = 1.0f;
    float m_fStartPitch = 1.0f;            ///< effective pitch when the voice was scheduled
    plUInt8 m_uiInstrument = 0;
    bool m_bActive = false;
    bool m_bVirtual = false;             ///< active without a decoding chain (see plAudioVoiceManager)
    bool m_bDecoderValid = false;
    bool m_bClipSourceValid = false;     ///< m_pClipSource is initialized and m_pClip is acquired
    bool m_bSourceValid = false;
    bool m_bSoundValid = false;          ///< m_Sound is initialized (it may not have started)
    bool m_bOcclusionLpfValid = false;
    bool m_bEnvSplitterValid = false;
    /// virtual, but the decoding chain still plays its fade-out; Update tears it down at m_uiTeardownFrame
    bool m_bTeardownPending = false;
    bool m_bNativeLoop = false;           ///< inner source loops by itself (gapless loop or loop region)
    bool m_bLoopPoints = false;           ///< the native loop repeats [m_uiLoopStart, m_uiLoopEnd) (clip loop points or loop region) instead of the whole source
  };

  enum class BuildResult : plUInt8
  {
    Success,
    NotLoaded,  ///< the clip is not loaded yet
    ClipFailed, ///< the clip cannot play
    Failed,
  };

  struct InstrumentState
  {
    ma_uint64 m_uiNextTriggerFrame = 0;    ///< absolute engine frame; only valid when m_bPendingTrigger
    ma_uint64 m_uiRetriggerGapFrames = 0;  ///< gap after the current voice; only valid when m_bGapRolled
    plUInt32 m_uiCursor = 0;               ///< sequential playlist cursor
    plUInt16 m_uiLastEntry = 0xFFFF;
    plUInt8 m_uiCurrentSlot = 0;           ///< voice slot of the most recently scheduled voice
    bool m_bPendingTrigger = true;
    bool m_bGapRolled = false;
    bool m_bDone = false;
    bool m_bLoadWaitLogged = false;  ///< the wait for the clip of the pending trigger was logged
    bool m_bClipMissingLogged = false; ///< a missing clip of the instrument was logged
    float m_fRangeGain = 1.0f;         ///< volume factor of the instrument's parameter range, set by ApplyModulation
    float m_fAutomationGain = 1.0f;    ///< volume factor of the instrument's volume automations, set by ApplyModulation
    plUInt8 m_uiSustainIndex = 0;      ///< sustain points at or before the instrument's start; it waits until all are released
    bool m_bChildPlaying = false;      ///< a nested event the instrument started still plays
  };

  /// \brief Whether the instrument waits for a key-off: a sustain point before its start was not released yet.
  PL_NODISCARD bool IsWaitingForKeyOff(plUInt32 uiInstrument) const { return m_Instruments[uiInstrument].m_uiSustainIndex > m_uiReleasedSustains; }

  /// \brief Queues the markers between the timeline position of the last call and the current one on the event data.
  void PassMarkers();

  /// \brief Whether the playhead has markers ahead of it: markers are part of the timeline, so the instance lives until
  /// it passed the last one, even when nothing sounds (e.g. while it holds at a sustain point).
  PL_NODISCARD bool HasMarkersAhead() const;

  /// \brief Base volume of the voice times the parameter range gain of its instrument.
  float GetVoiceVolume(const Voice& voice) const
  {
    const InstrumentState& state = m_Instruments[voice.m_uiInstrument];
    return voice.m_fBaseVolume * state.m_fRangeGain * state.m_fAutomationGain;
  }

  /// \brief Adds an entry about this instance to m_pEventLog, if set.
  void Log(plAudioLogEventType::Enum type, plStringView sDetail = plStringView()) const;

  static constexpr plUInt32 VoiceSlotsPerInstrument = 2;

  Voice& GetVoice(plUInt32 uiInstrument, plUInt32 uiSlot) { return m_Voices[uiInstrument * VoiceSlotsPerInstrument + uiSlot]; }

  void StartVoice(plUInt32 uiInstrument, plUInt32 uiSlot, plRandom& ref_rng, ma_uint64 uiStartFrame);

  /// \brief Builds the decoding chain of voice (m_uiInstrument, m_uiEntry, base volume and pitch set) and starts its
  /// sound, with the inner source sought to uiSourceCursor and its data starting at engine frame uiDataStartFrame.
  ///
  /// With bFreshStart the native loop of the voice is decided from the instrument, otherwise the stored loop info is
  /// applied. A uiStartFadeFrames above 0 fades the sound in from silence to fStartFadeVolume, from the engine frame at
  /// which its data begins (see plMaVoiceDataSource). Everything built is released again on failure.
  BuildResult BuildVoiceChain(Voice& voice, ma_uint64 uiDataStartFrame, ma_uint64 uiSourceCursor, bool bFreshStart, ma_uint64 uiStartFadeFrames, float fStartFadeVolume);

  /// \brief Decides m_bNativeLoop and the loop points of a voice whose length and sample rate are known.
  void DecideNativeLoop(Voice& voice, const plAudioClipResource* pClip);

  /// \brief Whether a new voice on uiSlot of uiInstrument starts real.
  bool AdmitVoice(plUInt32 uiInstrument, plUInt32 uiSlot, float fBaseVolume);

  /// \brief Whether the voice can get the stream its chain needs: true for voices that do not stream, that still hold
  /// their stream (fading out) or while the stream service has a free stream.
  bool HasStreamFor(const Voice& voice) const;

  /// \brief Audibility of a voice of the given base volume at the listener (see plAudioVoiceManager). 0 beyond the
  /// event's max distance.
  float ComputeAudibility(float fBaseVolume) const;

  /// \brief Starts the instrument's pending trigger on a free voice slot when its frame lies within uiHorizon.
  /// Returns true when a voice became active.
  bool StartPendingTrigger(plUInt32 uiInstrument, ma_uint64 uiHorizon, plRandom& ref_rng);
  void StopVoice(Voice& voice);

  /// \brief Tears down the decoding chain of a voice (sound, nodes, data source, decoder, clip source), also while its
  /// virtualization fade-out still plays, and keeps its timeline, clip handle and loop info.
  void ReleaseVoiceChain(Voice& voice);

  /// \brief Index of the next playlist entry to play, or plInvalidIndex for an empty playlist.
  plUInt32 PickPlaylistEntry(const plAudioEventInstrumentDesc& instrument, InstrumentState& state, plRandom& ref_rng);

  /// \brief Sets up voice.m_pInner from the clip hClip. Returns PL_FAILURE when the clip cannot play;
  /// out_bNotLoaded is set when it only is not loaded yet.
  plResult InitClipSource(Voice& voice, const plAudioClipResourceHandle& hClip, bool& out_bNotLoaded);
  void ApplyGlobalParameters();

  /// \brief Moves each parameter's value toward the value the game set, at the parameter's seek speed.
  void SeekParameters(plTime delta);

  /// \brief Sets parameter uiParam to fValue, clamped to its range; the value follows at the seek speed, or jumps.
  void SetParameterTarget(plUInt32 uiParam, float fValue);

  void ApplyModulation();

  /// \brief A wet output of the voices' splitter: the aux bus it feeds and at what linear level.
  struct AuxSend
  {
    ma_sound_group* m_pAux = nullptr;
    float m_fLevel = 0.0f;
  };

  /// \brief Adds the send level automations to the environment sends. A send to an aux bus already in the list adds its
  /// level; with no output left, it replaces the weakest send when it is louder.
  void AddAutomationSends(plHybridArray<AuxSend, MaxEnvironmentSends>& inout_sends);

  /// \brief The low-pass cutoff of the voices: the lower of the occlusion's and the filter automations'.
  float GetOcclusionCutoff() const;
  bool IsRegionLooping() const;

  /// \brief Converts a timeline duration to engine frames.
  ma_uint64 ToFrames(plTime duration) const;

  /// \brief Random retrigger gap of a looping instrument, in engine frames.
  ma_uint64 RollRetriggerGap(const plAudioEventInstrumentDesc& instrument, plRandom& ref_rng, double fMinSeconds) const;

  /// \brief Engine frame at which the voice runs out of data, or UnknownFrame for native loops and unknown lengths.
  ///
  /// Exact while the pitch is unchanged since scheduling and the voice's stream never starved; otherwise estimated from
  /// the published cursor (real voices) or the virtual cursor and the current pitch. Doppler pitch shifts applied inside
  /// miniaudio are not accounted for.
  ma_uint64 EstimateVoiceEndFrame(const Voice& voice, ma_uint64 uiNow) const;

  /// \brief Source frames the voice advances per engine frame at the current pitch.
  double GetSourceFramesPerEngineFrame(const Voice& voice) const;

  /// \brief Virtual voices: advances m_fVirtualCursor to uiNow at the current pitch.
  void AdvanceVirtualCursor(Voice& voice, ma_uint64 uiNow) const;

  /// \brief The distance to the closest listener, or the audition's distance for a global entity of the editor's
  /// audition. False for other global entities.
  bool GetListenerDistance(float& out_fDistance) const;

  /// \brief The gain of the distance curve (including its fade before the max distance) and the occlusion volume, for
  /// the closest listener. The distance part is 1 for a global entity without an audition distance or a 2D event.
  float ComputeSpatialGain() const;

  /// \brief The cone gain for the closest listener; 1 without a cone. miniaudio applies the cone to the sound itself,
  /// so this only feeds the audibility.
  float ComputeConeGain() const;

  /// \brief Hands the voice's placement to its spatializer node, if it has one.
  void UpdateSpatializer(Voice& ref_voice) const;

  plAudioEventResourceHandle m_hResource;            ///< keeps the event resource alive while m_pResource is set
  const plAudioEventResource* m_pResource = nullptr; ///< acquired until destruction or OnEventUnloading()
  plMiniAudioBusGraph* m_pBusGraph = nullptr;        ///< voices attach to its buses; its bus gains feed the audibility
  ma_engine* m_pEngine = nullptr;
  const plHashTable<plUInt64, float>* m_pGlobalParameters = nullptr;

  /// \brief Every clip source the instance allocated; the first m_uiClipSourcesAssigned are assigned to voices.
  plHybridArray<plUniquePtr<plAudioClipVoiceSource>, 4> m_ClipSources;
  plUInt32 m_uiClipSourcesAssigned = 0;

  plHybridArray<Voice, 4> m_Voices;           // VoiceSlotsPerInstrument per instrument
  plHybridArray<InstrumentState, 2> m_Instruments;
  plHybridArray<float, 4> m_ParameterValues;  // one per desc parameter
  plHybridArray<float, 4> m_ParameterTargets; // one per desc parameter: what the game set; the values seek toward them
  plHybridArray<plUInt64, 4> m_ParameterNameHashes; // one per desc parameter
  plHybridArray<float, 4> m_AutomationValues; // one per desc automation: the curve's value, set by ApplyModulation
  float m_fAutomationCutoff = 22000.0f;       ///< the lowest cutoff of the filter automations, set by ApplyModulation
  plUInt32 m_uiReleasedSustains = 0;          ///< sustain points released by KeyOff or passed by the start offset
  ma_uint64 m_uiStopFrame = 0;                ///< a limited nested event stops with its fade from this engine frame on; 0 for none
  double m_fMarkerPosition = 0.0;             ///< timeline position up to which PassMarkers queued the markers
  plHybridArray<bool, 4> m_ParameterLocal;    // true once SetParameter set the value

  ma_uint64 m_uiStartFrame = 0;               ///< engine frame at Start()
  ma_uint64 m_uiTimelineOriginFrame = 0;      ///< engine frame of timeline time zero for the current loop-region pass
  ma_uint32 m_uiSampleRate = 48000;           ///< engine sample rate
  plUInt32 m_uiRegionLoopInstrument = plInvalidIndex; ///< instrument that loops the region natively on its inner source

  /// \brief Generator of a seeded instance (plPlasmaAudioEventData::m_bSeeded); the others use the manager's.
  plRandom m_SeededRng;
  bool m_bSeeded = false;

  plTime m_Elapsed;
  bool m_bStopping = false;
  float m_fCurrentVolumeMultiplier = 1.0f;
  float m_fCurrentSpatialGain = 1.0f; ///< ComputeSpatialGain of the last update
  float m_fCurrentPitchMultiplier = 1.0f;
  float m_fAudibility = 1.0f;
  float m_fDistance = 0.0f;                   ///< entity to listener distance at the last audibility computation
  plUInt32 m_uiBus = 0;                       ///< mixer bus index of the event's output bus
};

/// \brief Owns all playing event instances of the PlasmaAudio backend: polyphony groups
/// (max instances, stealing, cooldown), per-entity lookup and the per-update tick.
///
/// Called from the audio thread. Voices read the memory of event and clip resources, and the resource manager frees
/// that memory synchronously when it unloads or reloads a resource, on whatever thread does so. The manager therefore
/// works on its instances only while it holds its own mutex, and it handles plResourceEvent::Type::
/// ResourceContentUnloading of clip and event resources, which the resource manager sends under its mutex right before
/// the data is freed: the handler takes the manager's mutex and stops every voice reading the resource (ma_sound_uninit
/// returns once the device thread no longer reads the sound).
///
/// Lock order: the resource manager's mutex may be held when the manager's mutex is taken (the unload handler), never
/// the other way round. While holding its mutex the manager therefore never loads, reloads or creates a resource and
/// never waits for one; such requests are collected (plPlasmaAudioDeferredLoads) and issued after the mutex is released.
/// Other threads never wait for the audio thread's update to take a resource lock.
///
/// An event that is not loaded when it is triggered starts asynchronously: its load is requested and the activation
/// waits until the event (and, for a legacy sound, the sound resource) is loaded, then starts with the next Update. A
/// one-shot that waits longer than MaxPendingStartTime is dropped; a looping event waits until it is stopped.
///
/// Finished instances are reset and kept in a pool (up to MaxPooledInstances) for later starts. A reset instance
/// holds no miniaudio objects, so the pool outlives device re-inits.
class PL_PLASMAAUDIOPLUGIN_DLL plPlasmaAudioEventManager
{
  PL_DISALLOW_COPY_AND_ASSIGN(plPlasmaAudioEventManager);

public:
  plPlasmaAudioEventManager();
  ~plPlasmaAudioEventManager();

  /// \brief How long a one-shot waits for its event to load before it is dropped.
  static constexpr plTime MaxPendingStartTime = plTime::MakeFromSeconds(1.0);

  /// \brief Starts the event, or queues the start until the event is loaded (see the class description).
  plResult ActivateEvent(plStringView sEventResourceId, plPlasmaAudioEntityData* pEntity, plPlasmaAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine);

  /// \brief Plays a legacy plMiniAudioSoundResource as an audio event.
  ///
  /// The first activation per sound resource builds an in-memory plAudioEventResource ("Legacy:<resource id>")
  /// with one instrument whose playlist holds every variant of the sound, and caches it. After that it runs
  /// the regular ActivateEvent path. A sound resource that is not loaded yet starts asynchronously like an event.
  plResult ActivateLegacySound(const plMiniAudioSoundResourceHandle& hSound, plPlasmaAudioEntityData* pEntity, plPlasmaAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine);

  /// \brief Stops the instance bound to the given event data. Returns PL_FAILURE if none is playing.
  plResult StopEvent(const plPlasmaAudioEventData* pEventData, bool bImmediate);

  /// \brief Unbinds every instance from pEventData, which is about to be deleted or reused.
  ///
  /// The instances are stopped with their authored fade-out and finish on their own; they no longer count as
  /// playing for that event data.
  void DetachEventData(plPlasmaAudioEventData* pEventData);

  void StopAllOnEntity(const plPlasmaAudioEntityData* pEntity, bool bImmediate);
  void StopAll(bool bImmediate);

  /// \brief Fills the emitters, listeners and voice counts the Audio view mode draws.
  void GetDebugSnapshot(plAudioDebugSnapshot& out_snapshot) const;

  /// \brief Detaches every instance playing on the bus graph from its buses, before the graph is rebuilt in place.
  void DetachFromBusGraph(const plMiniAudioBusGraph* pBusGraph);

  /// \brief Attaches every instance playing on the bus graph to the rebuilt graph's buses again.
  void ReattachToBusGraph(const plMiniAudioBusGraph* pBusGraph);

  void SetEventParameter(const plPlasmaAudioEntityData* pEntity, plStringView sParamName, float fValue);

  /// \brief Drops the instance-local value of the parameter on every instance of the entity (see plPlasmaAudioEventInstance::ResetParameter).
  void ResetEventParameter(const plPlasmaAudioEntityData* pEntity, plStringView sParamName);

  /// \brief Releases the next sustain point of every instance of the entity (see plPlasmaAudioEventInstance::KeyOff).
  void KeyOff(const plPlasmaAudioEntityData* pEntity);

  /// \brief Moves the markers the instances of the event data passed and nobody took yet into out_markers.
  void TakeMarkers(plPlasmaAudioEventData* pEventData, plDynamicArray<plString>& out_markers);

  /// \brief The sustain points of the instance of the event data not released yet, and whether its playhead waits at
  /// one. Both 0 / false when nothing plays for the event data.
  void GetSustainState(const plPlasmaAudioEventData* pEventData, plUInt32& out_uiSustainsLeft, bool& out_bHolding) const;

  /// \brief Whether an instance is bound to the event data, or its start is still waiting for the event to load.
  PL_NODISCARD bool IsEventPlaying(const plPlasmaAudioEventData* pEventData) const;

  /// \brief State of the instance started for the event data: whether it waits to start (its event or mixer is still
  /// loading), its timeline position in seconds, and per instrument whether it sounds and its parameter range gain.
  /// Returns false when nothing plays for the event data.
  bool GetEventState(const plPlasmaAudioEventData* pEventData, bool& out_bWaiting, double& out_fPosition, plDynamicArray<bool>& out_sounding, plDynamicArray<float>& out_rangeGains) const;

  /// \brief Starts the activations whose events finished loading, then updates every instance and the voice manager.
  /// listeners holds the position of every listener that is heard.
  void Update(plTime delta, plArrayPtr<const plVec3> listeners);

  /// \brief Update() with a single listener.
  void Update(plTime delta, const plVec3& vListenerPos) { Update(delta, plArrayPtr<const plVec3>(&vListenerPos, 1)); }

  /// \brief Activations waiting for their event to load.
  PL_NODISCARD plUInt32 GetPendingActivationCount() const;

  PL_NODISCARD plUInt32 GetInstanceCount() const { return m_Instances.GetCount(); }

  /// \brief Number of real voices (same as GetRealVoiceCount()).
  PL_NODISCARD plUInt32 GetActiveVoiceCount() const { return GetRealVoiceCount(); }

  /// \brief Voices that are decoded and mixed, including voices scheduled inside the look-ahead that are not audible yet.
  PL_NODISCARD plUInt32 GetRealVoiceCount() const;

  /// \brief Voices that play without a decoding chain (see plAudioVoiceManager).
  PL_NODISCARD plUInt32 GetVirtualVoiceCount() const;

  /// \brief Real and virtual voices of the instance bound to pEventData; both 0 when none is.
  void GetEventVoiceCounts(const plPlasmaAudioEventData* pEventData, plUInt32& out_uiReal, plUInt32& out_uiVirtual) const;

  /// \brief Number of voice changes between real and virtual so far.
  PL_NODISCARD plUInt64 GetVoiceFlipCount() const;

  /// \brief Fills the voice counts, budgets, instance count and one entry per active voice of out_snapshot. The
  /// middleware-level fields (DSP load, update time, streams, buses, memory, event log) are left untouched.
  void FillSnapshot(plAudioProfilingSnapshot& out_snapshot) const;

  /// \brief The log of instance starts, stops and voice changes. The middleware adds its device, mixer, bank and
  /// language events to it as well.
  PL_NODISCARD plAudioEventLog& GetEventLog() { return m_EventLog; }
  PL_NODISCARD const plAudioEventLog& GetEventLog() const { return m_EventLog; }

  /// \brief Replaces the global parameter snapshot (name hash -> value) that instances read in Update.
  void SetGlobalParameters(const plHashTable<plUInt64, float>& globals) { m_GlobalParameters = globals; }

  /// \brief The stream service that instances started from now on hand to streamed clips; null disables streamed clips.
  void SetStreamService(plAudioStreamService* pStreams) { m_pStreamService = pStreams; }

  /// \brief Overrides the scheduling look-ahead in engine frames. 0 selects the automatic value.
  ///
  /// Triggers are committed with their exact start frame once they fall within now + look-ahead.
  /// Offline renders (engines without a device) set this to at least the number of frames rendered
  /// per Update so that every trigger is scheduled before the engine reaches its frame.
  static void SetLookAheadFrames(ma_uint64 uiFrames);

  /// \brief The effective look-ahead: the override when set, otherwise two device periods
  /// (2 * 480 frames for engines without a device).
  static ma_uint64 GetLookAheadFrames(ma_engine* pEngine);

private:
  struct EventGroup
  {
    plTime m_LastStartTime;
    bool m_bHasStarted = false;
  };

  /// \brief Stops the voices of clip and event resources that are about to be unloaded. Runs on the unloading thread
  /// with the resource manager mutex held.
  void OnResourceEvent(const plResourceEvent& e);

  /// \brief Starts an instance of a loaded event. Takes the manager's mutex; the event must be loaded. With pParent and
  /// pRequest, the instance is a nested event of pParent.
  plResult ActivateEventInternal(const plAudioEventResourceHandle& hEvent, plStringView sEventResourceId, plPlasmaAudioEntityData* pEntity, plPlasmaAudioEventData* pEventData, plMiniAudioBusGraph* pBusGraph, ma_engine* pEngine, const plPlasmaAudioEventInstance* pParent = nullptr, const plPlasmaAudioEventInstance::ChildRequest* pRequest = nullptr);

  /// \brief The generated event of a loaded legacy sound (creating it on first use). Takes no manager lock: it may
  /// create a resource.
  plAudioEventResourceHandle GetLegacyEvent(const plMiniAudioSoundResourceHandle& hSound);

  /// \brief An activation whose event (or legacy sound) was not loaded yet.
  struct PendingActivation
  {
    plAudioEventResourceHandle m_hEvent;            ///< invalid for a legacy sound until it is loaded
    plMiniAudioSoundResourceHandle m_hLegacySound;  ///< set for legacy sounds
    plString m_sEventId;
    plPlasmaAudioEntityData* m_pEntity = nullptr;
    plPlasmaAudioEventData* m_pEventData = nullptr;
    plMiniAudioBusGraph* m_pBusGraph = nullptr;
    ma_engine* m_pEngine = nullptr;
    plTime m_RequestTime;
    bool m_bLooping = false; ///< the event data asked for a looping start: it waits however long the load takes
  };

  /// \brief Queues an activation of a resource that is still loading. Takes the manager's mutex.
  void QueuePendingActivation(PendingActivation&& activation);

  /// \brief Starts the queued activations whose resources finished loading and drops the expired ones.
  void ProcessPendingActivations();

  /// \brief Issues the clip loads the instances asked for. Must be called without the manager's mutex held.
  void FlushDeferredLoads();

  /// \brief Updates every instance and the voice manager. Takes the manager's mutex.
  void UpdateInstances(plTime delta);

  /// \brief Counts the live children of every instance and stops the children whose parent stops or is gone. Fills
  /// out_parents (serial -> instance) when any instance is nested. Under the mutex.
  void LinkNestedInstances(plHashTable<plUInt64, plPlasmaAudioEventInstance*>& out_parents);

  /// \brief Starts the nested events the instances asked for in their updates. Under the mutex.
  void StartNestedInstances();

  /// \brief Closest of the last known listener positions to the entity.
  plVec3 GetClosestListener(const plPlasmaAudioEntityData* pEntity) const;

  /// \brief Guards the instances, pools, pending activations and deferred loads (see the class description).
  mutable plMutex m_Mutex;

  plDynamicArray<PendingActivation> m_PendingActivations;
  plPlasmaAudioDeferredLoads m_DeferredLoads;
  plHybridArray<plVec3, 4> m_Listeners;

  /// \brief Maximum number of finished instances kept for reuse.
  static constexpr plUInt32 MaxPooledInstances = 64;

  /// \brief A reset instance from the pool, or a new one when the pool is empty.
  plUniquePtr<plPlasmaAudioEventInstance> AcquireInstance();

  /// \brief Resets the instance and keeps it in the pool, or deletes it when the pool is full.
  void RecycleInstance(plUniquePtr<plPlasmaAudioEventInstance>&& pInstance);

  plDynamicArray<plUniquePtr<plPlasmaAudioEventInstance>> m_Instances;
  plDynamicArray<plUniquePtr<plPlasmaAudioEventInstance>> m_FreeInstances; ///< reset instances for reuse
  plHashTable<plUInt64, EventGroup> m_EventGroups;
  plHashTable<plString, plAudioEventResourceHandle> m_LegacyEvents; ///< legacy sound resource id -> generated event
  plHashTable<plUInt64, float> m_GlobalParameters;                  ///< snapshot of the middleware's global parameters
  plAudioStreamService* m_pStreamService = nullptr;
  plAudioVoiceManager m_VoiceManager;
  plAudioEventLog m_EventLog;
  plUInt64 m_uiNextInstanceSerial = 1; // 0 is no instance (see plPlasmaAudioEventInstance::m_uiParentSerial)
  plRandom m_Rng;
  plTime m_Now;
  bool m_bRngInitialized = false;
  plEventSubscriptionID m_ResourceEventSubscription = 0;
};
