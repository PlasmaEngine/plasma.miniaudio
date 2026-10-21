#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <Core/Configuration/PlatformProfile.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAsset.h>
#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAssetManager.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <PlasmaAudioPlugin/Banks/AudioBankResource.h>
#include <PlasmaAudioPlugin/Clips/AudioClipDesc.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioBankAssetProperties, 2, plRTTIDefaultAllocator<plAudioBankAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ARRAY_MEMBER_PROPERTY("Events", m_Events)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Audio_Event", plDependencyFlags::Transform | plDependencyFlags::Package)),
    PL_ARRAY_MEMBER_PROPERTY("Clips", m_Clips)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Audio_Clip", plDependencyFlags::Transform | plDependencyFlags::Package)),
    PL_MEMBER_PROPERTY("AutoIncludeEventClips", m_bAutoIncludeEventClips)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("Packed", m_bPacked),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioBankAssetMetaInfo, 1, plRTTIDefaultAllocator<plAudioBankAssetMetaInfo>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ARRAY_MEMBER_PROPERTY("Events", m_Events),
    PL_ARRAY_MEMBER_PROPERTY("Clips", m_Clips),
    PL_MEMBER_PROPERTY("AutoIncludeEventClips", m_bAutoIncludeEventClips),
    PL_MEMBER_PROPERTY("Packed", m_bPacked),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioBankAssetDocument, 2, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  /// The asset's built file for the profile, as the curator would write it.
  plString GetOutputFile(const plAssetCurator::plLockedSubAsset& pSubAsset, const plPlatformProfile* pProfile)
  {
    const plAssetDocumentTypeDescriptor* pTypeDesc = pSubAsset->m_pAssetInfo->m_pDocumentTypeDescriptor;
    const auto* pManager = static_cast<const plAssetDocumentManager*>(pTypeDesc->m_pManager);
    return pManager->GetAbsoluteOutputFileName(pTypeDesc, pSubAsset->m_pAssetInfo->m_Path.GetAbsolutePath(), "", pProfile);
  }

  /// Appends the built file of the asset to the packed data, unless it is a streamed clip. Fails when it is not built.
  plStatus PackAsset(plStringView sGuid, bool bClip, const plPlatformProfile* pProfile, plAudioBankDesc& inout_desc, plDynamicArray<plUInt8>& inout_data)
  {
    plStringBuilder sFile;
    plString sName;
    {
      const auto pSubAsset = plAssetCurator::GetSingleton()->FindSubAsset(sGuid);
      if (!pSubAsset.isValid() || pSubAsset->m_pAssetInfo == nullptr || pSubAsset->m_pAssetInfo->m_pDocumentTypeDescriptor == nullptr)
        return plStatus(plFmt("The bank references an unknown asset: '{0}'", sGuid));

      sFile = GetOutputFile(pSubAsset, pProfile);
      sName = pSubAsset->GetName();
    }

    plFileReader file;
    if (file.Open(sFile).Failed())
      return plStatus(plFmt("'{0}' is not built yet; the packed bank needs its built file", sName));

    plDynamicArray<plUInt8> bytes;
    bytes.SetCountUninitialized(static_cast<plUInt32>(file.GetFileSize()));
    if (file.ReadBytes(bytes.GetData(), bytes.GetCount()) != bytes.GetCount())
      return plStatus(plFmt("Reading the built file of '{0}' failed", sName));

    // the stream service reads a streamed payload from the clip's own file
    if (bClip)
    {
      plRawMemoryStreamReader reader(bytes.GetData(), bytes.GetCount());
      plAssetFileHeader assetHeader;
      plAudioClipHeader header;
      if (assetHeader.Read(reader).Failed() || header.Read(reader).Failed())
        return plStatus(plFmt("The built file of '{0}' is not a valid audio clip", sName));

      if (header.m_eLoadMode == plAudioClipLoadMode::Streamed)
        return plStatus(PL_SUCCESS);
    }

    plAudioBankPackedEntry& entry = inout_desc.m_Packed.ExpandAndGetRef();
    entry.m_sResourceId = sGuid;
    entry.m_bClip = bClip;
    entry.m_uiOffset = inout_data.GetCount();
    entry.m_uiBytes = bytes.GetCount();
    inout_data.PushBackRange(bytes);
    return plStatus(PL_SUCCESS);
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// plAudioBankAssetUtils

plString plAudioBankAssetUtils::ResolveAsset(plStringView sReference)
{
  if (sReference.IsEmpty())
    return plString();

  const auto pSubAsset = plAssetCurator::GetSingleton()->FindSubAsset(sReference);
  if (!pSubAsset.isValid())
    return plString();

  plStringBuilder sGuid;
  plConversionUtils::ToString(pSubAsset->m_Data.m_Guid, sGuid);
  return sGuid;
}

void plAudioBankAssetUtils::AddEventClips(plStringView sEventGuid, plDynamicArray<plString>& inout_clips)
{
  plHybridArray<plString, 8> references;
  {
    const auto pEvent = plAssetCurator::GetSingleton()->FindSubAsset(sEventGuid);
    if (!pEvent.isValid() || pEvent->m_pAssetInfo == nullptr || pEvent->m_pAssetInfo->m_Info == nullptr)
      return;

    // the event's clip references are dependencies of its transform
    for (const plString& sDependency : pEvent->m_pAssetInfo->m_Info->m_TransformDependencies)
    {
      references.PushBack(sDependency);
    }
  }

  for (const plString& sReference : references)
  {
    const auto pClip = plAssetCurator::GetSingleton()->FindSubAsset(sReference);
    if (!pClip.isValid() || pClip->m_Data.m_sSubAssetsDocumentTypeName.GetView() != "Audio Clip")
      continue;

    plStringBuilder sGuid;
    plConversionUtils::ToString(pClip->m_Data.m_Guid, sGuid);
    if (!inout_clips.Contains(sGuid))
      inout_clips.PushBack(sGuid);
  }
}

plAudioMemoryBudget plAudioBankAssetUtils::GetBudget(const plPlatformProfile* pProfile)
{
  plAudioMemoryBudget budget;

  const plAudioBudgetProfileConfig* pConfig = pProfile != nullptr ? pProfile->GetTypeConfig<plAudioBudgetProfileConfig>() : nullptr;
  if (pConfig == nullptr)
    return budget;

  auto ToBytes = [](float fMegabytes)
  { return static_cast<plUInt64>(plMath::Max(fMegabytes, 0.0f) * 1024.0 * 1024.0); };

  budget.m_uiDecodedBytes = ToBytes(pConfig->m_fDecodedMB);
  budget.m_uiCompressedBytes = ToBytes(pConfig->m_fCompressedMB);
  budget.m_uiStreamingBytes = ToBytes(pConfig->m_fStreamingMB);
  budget.m_uiTotalBytes = ToBytes(pConfig->m_fTotalMB);
  return budget;
}

//////////////////////////////////////////////////////////////////////////
// plAudioBankAssetDocument

plAudioBankAssetDocument::plAudioBankAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plAudioBankAssetProperties>(sDocumentPath, plAssetDocEngineConnection::Simple)
{
}

void plAudioBankAssetDocument::SendBankCommand(plAudioBankCommand::Enum command, plStringView sBankGuid)
{
  plAudioBankMsgToEngine msg;
  msg.m_Command = command;
  msg.m_sBank = sBankGuid;
  SendMessageToEngine(&msg);
}

void plAudioBankAssetDocument::HandleEngineMessage(const plEditorEngineDocumentMsg* pMsg)
{
  if (const auto* pStatus = plDynamicCast<const plAudioBankStatusMsgToEditor*>(pMsg))
  {
    m_EngineStatus = *pStatus;
    m_EngineStatusEvents.Broadcast(m_EngineStatus);
    return;
  }

  plSimpleAssetDocument<plAudioBankAssetProperties>::HandleEngineMessage(pMsg);
}

void plAudioBankAssetDocument::UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const
{
  SUPER::UpdateAssetDocumentInfo(pInfo);

  const plAudioBankAssetProperties* pProp = GetProperties();

  plAudioBankAssetMetaInfo* pMeta = PL_DEFAULT_NEW(plAudioBankAssetMetaInfo);
  pMeta->m_bAutoIncludeEventClips = pProp->m_bAutoIncludeEventClips;
  pMeta->m_bPacked = pProp->m_bPacked;

  for (const plString& sEvent : pProp->m_Events)
  {
    const plString sGuid = plAudioBankAssetUtils::ResolveAsset(sEvent);
    if (!sGuid.IsEmpty() && !pMeta->m_Events.Contains(sGuid))
      pMeta->m_Events.PushBack(sGuid);
  }

  for (const plString& sClip : pProp->m_Clips)
  {
    const plString sGuid = plAudioBankAssetUtils::ResolveAsset(sClip);
    if (!sGuid.IsEmpty() && !pMeta->m_Clips.Contains(sGuid))
      pMeta->m_Clips.PushBack(sGuid);
  }

  pInfo->m_MetaInfo.PushBack(pMeta);
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

      const plString sGuid = plAudioBankAssetUtils::ResolveAsset(sReference);
      if (sGuid.IsEmpty())
        return plStatus(plFmt("The bank references an unknown asset: '{0}'", sReference));

      if (!out_ids.Contains(sGuid))
        out_ids.PushBack(sGuid);
    }

    return plStatus(PL_SUCCESS);
  };

  plAudioBankDesc desc;
  PL_SUCCEED_OR_RETURN(ResolveList(pProp->m_Events, desc.m_Events));
  PL_SUCCEED_OR_RETURN(ResolveList(pProp->m_Clips, desc.m_Clips));

  if (pProp->m_bAutoIncludeEventClips)
  {
    for (const plString& sEvent : desc.m_Events)
    {
      plAudioBankAssetUtils::AddEventClips(sEvent, desc.m_Clips);
    }
  }

  desc.m_Budget = plAudioBankAssetUtils::GetBudget(pAssetProfile);

  plDynamicArray<plUInt8> packedData;
  if (pProp->m_bPacked)
  {
    for (const plString& sEvent : desc.m_Events)
    {
      PL_SUCCEED_OR_RETURN(PackAsset(sEvent, false, pAssetProfile, desc, packedData));
    }

    for (const plString& sClip : desc.m_Clips)
    {
      PL_SUCCEED_OR_RETURN(PackAsset(sClip, true, pAssetProfile, desc, packedData));
    }
  }

  if (desc.Save(stream).Failed() || stream.WriteBytes(packedData.GetData(), packedData.GetCount()).Failed())
    return plStatus("Writing the audio bank failed.");

  return plStatus(PL_SUCCESS);
}
