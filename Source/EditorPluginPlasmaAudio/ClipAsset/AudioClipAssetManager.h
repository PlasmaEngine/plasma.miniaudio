#pragma once

#include <Core/Configuration/PlatformProfile.h>
#include <EditorFramework/Assets/AssetDocumentManager.h>

/// \brief Per platform profile limits for audio clips, applied on top of each clip's own settings.
class plAudioClipProfileConfig : public plProfileConfigData
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioClipProfileConfig, plProfileConfigData);

public:
  /// \brief Clips with a higher output rate are resampled down to this rate. Opus clips always use 48 kHz.
  plUInt32 m_uiMaxSampleRate = 48000;

  /// \brief Multiplier for the Opus bitrate of every clip.
  float m_fOpusBitrateScale = 1.0f;

  /// \brief Encodes clips set to PCM with IMA ADPCM instead.
  bool m_bPreferAdpcmOverPcm = false;
};

class plAudioClipAssetDocumentManager : public plAssetDocumentManager
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioClipAssetDocumentManager, plAssetDocumentManager);

public:
  plAudioClipAssetDocumentManager();
  ~plAudioClipAssetDocumentManager();

  virtual OutputReliability GetAssetTypeOutputReliability() const override { return plAssetDocumentManager::OutputReliability::Perfect; }

private:
  void OnDocumentManagerEvent(const plDocumentManager::Event& e);

  virtual plUInt64 ComputeAssetProfileHashImpl(const plPlatformProfile* pAssetProfile) const override;

  virtual void InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext) override;
  virtual void InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const override;

  virtual bool GeneratesProfileSpecificAssets() const override { return true; }

  plAssetDocumentTypeDescriptor m_DocTypeDesc;
};
