#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerAsset.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerAssetManager.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerAssetWindow.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerAssetDocumentManager, 1, plRTTIDefaultAllocator<plAudioMixerAssetDocumentManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plAudioMixerAssetDocumentManager::plAudioMixerAssetDocumentManager()
{
  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentManager::OnDocumentManagerEvent, this));

  m_DocTypeDesc.m_sDocumentTypeName = "Audio Mixer";
  m_DocTypeDesc.m_sFileExtension = "plAudioMixerAsset";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/AudioMixer.svg";
  m_DocTypeDesc.m_sAssetCategory = "Audio";
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plAudioMixerAssetDocument>();
  m_DocTypeDesc.m_pManager = this;
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_Audio_Mixer");

  m_DocTypeDesc.m_sResourceFileExtension = "plBinAudioMixer";
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::AutoTransformOnSave;
}

plAudioMixerAssetDocumentManager::~plAudioMixerAssetDocumentManager()
{
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentManager::OnDocumentManagerEvent, this));
}

void plAudioMixerAssetDocumentManager::OnDocumentManagerEvent(const plDocumentManager::Event& e)
{
  switch (e.m_Type)
  {
    case plDocumentManager::Event::Type::DocumentWindowRequested:
    {
      if (e.m_pDocument->GetDynamicRTTI() == plGetStaticRTTI<plAudioMixerAssetDocument>())
      {
        new plAudioMixerAssetDocumentWindow(e.m_pDocument); // NOLINT: Not a memory leak
      }
    }
    break;
    default:
      break;
  }
}

void plAudioMixerAssetDocumentManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  out_pDocument = new plAudioMixerAssetDocument(sPath);
}

void plAudioMixerAssetDocumentManager::InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const
{
  inout_DocumentTypes.PushBack(&m_DocTypeDesc);
}
