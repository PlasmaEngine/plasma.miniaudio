#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/SoundAsset/MiniAudioSoundAssetManager.h>
#include <EditorPluginPlasmaAudio/SoundAsset/MiniAudioSoundAssetWindow.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioSoundAssetDocumentManager, 1, plRTTIDefaultAllocator<plMiniAudioSoundAssetDocumentManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plMiniAudioSoundAssetDocumentManager::plMiniAudioSoundAssetDocumentManager()
{
  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plMiniAudioSoundAssetDocumentManager::OnDocumentManagerEvent, this));

  m_DocTypeDesc.m_sDocumentTypeName = "MiniAudioSound";
  m_DocTypeDesc.m_sFileExtension = "plMiniAudioSoundAsset";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/MiniAudioSound.svg";
  m_DocTypeDesc.m_sAssetCategory = "Audio";

  // legacy asset: existing ones still open, transform and play, but new content
  // should be authored as Audio Events, so this type cannot be created any more
  m_DocTypeDesc.m_bCanCreate = false;
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plMiniAudioSoundAssetDocument>();
  m_DocTypeDesc.m_pManager = this;
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_MiniAudio_Sound");

  m_DocTypeDesc.m_sResourceFileExtension = "plBinMiniAudioSound";
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::AutoTransformOnSave;
}

plMiniAudioSoundAssetDocumentManager::~plMiniAudioSoundAssetDocumentManager()
{
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plMiniAudioSoundAssetDocumentManager::OnDocumentManagerEvent, this));
}

void plMiniAudioSoundAssetDocumentManager::OnDocumentManagerEvent(const plDocumentManager::Event& e)
{
  switch (e.m_Type)
  {
    case plDocumentManager::Event::Type::DocumentWindowRequested:
    {
      if (e.m_pDocument->GetDynamicRTTI() == plGetStaticRTTI<plMiniAudioSoundAssetDocument>())
      {
        new plMiniAudioSoundAssetDocumentWindow(e.m_pDocument); // NOLINT: Not a memory leak
      }
    }
    break;
    default:
      break;
  }
}

void plMiniAudioSoundAssetDocumentManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  out_pDocument = new plMiniAudioSoundAssetDocument(sPath);
}

void plMiniAudioSoundAssetDocumentManager::InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const
{
  inout_DocumentTypes.PushBack(&m_DocTypeDesc);
}
