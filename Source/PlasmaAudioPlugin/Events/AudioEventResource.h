#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Core/ResourceManager/Resource.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Vec2.h>
#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Tracks/Curve1D.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResource.h>

using plAudioEventResourceHandle = plTypedResourceHandle<class plAudioEventResource>;
using plDataBuffer = plDynamicArray<plUInt8>;

/// \brief How a spatialized event's gain falls between its min and max distance.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioAttenuationCurve
{
  using StorageType = plUInt8;

  enum Enum : StorageType
  {
    Inverse,     ///< min / (min + rolloff * (d - min)), the physical falloff
    Linear,      ///< straight from 1 at min to 0 at max
    Logarithmic, ///< 1 - log(d / min) / log(max / min): loud for longer, then quickly quiet
    Custom,      ///< the event's attenuation points
    None,        ///< no distance gain; only the fade before the max distance

    Default = Inverse
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PLASMAAUDIOPLUGIN_DLL, plAudioAttenuationCurve);

/// \brief How a multi-sound instrument advances through its playlist.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEventPlaylistMode
{
  using StorageType = plUInt8;

  enum Enum : StorageType
  {
    Random = 0,     ///< weighted random pick, repeats allowed
    Shuffle = 1,    ///< weighted random pick, never repeats the previous entry
    Sequential = 2, ///< round-robin through the playlist

    Default = Random
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PLASMAAUDIOPLUGIN_DLL, plAudioEventPlaylistMode);

/// \brief Which playing instance gets killed when an event exceeds its max instance count.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEventStealingMode
{
  using StorageType = plUInt8;

  enum Enum : StorageType
  {
    None = 0,     ///< the new instance is simply not started
    Oldest = 1,   ///< the longest-playing instance is stopped
    Quietest = 2, ///< the instance with the lowest effective volume is stopped

    Default = Oldest
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PLASMAAUDIOPLUGIN_DLL, plAudioEventStealingMode);

/// \brief What happens to a voice of the event when the voice manager takes its real voice away (voice budget
/// exceeded, or started beyond the max distance).
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEventVirtualMode
{
  using StorageType = plUInt8;

  enum Enum : StorageType
  {
    Resume = 0,  ///< the voice keeps its timeline while virtual and continues at the matching position when it becomes real again
    Restart = 1, ///< the voice starts again from the beginning of its sample when it becomes real again
    Kill = 2,    ///< the event instance is stopped instead of going virtual

    Default = Resume
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PLASMAAUDIOPLUGIN_DLL, plAudioEventVirtualMode);

/// \brief One entry of an instrument playlist.
///
/// An entry plays the nested event m_sEvent when set, else the audio clip m_sClip when set, otherwise the embedded sample
/// m_uiSample (or silence).
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEventPlaylistEntryDesc
{
  /// \brief Index into plAudioEventDesc::m_SampleNames, or 0xFFFF for a silence entry. Unused when m_sClip is set.
  plUInt16 m_uiSample = 0xFFFF;
  float m_fWeight = 1.0f;

  /// \brief Resource id of the plAudioClipResource this entry plays. Empty = the embedded sample m_uiSample.
  plString m_sClip;

  /// \brief Resource id of a plAudioEventResource this entry plays as a nested event, on the same entity. It gets the
  /// parent's parameter values by name, and stops with the parent. Empty = not nested.
  plString m_sEvent;

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream, plTypeVersion uiVersion);
};

constexpr plUInt16 plAudioEventSilenceSample = 0xFFFF;
constexpr plUInt8 plAudioEventNoParameter = 0xFF;

/// \brief One instrument of an audio event: a (possibly single-entry) playlist with
/// randomization, placed on the event timeline at a start delay.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEventInstrumentDesc
{
  plString m_sName;
  plTime m_StartDelay = plTime::MakeZero();
  plAudioEventPlaylistMode::Enum m_ePlaylistMode = plAudioEventPlaylistMode::Default;

  /// \brief When true, the instrument re-triggers its playlist every time the current entry
  /// finishes, for as long as the event instance is active (looping ambiences, scatterers).
  bool m_bLoop = false;

  /// \brief A muted (disabled) instrument is skipped entirely, in the editor audition and at runtime.
  bool m_bMute = false;

  /// \brief Optional switch filter ("Surface/Grass"). When set, this instrument only plays while
  /// the entity's switch state of that group matches. Empty means "always play".
  plString m_sSwitchState;

  /// \brief Extra randomized delay between loop re-triggers (scatterer behavior). Zero = gapless.
  plTime m_MinRetriggerDelay = plTime::MakeZero();
  plTime m_MaxRetriggerDelay = plTime::MakeZero();

  float m_fVolumeDb = 0.0f;
  float m_fVolumeRandDb = 0.0f;       ///< +/- random range applied per trigger
  float m_fPitchSemitones = 0.0f;
  float m_fPitchRandSemitones = 0.0f; ///< +/- random range applied per trigger

  /// \brief Optional parameter range: the instrument plays at full volume while the parameter m_uiRangeParameter
  /// (index into plAudioEventDesc::m_Parameters, plAudioEventNoParameter = always) is inside [m_fRangeMin, m_fRangeMax],
  /// fades linearly to silence over m_fRangeFade outside it, and is silent beyond.
  plUInt8 m_uiRangeParameter = plAudioEventNoParameter;
  float m_fRangeMin = 0.0f;
  float m_fRangeMax = 1.0f;
  float m_fRangeFade = 0.0f;

  /// \brief How long a nested event of a non-looping instrument plays before it is stopped with its stop fade; zero
  /// plays one pass of the nested event's timeline. A looping instrument's nested event plays until the event stops.
  plTime m_NestedLength = plTime::MakeZero();

  /// \brief Volume factor of the parameter range at a parameter value.
  PL_NODISCARD float GetRangeGain(float fValue) const;

  plHybridArray<plAudioEventPlaylistEntryDesc, 4> m_Playlist;

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream, plTypeVersion uiVersion);
};

/// \brief A named, ranged event parameter. The parameter named "Distance" is built-in and fed
/// with the entity-to-listener distance automatically.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEventParameterDesc
{
  plString m_sName;
  float m_fMin = 0.0f;
  float m_fMax = 1.0f;
  float m_fDefault = 0.0f;

  /// \brief How fast the value follows what the game sets, in parameter units per second. 0 jumps.
  float m_fSeekSpeed = 0.0f;

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// \brief What an automation curve modulates.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEventAutomationTarget
{
  using StorageType = plUInt8;

  enum Enum : StorageType
  {
    EventVolume = 0,      ///< curve value = linear volume multiplier
    EventPitch = 1,       ///< curve value = pitch factor multiplier
    InstrumentVolume = 2, ///< curve value = linear volume multiplier of the instrument m_uiInstrument
    FilterCutoff = 3,     ///< curve value = low-pass cutoff in Hz (the lower of it and the occlusion cutoff applies)
    SendLevel = 4,        ///< curve value = linear level of a send to the aux bus m_sBus

    Default = EventVolume
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PLASMAAUDIOPLUGIN_DLL, plAudioEventAutomationTarget);

/// \brief A named point on an event's timeline.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEventMarkerDesc
{
  plString m_sName;
  plTime m_Time = plTime::MakeZero();
};

/// \brief A curve driven by an event parameter, modulating a target property of the instance.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEventAutomationDesc
{
  plUInt8 m_uiParameter = 0; ///< index into plAudioEventDesc::m_Parameters
  plAudioEventAutomationTarget::Enum m_eTarget = plAudioEventAutomationTarget::Default;
  plCurve1D m_Curve; ///< x = parameter value (in parameter range), y = the target's value
  plUInt8 m_uiInstrument = 0; ///< InstrumentVolume: index into plAudioEventDesc::m_Instruments
  plString m_sBus;            ///< SendLevel: name of the aux bus

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// \brief The full serialized description of an audio event.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEventDesc
{
  plString m_sOutputBus; ///< mixer bus name; empty = master

  // polyphony / lifetime macros
  plUInt8 m_uiMaxInstances = 8;
  plAudioEventStealingMode::Enum m_eStealing = plAudioEventStealingMode::Default;

  /// \brief Voice priority: when the real voice budget is exceeded, voices of higher priority stay real before any
  /// audibility comparison.
  plUInt8 m_uiPriority = 128;
  plAudioEventVirtualMode::Enum m_eVirtualMode = plAudioEventVirtualMode::Default;

  plTime m_Cooldown = plTime::MakeZero(); ///< minimum time between two starts of this event
  plTime m_FadeIn = plTime::MakeZero();   ///< voices fade in over this time when the event starts
  plTime m_StopFadeOut = plTime::MakeFromSeconds(0.1);

  /// \brief When set, a looping event repeats the [m_LoopStart, m_LoopEnd] window of its timeline
  /// instead of replaying from zero.
  bool m_bLoopRegion = false;
  plTime m_LoopStart = plTime::MakeZero();
  plTime m_LoopEnd = plTime::MakeFromSeconds(1.0);

  // spatializer
  bool m_bSpatialize = true;
  float m_fMinDistance = 1.0f;
  float m_fMaxDistance = 100.0f;
  float m_fRolloff = 1.0f; ///< Inverse curve only: how fast the gain falls past the min distance
  float m_fDopplerFactor = 0.0f;

  /// \brief The gain over distance between the min and max distance (see GetDistanceGain).
  plAudioAttenuationCurve::Enum m_eAttenuationCurve = plAudioAttenuationCurve::Default;

  /// \brief Custom curve: (distance between min and max as 0..1, gain 0..1) points with increasing x. The gain is 1 at
  /// x = 0 and follows straight lines between the points; past the last point it keeps the last gain.
  plHybridArray<plVec2, 6> m_AttenuationPoints;

  /// \brief Sound cone around the emitter's forward direction, in degrees: full volume inside the inner angle, the outer
  /// gain outside the outer angle, blended between. 360 for both means no cone.
  float m_fConeInnerAngle = 360.0f;
  float m_fConeOuterAngle = 360.0f;
  float m_fConeOuterGainDb = -12.0f;

  /// \brief How wide the source sounds, in degrees: 0 pans as a point, 360 is heard from all around.
  float m_fSpread = 0.0f;

  /// \brief What full occlusion does: the low-pass cutoff it sweeps down to and the volume change.
  float m_fOcclusionCutoffHz = 250.0f;
  float m_fOcclusionVolumeDb = -6.0f;

  /// \brief The distance gain of a spatialized event (1 for a 2D one): 1 up to the min distance, then the attenuation
  /// curve, which also fades to silence over the last 15 % before the max distance (no hard cut).
  PL_NODISCARD float GetDistanceGain(float fDistance) const;

  /// \brief The cone gain for a listener at the angle (degrees) from the emitter's forward direction.
  PL_NODISCARD float GetConeGain(float fAngleDegrees) const;

  /// \brief The occlusion low-pass cutoff and the volume factor at an occlusion amount (0..1).
  PL_NODISCARD float GetOcclusionCutoff(float fOcclusion) const;
  PL_NODISCARD float GetOcclusionGain(float fOcclusion) const;

  plDynamicArray<plAudioEventInstrumentDesc> m_Instruments;
  plDynamicArray<plAudioEventParameterDesc> m_Parameters;
  plDynamicArray<plAudioEventAutomationDesc> m_Automations;

  /// \brief Named points on the timeline; the game object of the entity gets a message when the playhead passes one.
  plDynamicArray<plAudioEventMarkerDesc> m_Markers;

  /// \brief Timeline positions where the playhead waits until the game sends key-off (in time order). Instruments that
  /// start later wait with it; those already playing go on.
  plDynamicArray<plTime> m_SustainPoints;

  /// \brief Original file names of the embedded samples (debug / editor roundtrip).
  plDynamicArray<plString> m_SampleNames;

  plResult Save(plStreamWriter& inout_stream) const;
  plResult Load(plStreamReader& inout_stream);

  plUInt32 FindParameterByName(plStringView sName) const;
};

struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEventResourceDescriptor
{
  plAudioEventDesc m_Desc;
  plDynamicArray<plDataBuffer> m_SampleData;
};

/// \brief Runtime resource for an audio event (compiled from a .plAudioEventAsset).
///
/// Holds the event description, the embedded encoded sample payloads (wav/ogg bytes, decoded by
/// the backend at voice start) and a handle per playlist entry that references an audio clip.
/// The referenced clips are preloaded when the event is loaded or created. Activating an ATL
/// trigger whose name references this resource plays the event through the active middleware.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioEventResource : public plResource
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventResource, plResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plAudioEventResource);
  PL_RESOURCE_DECLARE_CREATEABLE(plAudioEventResource, plAudioEventResourceDescriptor);

public:
  plAudioEventResource();
  ~plAudioEventResource();

  const plAudioEventDesc& GetDescriptor() const { return m_Desc; }

  plUInt32 GetNumSamples() const { return m_SampleData.GetCount(); }
  const plDataBuffer& GetSampleData(plUInt32 uiIndex) const { return m_SampleData[uiIndex]; }

  /// \brief Length in frames and sample rate of the decoded embedded sample uiIndex; both 0 when it cannot be decoded.
  ///
  /// The first query per sample probes the sample with a decoder and caches the result. Callers hold the resource
  /// manager mutex (the event manager's contract), which serializes the cache.
  void GetSampleLength(plUInt32 uiIndex, plUInt64& out_uiFrames, plUInt32& out_uiSampleRate) const;

  /// \brief The clip of playlist entry uiEntry of instrument uiInstrument; an invalid handle when the entry has no clip.
  const plAudioClipResourceHandle& GetClip(plUInt32 uiInstrument, plUInt32 uiEntry) const;

  /// \brief The nested event of playlist entry uiEntry of instrument uiInstrument; an invalid handle when the entry
  /// does not play an event.
  plAudioEventResourceHandle GetNestedEvent(plUInt32 uiInstrument, plUInt32 uiEntry) const;

  /// \brief Replaces out_events with an entry per loaded event resource (name and embedded sample bytes). Any thread.
  ///
  /// Events are listed from their own registry of loaded events, not through the resource manager: an event enters it
  /// when its content finished loading and leaves it before its data is cleared or it is destroyed.
  static void GatherLoadedEvents(plDynamicArray<plAudioEventMemorySnapshotEntry>& out_events);

private:
  /// Adds the event to the registry of loaded events.
  void LinkLoaded();

  /// Removes the event from the registry of loaded events.
  void UnlinkLoaded();

  /// registry of loaded events: an intrusive list, guarded by s_LoadedMutex
  static plMutex s_LoadedMutex;
  static plAudioEventResource* s_pFirstLoaded;
  plAudioEventResource* m_pPrevLoaded = nullptr;
  plAudioEventResource* m_pNextLoaded = nullptr;
  plUInt64 m_uiSampleBytes = 0; ///< bytes of the embedded samples, set when the event enters the registry
  bool m_bLinkedLoaded = false;

  virtual plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  virtual plResourceLoadDesc UpdateContent(plStreamReader* pStream) override;
  virtual void UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage) override;

  /// \brief Fills m_Clips, m_NestedEvents and m_InstrumentClipOffsets from m_Desc and preloads what they reference.
  void SetupClips();

  plAudioEventDesc m_Desc;
  plDynamicArray<plDataBuffer> m_SampleData;

  /// \brief One handle per playlist entry of every instrument, instrument major; invalid for entries without a clip.
  plDynamicArray<plAudioClipResourceHandle> m_Clips;
  plDynamicArray<plAudioEventResourceHandle> m_NestedEvents; ///< like m_Clips; invalid for entries without an event
  plDynamicArray<plUInt32> m_InstrumentClipOffsets;          ///< index into m_Clips of the first entry of each instrument

  struct SampleLength
  {
    plUInt64 m_uiFrames = 0;
    plUInt32 m_uiSampleRate = 0;
    bool m_bProbed = false;
  };

  mutable plDynamicArray<SampleLength> m_SampleLengths; ///< per embedded sample, filled by GetSampleLength
};
