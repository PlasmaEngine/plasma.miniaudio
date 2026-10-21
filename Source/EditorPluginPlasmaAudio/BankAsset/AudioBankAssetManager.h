#pragma once

#include <Core/Configuration/PlatformProfile.h>
#include <EditorFramework/Assets/AssetDocumentManager.h>

/// \brief Per platform profile memory budget of the loaded audio content, in MB. 0 is no limit.
///
/// Banks bake the budget of the profile they are built for; the runtime checks the loaded content against it whenever a
/// bank finishes loading. The bank window shows it for each platform.
class plAudioBudgetProfileConfig : public plProfileConfigData
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioBudgetProfileConfig, plProfileConfigData);

public:
  float m_fTotalMB = 0.0f;
  float m_fDecodedMB = 0.0f;    ///< clips decompressed on load
  float m_fCompressedMB = 0.0f; ///< clips compressed in memory
  float m_fStreamingMB = 0.0f;  ///< priming data of streamed clips and the stream rings
};

class plAudioBankAssetDocumentManager : public plAssetDocumentManager
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioBankAssetDocumentManager, plAssetDocumentManager);

public:
  plAudioBankAssetDocumentManager();
  ~plAudioBankAssetDocumentManager();

  virtual OutputReliability GetAssetTypeOutputReliability() const override { return plAssetDocumentManager::OutputReliability::Perfect; }

private:
  void OnDocumentManagerEvent(const plDocumentManager::Event& e);

  virtual void InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext) override;
  virtual void InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const override;

  /// a bank carries the budget of its profile and, when packed, the clips as built for it
  virtual bool GeneratesProfileSpecificAssets() const override { return true; }
  virtual plUInt64 ComputeAssetProfileHashImpl(const plPlatformProfile* pAssetProfile) const override;

  plAssetDocumentTypeDescriptor m_DocTypeDesc;
};
