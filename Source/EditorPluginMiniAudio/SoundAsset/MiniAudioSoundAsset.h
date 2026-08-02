#pragma once

#include <EditorFramework/Assets/AssetDocumentGenerator.h>
#include <EditorFramework/Assets/SimpleAssetDocument.h>

class plMiniAudioSoundAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioSoundAssetProperties, plReflectedClass);

public:
  plMiniAudioSoundAssetProperties() = default;

  static void PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);

  plString m_sGroup;
  bool m_bLoop = false;
  float m_fMinVolume = 1.0f;
  float m_fMaxVolume = 1.0f;
  float m_fMinPitch = 1.0f;
  float m_fMaxPitch = 1.0f;
  bool m_bSpatialize = true;
  float m_fMinDistance = 0.1f;
  float m_fMaxDistance = 10.0f;
  float m_fRolloff = 1.0f;
  float m_fDopplerFactor = 0.0f;
  plDynamicArray<plString> m_SoundFiles;
  // disable pitch
  // no global pitch
  // looping
  // fade out duration ?
  // fully decode / stream
};

class plMiniAudioSoundAssetDocument : public plSimpleAssetDocument<plMiniAudioSoundAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioSoundAssetDocument, plSimpleAssetDocument<plMiniAudioSoundAssetProperties>);

public:
  plMiniAudioSoundAssetDocument(plStringView sDocumentPath);

protected:
  virtual void UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const override;
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile,
    const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
};


//////////////////////////////////////////////////////////////////////////


class plMiniAudioSoundAssetDocumentGenerator : public plAssetDocumentGenerator
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioSoundAssetDocumentGenerator, plAssetDocumentGenerator);

public:
  plMiniAudioSoundAssetDocumentGenerator();
  ~plMiniAudioSoundAssetDocumentGenerator();

  virtual void GetImportModes(plStringView sAbsInputFile, plDynamicArray<plAssetDocumentGenerator::ImportMode>& out_modes) const override;
  virtual plStringView GetDocumentExtension() const override { return "plMiniAudioSoundAsset"; }
  virtual plStringView GetGeneratorGroup() const override { return "Audio"; }
  virtual plStatus Generate(plStringView sInputFileAbs, plStringView sMode, plDynamicArray<plDocument*>& out_generatedDocuments) override;
};