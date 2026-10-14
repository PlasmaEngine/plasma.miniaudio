#pragma once

#include <EditorFramework/Assets/AssetDocumentManager.h>

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

  virtual bool GeneratesProfileSpecificAssets() const override { return false; }

  plAssetDocumentTypeDescriptor m_DocTypeDesc;
};
