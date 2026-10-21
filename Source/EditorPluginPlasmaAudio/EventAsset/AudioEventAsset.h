#pragma once

#include <PlasmaAudioPlugin/Events/AudioEventResource.h>
#include <EditorFramework/Assets/AssetDocumentGenerator.h>
#include <EnginePluginPlasmaAudio/Audition/AudioAuditionMessages.h>
#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>

/// \brief One entry of an instrument playlist.
///
/// Plays the Audio Event asset m_sEvent when set (a nested event), else the Audio Clip asset m_sClip. Otherwise m_sFile
/// is a sound file embedded into the event, and an entry with none of them is a weighted silence entry.
class plAudioEventPlaylistEntry : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventPlaylistEntry, plReflectedClass);

public:
  plString m_sEvent; ///< asset GUID of an Audio Event played as a nested event
  plString m_sClip;  ///< asset GUID of an Audio Clip
  plString m_sFile;  ///< embedded sound file, used when m_sClip is empty
  float m_fWeight = 1.0f;
};

/// \brief One instrument of the event: a playlist with randomization, placed at a start delay.
class plAudioEventInstrument : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventInstrument, plReflectedClass);

public:
  plString m_sName;
  float m_fStartDelay = 0.0f; // seconds
  plEnum<plAudioEventPlaylistMode> m_ePlaylistMode;
  bool m_bLoop = false;
  bool m_bMute = false;      // disabled channel: skipped in audition and at runtime
  plString m_sSwitchState;   // optional "Group/State" filter, e.g. "Surface/Grass"
  float m_fMinRetriggerDelay = 0.0f; // seconds, loop/scatterer gap
  float m_fMaxRetriggerDelay = 0.0f;
  float m_fVolumeDb = 0.0f;
  float m_fVolumeRandDb = 0.0f;
  float m_fPitchSemitones = 0.0f;
  float m_fPitchRandSemitones = 0.0f;

  // optional parameter range: full volume while the parameter is in [min, max], fading to silence over 'fade' outside
  plString m_sRangeParameter; // empty = always plays
  float m_fRangeMin = 0.0f;
  float m_fRangeMax = 1.0f;
  float m_fRangeFade = 0.0f;

  float m_fNestedLength = 0.0f; // seconds a nested event plays when the instrument does not loop; 0 = one pass of it

  plDynamicArray<plAudioEventPlaylistEntry> m_Playlist;
};

/// \brief A named, ranged event parameter ("Distance" is built-in and fed automatically). The window shows each as a
/// parameter sheet.
class plAudioEventParameter : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventParameter, plReflectedClass);

public:
  plString m_sName;
  float m_fMin = 0.0f;
  float m_fMax = 1.0f;
  float m_fDefault = 0.0f;
  float m_fSeekSpeed = 0.0f; ///< units per second the value moves toward what the game set; 0 jumps
};

/// \brief A named point on the timeline; the game object of the entity gets a plMsgAudioMarker when the playhead passes it.
class plAudioEventMarker : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventMarker, plReflectedClass);

public:
  plString m_sName;
  float m_fTime = 0.0f; // seconds
};

/// \brief A point where the playhead waits until the game sends key-off.
class plAudioEventSustainPoint : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventSustainPoint, plReflectedClass);

public:
  float m_fTime = 0.0f; // seconds
};

/// \brief One control point of an automation curve (x = parameter value, y = multiplier).
class plAudioEventCurvePoint : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventCurvePoint, plReflectedClass);

public:
  float m_fX = 0.0f;
  float m_fY = 1.0f;
};

/// \brief A parameter-driven automation curve modulating the event's volume or pitch, an instrument's volume, a
/// low-pass cutoff or a send to an aux bus.
class plAudioEventAutomation : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventAutomation, plReflectedClass);

public:
  plString m_sParameter;
  plEnum<plAudioEventAutomationTarget> m_eTarget;
  plString m_sInstrument; ///< InstrumentVolume: the name of the instrument
  plString m_sBus;        ///< SendLevel: the aux bus
  plDynamicArray<plAudioEventCurvePoint> m_Points;
};

/// \brief Names this event defines, stored in the asset's document info so the audio components
/// can offer RTPC / switch-state dropdowns without the event document being open.
class plAudioEventAssetMetaInfo : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventAssetMetaInfo, plReflectedClass);

public:
  plDynamicArray<plString> m_Parameters;
  plDynamicArray<plString> m_SwitchStates;

  /// \brief One pass of the event's timeline in seconds: where its last sound ends (0 when no length is known), and
  /// whether an instrument loops past it.
  float m_fLength = 0.0f;
  bool m_bLoops = false;

  /// \brief The meta info of the event asset with that GUID. Returns false for an unknown asset.
  static bool Find(plStringView sAssetGuid, plAudioEventAssetMetaInfo& out_info);
};

class plAudioEventAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventAssetProperties, plReflectedClass);

public:
  static void PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);

  // routing & polyphony
  plString m_sOutputBus;
  plUInt8 m_uiMaxInstances = 8;
  plEnum<plAudioEventStealingMode> m_eStealing;
  plUInt8 m_uiPriority = 128;
  plEnum<plAudioEventVirtualMode> m_eVirtualMode;

  // lifetime
  float m_fCooldown = 0.0f;    // seconds
  float m_fFadeIn = 0.0f;      // seconds
  float m_fStopFadeOut = 0.1f; // seconds
  bool m_bLoopRegion = false;
  float m_fLoopStart = 0.0f; // seconds
  float m_fLoopEnd = 1.0f;   // seconds

  // spatializer
  bool m_bSpatialize = true;
  plString m_sAttenuation; ///< a shared Audio Attenuation asset; replaces the curve and distances below
  plEnum<plAudioAttenuationCurve> m_eCurve;
  float m_fMinDistance = 1.0f;
  float m_fMaxDistance = 100.0f;
  float m_fRolloff = 1.0f;
  plDynamicArray<plAudioEventCurvePoint> m_CurvePoints; ///< Custom curve: x = 0..1 between min and max, y = gain
  float m_fConeInnerAngle = 360.0f;
  float m_fConeOuterAngle = 360.0f;
  float m_fConeOuterVolumeDb = -12.0f;
  float m_fSpread = 0.0f;
  float m_fDopplerFactor = 0.0f;

  // occlusion
  float m_fOcclusionCutoff = 250.0f; ///< Hz at full occlusion
  float m_fOcclusionVolumeDb = -6.0f;

  plDynamicArray<plAudioEventInstrument> m_Instruments;
  plDynamicArray<plAudioEventParameter> m_Parameters;
  plDynamicArray<plAudioEventAutomation> m_Automations;
  plDynamicArray<plAudioEventMarker> m_Markers;
  plDynamicArray<plAudioEventSustainPoint> m_SustainPoints;
};

class plAudioEventAssetDocument : public plSimpleAssetDocument<plAudioEventAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventAssetDocument, plSimpleAssetDocument<plAudioEventAssetProperties>);

public:
  plAudioEventAssetDocument(plStringView sDocumentPath);

  /// \brief Turns every playlist entry that embeds a sound file into one that references an Audio Clip of that file.
  ///
  /// The clip next to the sound file ("<file>.plAudioClipAsset") is reused when it exists and created otherwise.
  /// Entries that already reference a clip are left alone, so running this again changes nothing. The edit is one
  /// undoable transaction; the document is not saved.
  plStatus ExtractClips(plUInt32& out_uiConverted);

  /// \brief The runtime description of the event as it is edited now (not as it was saved).
  plStatus BuildEventDesc(plAudioEventDesc& out_desc) const;

  /// \brief Writes the distance curve into the description: the event's own, or the saved one of its shared Audio
  /// Attenuation.
  plStatus BuildAttenuation(plAudioEventDesc& ref_desc) const;

  /// \brief The description followed by the embedded sound files: the data of an event resource.
  plStatus WriteEventData(plStreamWriter& inout_stream, const plAudioEventDesc& desc) const;

  /// \brief Everything an event resource file holds (its path, the asset file header, the event data), for the audition
  /// in the engine process.
  plStatus WriteAuditionData(plStreamWriter& inout_stream, const plAudioEventDesc& desc) const;

  /// \brief The audition status the engine process reported last.
  const plAudioAuditionStatusMsgToEditor& GetAuditionStatus() const { return m_AuditionStatus; }

  /// \brief What an automation object drives, for labels: "Volume", "Pitch", "<instrument> volume", "Filter cutoff" or
  /// "Send <bus>".
  static void FormatAutomationTarget(const plDocumentObject* pAutomation, plStringBuilder& out_sTarget);

  /// \brief One pass of the event's timeline in seconds (where its last measured sound ends) and whether an instrument
  /// loops past it. Clip lengths come from their asset info; embedded sound files are not measured.
  void ComputeLength(float& out_fLength, bool& out_bLoops) const;

  /// \brief Raised for every audition status from the engine process.
  plEvent<const plAudioAuditionStatusMsgToEditor&> m_AuditionEvents;

protected:
  virtual void HandleEngineMessage(const plEditorEngineDocumentMsg* pMsg) override;

  virtual void UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const override;
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile,
    const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;

private:
  plAudioAuditionStatusMsgToEditor m_AuditionStatus;
};

//////////////////////////////////////////////////////////////////////////

/// \brief Creates audio events for imported sound files.
///
/// Offers no import mode of its own: sound files are imported through plAudioClipAssetDocumentGenerator, whose
/// "Audio_Clip_And_Event" mode calls GenerateForClip.
class plAudioEventAssetDocumentGenerator : public plAssetDocumentGenerator
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventAssetDocumentGenerator, plAssetDocumentGenerator);

public:
  plAudioEventAssetDocumentGenerator();
  ~plAudioEventAssetDocumentGenerator();

  /// \brief Creates "<sound file>.plAudioEventAsset" with one instrument playing the Audio Clip sClipGuid.
  /// Does nothing when the event exists already.
  static plStatus GenerateForClip(plStringView sInputFileAbs, plStringView sClipGuid, plDynamicArray<plDocument*>& out_generatedDocuments);

  virtual void GetImportModes(plStringView sAbsInputFile, plDynamicArray<plAssetDocumentGenerator::ImportMode>& out_modes) const override;
  virtual plStringView GetDocumentExtension() const override { return "plAudioEventAsset"; }
  virtual plStringView GetGeneratorGroup() const override { return "Audio"; }
  virtual plStatus Generate(plStringView sInputFileAbs, plStringView sMode, plDynamicArray<plDocument*>& out_generatedDocuments) override;
};
