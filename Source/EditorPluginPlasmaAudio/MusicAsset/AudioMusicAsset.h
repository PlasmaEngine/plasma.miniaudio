#pragma once

#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <EnginePluginPlasmaAudio/Audition/AudioAuditionMessages.h>
#include <PlasmaAudioPlugin/Events/AudioEventResource.h>

struct plPropertyMetaStateEvent;

/// \brief An audio clip a segment plays from its start, and how its volume follows the Intensity parameter.
class plAudioMusicStem : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMusicStem, plReflectedClass);

public:
  plString m_sClip;
  float m_fVolumeDb = 0.0f;

  /// \brief The stem plays at full volume while Intensity is inside [min, max] and fades out over the fade beyond.
  bool m_bFollowIntensity = false;
  float m_fIntensityMin = 0.0f;
  float m_fIntensityMax = 1.0f;
  float m_fIntensityFade = 0.25f;
};

class plAudioMusicSegment : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMusicSegment, plReflectedClass);

public:
  plString m_sName;
  float m_fTempo = 120.0f;
  plUInt32 m_uiBeatsPerBar = 4;
  plUInt32 m_uiBars = 4;
  plDynamicArray<plUInt32> m_ExitCues; ///< bars after which a change set to "exit cue" may leave
  plDynamicArray<plAudioMusicStem> m_Stems;
};

class plAudioMusicState : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMusicState, plReflectedClass);

public:
  plString m_sName;
  plDynamicArray<plString> m_Segments; ///< segment names, played in order and again
};

class plAudioMusicTransition : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMusicTransition, plReflectedClass);

public:
  plString m_sFrom; ///< a state name; empty: from any state
  plString m_sTo;
  plEnum<plAudioMusicQuantize> m_eChangeAt;
  float m_fFadeBeats = 0.0f;
  plString m_sVia; ///< a segment that plays in between; empty: none
  plEnum<plAudioMusicStartAt> m_eStartAt;
};

class plAudioMusicStinger : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMusicStinger, plReflectedClass);

public:
  plString m_sName;
  plString m_sClip;
  float m_fVolumeDb = 0.0f;
  plEnum<plAudioMusicQuantize> m_eAt = plAudioMusicQuantize::NextBeat;
};

class plAudioMusicAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMusicAssetProperties, plReflectedClass);

public:
  /// \brief Hides the intensity range of stems that do not follow Intensity.
  static void PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);

  plString m_sOutputBus;
  plUInt8 m_uiPriority = 200;
  float m_fFadeIn = 0.0f;
  float m_fStopFadeOut = 1.0f;

  plString m_sInitialState; ///< empty: the first state
  bool m_bBeatMessages = false;

  float m_fIntensity = 1.0f;     ///< the Intensity parameter's default
  float m_fIntensitySeek = 0.5f; ///< how fast Intensity follows the game, per second

  plDynamicArray<plAudioMusicSegment> m_Segments;
  plDynamicArray<plAudioMusicState> m_States;
  plDynamicArray<plAudioMusicTransition> m_Transitions;
  plDynamicArray<plAudioMusicStinger> m_Stingers;
};

/// \brief An Audio Music asset: segments of stems on a beat grid, states that play them, transitions between the states
/// and stingers. It transforms into an audio event with a music section (plAudioMusicDesc), so it plays wherever an event
/// does: audio trigger components, banks, the mixer.
class plAudioMusicAssetDocument : public plSimpleAssetDocument<plAudioMusicAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMusicAssetDocument, plSimpleAssetDocument<plAudioMusicAssetProperties>);

public:
  plAudioMusicAssetDocument(plStringView sDocumentPath);

  static constexpr const char* IntensityParameter = "Intensity";

  /// \brief The runtime description of the music as it is edited now.
  plStatus BuildEventDesc(plAudioEventDesc& out_desc) const;

  /// \brief Everything an event resource file holds, for the audition in the engine process.
  plStatus WriteAuditionData(plStreamWriter& inout_stream, const plAudioEventDesc& desc) const;

  const plAudioAuditionStatusMsgToEditor& GetAuditionStatus() const { return m_AuditionStatus; }

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
