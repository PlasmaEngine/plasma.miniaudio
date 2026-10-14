#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAsset.h>
#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAssetManager.h>
#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAssetWindow.moc.h>
#include <GuiFoundation/UIServices/ImageCache.moc.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioBankAssetDocumentManager, 1, plRTTIDefaultAllocator<plAudioBankAssetDocumentManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plAudioBankAssetDocumentManager::plAudioBankAssetDocumentManager()
{
  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plAudioBankAssetDocumentManager::OnDocumentManagerEvent, this));

  plQtImageCache::GetSingleton()->RegisterTypeImage("Audio Bank", plSvgThumbnailToPixmap(":/AssetIcons/AudioBank.svg"));

  m_DocTypeDesc.m_sDocumentTypeName = "Audio Bank";
  m_DocTypeDesc.m_sFileExtension = "plAudioBankAsset";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/AudioBank.svg";
  m_DocTypeDesc.m_sAssetCategory = "Audio";
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plAudioBankAssetDocument>();
  m_DocTypeDesc.m_pManager = this;
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_Audio_Bank");

  m_DocTypeDesc.m_sResourceFileExtension = "plBinAudioBank";
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::AutoTransformOnSave;
}

plAudioBankAssetDocumentManager::~plAudioBankAssetDocumentManager()
{
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plAudioBankAssetDocumentManager::OnDocumentManagerEvent, this));
}

void plAudioBankAssetDocumentManager::OnDocumentManagerEvent(const plDocumentManager::Event& e)
{
  switch (e.m_Type)
  {
    case plDocumentManager::Event::Type::DocumentWindowRequested:
    {
      if (e.m_pDocument->GetDynamicRTTI() == plGetStaticRTTI<plAudioBankAssetDocument>())
      {
        new plAudioBankAssetDocumentWindow(e.m_pDocument); // NOLINT: Not a memory leak
      }
    }
    break;
    default:
      break;
  }
}

void plAudioBankAssetDocumentManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  out_pDocument = new plAudioBankAssetDocument(sPath);
}

void plAudioBankAssetDocumentManager::InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const
{
  inout_DocumentTypes.PushBack(&m_DocTypeDesc);
}
