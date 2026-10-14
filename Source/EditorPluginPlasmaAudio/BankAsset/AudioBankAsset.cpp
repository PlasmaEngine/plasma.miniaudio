#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAsset.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <PlasmaAudioPlugin/Banks/AudioBankResource.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioBankAssetProperties, 1, plRTTIDefaultAllocator<plAudioBankAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ARRAY_MEMBER_PROPERTY("Events", m_Events)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Audio_Event", plDependencyFlags::Package)),
    PL_ARRAY_MEMBER_PROPERTY("Clips", m_Clips)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Audio_Clip", plDependencyFlags::Package)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioBankAssetDocument, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plAudioBankAssetDocument::plAudioBankAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plAudioBankAssetProperties>(sDocumentPath, plAssetDocEngineConnection::None)
{
}

plTransformStatus plAudioBankAssetDocument::InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  const plAudioBankAssetProperties* pProp = GetProperties();

  // references are stored as asset GUID strings (or paths); the runtime loads them by GUID string
  auto ResolveList = [](const plDynamicArray<plString>& references, plDynamicArray<plString>& out_ids) -> plStatus
  {
    for (const plString& sReference : references)
    {
      if (sReference.IsEmpty())
        continue;

      plAssetCurator::plLockedSubAsset pSubAsset = plAssetCurator::GetSingleton()->FindSubAsset(sReference);
      if (pSubAsset == nullptr)
        return plStatus(plFmt("The bank references an unknown asset: '{0}'", sReference));

      plStringBuilder sGuid;
      plConversionUtils::ToString(pSubAsset->m_Data.m_Guid, sGuid);

      if (!out_ids.Contains(sGuid))
        out_ids.PushBack(sGuid);
    }

    return plStatus(PL_SUCCESS);
  };

  plAudioBankDesc desc;
  PL_SUCCEED_OR_RETURN(ResolveList(pProp->m_Events, desc.m_Events));
  PL_SUCCEED_OR_RETURN(ResolveList(pProp->m_Clips, desc.m_Clips));

  if (desc.Save(stream).Failed())
    return plStatus("Writing the audio bank failed.");

  return plStatus(PL_SUCCESS);
}
