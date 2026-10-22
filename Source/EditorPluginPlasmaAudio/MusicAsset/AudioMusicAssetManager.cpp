#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicAsset.h>
#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicAssetManager.h>
#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicAssetWindow.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMusicAssetDocumentManager, 1, plRTTIDefaultAllocator<plAudioMusicAssetDocumentManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plAudioMusicAssetDocumentManager::plAudioMusicAssetDocumentManager()
{
  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plAudioMusicAssetDocumentManager::OnDocumentManagerEvent, this));

  m_DocTypeDesc.m_sDocumentTypeName = "Audio Music";
  m_DocTypeDesc.m_sFileExtension = "plAudioMusicAsset";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/AudioMusic.svg";
  m_DocTypeDesc.m_sAssetCategory = "Audio";
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plAudioMusicAssetDocument>();
  m_DocTypeDesc.m_pManager = this;

  // it plays wherever an event plays
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_Audio_Event");
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_Audio_Music");

  m_DocTypeDesc.m_sResourceFileExtension = "plBinAudioEvent";
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::AutoTransformOnSave;
}

plAudioMusicAssetDocumentManager::~plAudioMusicAssetDocumentManager()
{
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plAudioMusicAssetDocumentManager::OnDocumentManagerEvent, this));
}

void plAudioMusicAssetDocumentManager::OnDocumentManagerEvent(const plDocumentManager::Event& e)
{
  if (e.m_Type == plDocumentManager::Event::Type::DocumentWindowRequested && e.m_pDocument->GetDynamicRTTI() == plGetStaticRTTI<plAudioMusicAssetDocument>())
  {
    new plAudioMusicAssetDocumentWindow(e.m_pDocument); // NOLINT: Not a memory leak
  }
}

void plAudioMusicAssetDocumentManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  out_pDocument = new plAudioMusicAssetDocument(sPath);
}

void plAudioMusicAssetDocumentManager::InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const
{
  inout_DocumentTypes.PushBack(&m_DocTypeDesc);
}
