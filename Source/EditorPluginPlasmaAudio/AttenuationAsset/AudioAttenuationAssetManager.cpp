#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationAsset.h>
#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationAssetManager.h>
#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationAssetWindow.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioAttenuationAssetDocumentManager, 1, plRTTIDefaultAllocator<plAudioAttenuationAssetDocumentManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plAudioAttenuationAssetDocumentManager::plAudioAttenuationAssetDocumentManager()
{
  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plAudioAttenuationAssetDocumentManager::OnDocumentManagerEvent, this));

  m_DocTypeDesc.m_sDocumentTypeName = "Audio Attenuation";
  m_DocTypeDesc.m_sFileExtension = "plAudioAttenuationAsset";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/AudioAttenuation.svg";
  m_DocTypeDesc.m_sAssetCategory = "Audio";
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plAudioAttenuationAssetDocument>();
  m_DocTypeDesc.m_pManager = this;
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_Audio_Attenuation");

  // the events that use the attenuation bake it when they transform
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::DisableTransform;
}

plAudioAttenuationAssetDocumentManager::~plAudioAttenuationAssetDocumentManager()
{
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plAudioAttenuationAssetDocumentManager::OnDocumentManagerEvent, this));
}

void plAudioAttenuationAssetDocumentManager::OnDocumentManagerEvent(const plDocumentManager::Event& e)
{
  switch (e.m_Type)
  {
    case plDocumentManager::Event::Type::DocumentWindowRequested:
    {
      if (e.m_pDocument->GetDynamicRTTI() == plGetStaticRTTI<plAudioAttenuationAssetDocument>())
      {
        new plAudioAttenuationAssetDocumentWindow(e.m_pDocument); // NOLINT: Not a memory leak
      }
    }
    break;
    default:
      break;
  }
}

void plAudioAttenuationAssetDocumentManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  out_pDocument = new plAudioAttenuationAssetDocument(sPath);
}

void plAudioAttenuationAssetDocumentManager::InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const
{
  inout_DocumentTypes.PushBack(&m_DocTypeDesc);
}
