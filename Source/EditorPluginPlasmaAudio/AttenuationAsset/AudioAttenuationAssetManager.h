#pragma once

#include <EditorFramework/Assets/AssetDocumentManager.h>
#include <Foundation/Types/Status.h>

class plAudioAttenuationAssetDocumentManager : public plAssetDocumentManager
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioAttenuationAssetDocumentManager, plAssetDocumentManager);

public:
  plAudioAttenuationAssetDocumentManager();
  ~plAudioAttenuationAssetDocumentManager();

  virtual OutputReliability GetAssetTypeOutputReliability() const override { return plAssetDocumentManager::OutputReliability::Perfect; }

  /// \brief Always up to date: the asset has no output, the events that use it bake it.
  virtual bool IsOutputUpToDate(plStringView sDocumentPath, plStringView sOutputTag, plUInt64 uiHash, const plAssetDocumentTypeDescriptor* pTypeDescriptor) override { return true; }

private:
  void OnDocumentManagerEvent(const plDocumentManager::Event& e);

  virtual void InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext) override;
  virtual void InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const override;

  virtual bool GeneratesProfileSpecificAssets() const override { return false; }

  plAssetDocumentTypeDescriptor m_DocTypeDesc;
};
