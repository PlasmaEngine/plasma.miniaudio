#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAssetManager.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAssetWindow.moc.h>
#include <GuiFoundation/UIServices/ImageCache.moc.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioClipProfileConfig, 1, plRTTIDefaultAllocator<plAudioClipProfileConfig>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("MaxSampleRate", m_uiMaxSampleRate)->AddAttributes(new plDefaultValueAttribute(48000), new plClampValueAttribute(8000, 192000)),
    PL_MEMBER_PROPERTY("OpusBitrateScale", m_fOpusBitrateScale)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.1f, 4.0f)),
    PL_MEMBER_PROPERTY("PreferAdpcmOverPcm", m_bPreferAdpcmOverPcm),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioClipAssetDocumentManager, 1, plRTTIDefaultAllocator<plAudioClipAssetDocumentManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plAudioClipAssetDocumentManager::plAudioClipAssetDocumentManager()
{
  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plAudioClipAssetDocumentManager::OnDocumentManagerEvent, this));

  plQtImageCache::GetSingleton()->RegisterTypeImage("Audio Clip", plSvgThumbnailToPixmap(":/AssetIcons/AudioClip.svg"));

  m_DocTypeDesc.m_sDocumentTypeName = "Audio Clip";
  m_DocTypeDesc.m_sFileExtension = "plAudioClipAsset";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/AudioClip.svg";
  m_DocTypeDesc.m_sAssetCategory = "Audio";
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plAudioClipAssetDocument>();
  m_DocTypeDesc.m_pManager = this;
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_Audio_Clip");

  m_DocTypeDesc.m_sResourceFileExtension = "plBinAudioClip";
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::AutoTransformOnSave;
}

plAudioClipAssetDocumentManager::~plAudioClipAssetDocumentManager()
{
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plAudioClipAssetDocumentManager::OnDocumentManagerEvent, this));
}

plUInt64 plAudioClipAssetDocumentManager::ComputeAssetProfileHashImpl(const plPlatformProfile* pAssetProfile) const
{
  // the profile's config name selects the clip's platform override
  plUInt64 uiHash = plHashingUtils::xxHash64String(pAssetProfile->GetConfigName());

  if (const plAudioClipProfileConfig* pConfig = pAssetProfile->GetTypeConfig<plAudioClipProfileConfig>())
  {
    const plUInt8 uiPreferAdpcm = pConfig->m_bPreferAdpcmOverPcm ? 1 : 0;
    uiHash = plHashingUtils::xxHash64(&pConfig->m_uiMaxSampleRate, sizeof(pConfig->m_uiMaxSampleRate), uiHash);
    uiHash = plHashingUtils::xxHash64(&pConfig->m_fOpusBitrateScale, sizeof(pConfig->m_fOpusBitrateScale), uiHash);
    uiHash = plHashingUtils::xxHash64(&uiPreferAdpcm, sizeof(uiPreferAdpcm), uiHash);
  }

  return uiHash;
}

void plAudioClipAssetDocumentManager::OnDocumentManagerEvent(const plDocumentManager::Event& e)
{
  switch (e.m_Type)
  {
    case plDocumentManager::Event::Type::DocumentWindowRequested:
    {
      if (e.m_pDocument->GetDynamicRTTI() == plGetStaticRTTI<plAudioClipAssetDocument>())
      {
        new plAudioClipAssetDocumentWindow(e.m_pDocument); // NOLINT: Not a memory leak
      }
    }
    break;
    default:
      break;
  }
}

void plAudioClipAssetDocumentManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  out_pDocument = new plAudioClipAssetDocument(sPath);
}

void plAudioClipAssetDocumentManager::InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const
{
  inout_DocumentTypes.PushBack(&m_DocTypeDesc);
}
