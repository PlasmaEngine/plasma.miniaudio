#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAssetManager.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAssetWindow.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioEventAssetDocumentManager, 1, plRTTIDefaultAllocator<plAudioEventAssetDocumentManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plAudioEventAssetDocumentManager::plAudioEventAssetDocumentManager()
{
  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plAudioEventAssetDocumentManager::OnDocumentManagerEvent, this));

  m_DocTypeDesc.m_sDocumentTypeName = "Audio Event";
  m_DocTypeDesc.m_sFileExtension = "plAudioEventAsset";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/AudioEvent.svg";
  m_DocTypeDesc.m_sAssetCategory = "Audio";
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plAudioEventAssetDocument>();
  m_DocTypeDesc.m_pManager = this;
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_Audio_Event");

  m_DocTypeDesc.m_sResourceFileExtension = "plBinAudioEvent";
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::AutoTransformOnSave;
}

plAudioEventAssetDocumentManager::~plAudioEventAssetDocumentManager()
{
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plAudioEventAssetDocumentManager::OnDocumentManagerEvent, this));
}

void plAudioEventAssetDocumentManager::OnDocumentManagerEvent(const plDocumentManager::Event& e)
{
  switch (e.m_Type)
  {
    case plDocumentManager::Event::Type::DocumentWindowRequested:
    {
      if (e.m_pDocument->GetDynamicRTTI() == plGetStaticRTTI<plAudioEventAssetDocument>())
      {
        new plAudioEventAssetDocumentWindow(e.m_pDocument); // NOLINT: Not a memory leak
      }
    }
    break;
    default:
      break;
  }
}

void plAudioEventAssetDocumentManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  out_pDocument = new plAudioEventAssetDocument(sPath);
}

void plAudioEventAssetDocumentManager::InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const
{
  inout_DocumentTypes.PushBack(&m_DocTypeDesc);
}
