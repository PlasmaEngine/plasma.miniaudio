#pragma once

#include <EditorFramework/Assets/AssetDocumentGenerator.h>
#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <PlasmaAudioPlugin/Clips/AudioClipBuilder.h>
#include <PlasmaAudioPlugin/Codecs/AudioCodec.h>

class plPlatformProfile;
struct plPropertyMetaStateEvent;

/// \brief Codec and quality settings of an audio clip for one platform profile.
///
/// Applies when the clip is transformed for the profile whose config name is m_sProfile.
class plAudioClipPlatformOverride : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioClipPlatformOverride, plReflectedClass);

public:
  plString m_sProfile;
  plEnum<plAudioClipLoadMode> m_eLoadMode;
  plEnum<plAudioClipCodec> m_eCodec;
  plUInt16 m_uiOpusKbps = 96;
  plUInt32 m_uiSampleRate = 0; ///< 0 keeps the sample rate of the default settings
};

/// \brief A localised variant of an audio clip: the same sound in another language.
class plAudioClipLanguageVariant : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioClipLanguageVariant, plReflectedClass);

public:
  plString m_sLanguage;
  plString m_sFile;
};

class plAudioClipAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioClipAssetProperties, plReflectedClass);

public:
  static void PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);

  plString m_sSourceFile;

  plEnum<plAudioClipLoadMode> m_eLoadMode;
  plEnum<plAudioClipCodec> m_eCodec;
  plUInt16 m_uiOpusKbps = 96;
  plUInt32 m_uiSampleRate = 0; ///< 0 keeps the source rate

  bool m_bForceMono = false;
  float m_fNormaliseLufs = -18.0f; ///< 0 disables normalisation
  bool m_bTrimSilence = false;

  bool m_bOverrideLoop = false;
  float m_fLoopStart = 0.0f; ///< seconds of the source file
  float m_fLoopEnd = 0.0f;   ///< seconds of the source file, 0 loops to the end

  plDynamicArray<plAudioClipPlatformOverride> m_PlatformOverrides;
  plDynamicArray<plAudioClipLanguageVariant> m_Languages;
};

/// \brief Source file facts published in the asset document info, so tools can show them without opening the clip.
class plAudioClipAssetMetaInfo : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioClipAssetMetaInfo, plReflectedClass);

public:
  plString m_sSourceFile;   ///< data directory relative path of the clip's source file
  double m_fDuration = 0.0; ///< seconds
  plUInt32 m_uiSampleRate = 0;
  plUInt8 m_uiChannels = 0;
  bool m_bHasLoop = false;
  double m_fLoopStart = 0.0; ///< seconds
  double m_fLoopEnd = 0.0;   ///< seconds
};

/// \brief Helpers shared by the clip transform, the clip editor preview and the importer.
struct plAudioClipAssetUtils
{
  /// \brief Level below which leading and trailing audio is removed when TrimSilence is on.
  static constexpr float TrimSilenceDb = -60.0f;

  /// \brief Build settings for pProfile (the default settings when pProfile is null): the profile's override, limited by
  /// the profile's plAudioClipProfileConfig. Loop points are converted with the source sample rate.
  static plAudioClipBuildSettings MakeBuildSettings(const plAudioClipAssetProperties& props, const plPlatformProfile* pProfile, plUInt32 uiSourceSampleRate);

  /// \brief Same as MakeBuildSettings, for an explicitly chosen override (may be null) and profile limits.
  static plAudioClipBuildSettings MakeBuildSettings(const plAudioClipAssetProperties& props, const plAudioClipPlatformOverride* pOverride, const plPlatformProfile* pProfile, plUInt32 uiSourceSampleRate);

  /// \brief The override of props for the profile's config name, or null.
  static const plAudioClipPlatformOverride* FindOverride(const plAudioClipAssetProperties& props, const plPlatformProfile* pProfile);

  /// \brief Length and format of an audio file without decoding it, cached per path and modification time.
  static plResult ReadSourceInfo(plStringView sAbsolutePath, plAudioClipAssetMetaInfo& out_info);

  /// \brief The Audio Clip of a sound file: "<file>.plAudioClipAsset" next to it, reused when it exists, otherwise created
  /// with the file as its SourceFile and saved. sSourceFile may be absolute or data directory relative.
  /// out_sClipGuid is the clip's asset GUID in string form, as stored in asset references.
  static plStatus FindOrCreateClip(plStringView sSourceFile, plString& out_sClipGuid);

  /// \brief The published meta info of the Audio Clip sClipGuid (a GUID string or asset path). False when the clip is unknown to the curator.
  static bool GetClipInfo(plStringView sClipGuid, plAudioClipAssetMetaInfo& out_info, plString* out_pDisplayName = nullptr);
};

class plAudioClipAssetDocument : public plSimpleAssetDocument<plAudioClipAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioClipAssetDocument, plSimpleAssetDocument<plAudioClipAssetProperties>);

public:
  plAudioClipAssetDocument(plStringView sDocumentPath);

protected:
  virtual void UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const override;
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile,
    const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
};

//////////////////////////////////////////////////////////////////////////

/// \brief Imports WAV, MP3 and FLAC files as audio clips ("Audio_Clip"), or as an audio clip plus an audio event
/// ("Audio_Clip_And_Event").
class plAudioClipAssetDocumentGenerator : public plAssetDocumentGenerator
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioClipAssetDocumentGenerator, plAssetDocumentGenerator);

public:
  plAudioClipAssetDocumentGenerator();
  ~plAudioClipAssetDocumentGenerator();

  virtual void GetImportModes(plStringView sAbsInputFile, plDynamicArray<plAssetDocumentGenerator::ImportMode>& out_modes) const override;
  virtual plStringView GetDocumentExtension() const override { return "plAudioClipAsset"; }
  virtual plStringView GetGeneratorGroup() const override { return "Audio"; }
  virtual plStatus Generate(plStringView sInputFileAbs, plStringView sMode, plDynamicArray<plDocument*>& out_generatedDocuments) override;
};
