#pragma once

#include <PlasmaAudioPlugin/Events/AudioEventResource.h>
#include <EditorFramework/Assets/AssetDocumentGenerator.h>
#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioRolloffAttribute.h>

/// \brief One entry of an instrument playlist.
///
/// Plays the Audio Clip asset m_sClip when set. Otherwise m_sFile is a sound file embedded into the event, and an
/// entry with neither is a weighted silence entry.
class plAudioEventPlaylistEntry : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventPlaylistEntry, plReflectedClass);

public:
  plString m_sClip; ///< asset GUID of an Audio Clip
  plString m_sFile; ///< embedded sound file, used when m_sClip is empty
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
  plDynamicArray<plAudioEventPlaylistEntry> m_Playlist;
};

/// \brief A named, ranged event parameter ("Distance" is built-in and fed automatically).
class plAudioEventParameter : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventParameter, plReflectedClass);

public:
  plString m_sName;
  float m_fMin = 0.0f;
  float m_fMax = 1.0f;
  float m_fDefault = 0.0f;
};

/// \brief One control point of an automation curve (x = parameter value, y = multiplier).
class plAudioEventCurvePoint : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventCurvePoint, plReflectedClass);

public:
  float m_fX = 0.0f;
  float m_fY = 1.0f;
};

/// \brief A parameter-driven automation curve modulating event volume or pitch.
class plAudioEventAutomation : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventAutomation, plReflectedClass);

public:
  plString m_sParameter;
  plEnum<plAudioEventAutomationTarget> m_eTarget;
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
  float m_fMinDistance = 1.0f;
  float m_fMaxDistance = 100.0f;
  float m_fRolloff = 1.0f;
  float m_fDopplerFactor = 0.0f;

  plDynamicArray<plAudioEventInstrument> m_Instruments;
  plDynamicArray<plAudioEventParameter> m_Parameters;
  plDynamicArray<plAudioEventAutomation> m_Automations;
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

protected:
  virtual void UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const override;
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile,
    const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
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
