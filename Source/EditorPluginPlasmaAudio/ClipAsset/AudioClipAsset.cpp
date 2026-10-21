#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <Core/Configuration/PlatformProfile.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAssetManager.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAsset.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Serialization/AbstractObjectGraph.h>
#include <Foundation/Serialization/DdlSerializer.h>
#include <Foundation/Serialization/RttiConverter.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <PlasmaAudioPlugin/Clips/AudioClipDesc.h>
#include <PlasmaAudioPlugin/Codecs/ImaAdpcm.h>
#include <Foundation/Strings/PathUtils.h>
#include <Foundation/Strings/StringConversion.h>
#include <Foundation/Threading/Mutex.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <MiniAudio/miniaudio.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioClipPlatformOverride, 1, plRTTIDefaultAllocator<plAudioClipPlatformOverride>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Profile", m_sProfile),
    PL_ENUM_MEMBER_PROPERTY("LoadMode", plAudioClipLoadMode, m_eLoadMode),
    PL_ENUM_MEMBER_PROPERTY("Codec", plAudioClipCodec, m_eCodec),
    PL_MEMBER_PROPERTY("OpusKbps", m_uiOpusKbps)->AddAttributes(new plDefaultValueAttribute(96), new plClampValueAttribute(6, 510), new plSuffixAttribute(" kbps")),
    PL_MEMBER_PROPERTY("SampleRate", m_uiSampleRate)->AddAttributes(new plClampValueAttribute(0, 192000), new plSuffixAttribute(" Hz")),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioClipLanguageVariant, 1, plRTTIDefaultAllocator<plAudioClipLanguageVariant>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Language", m_sLanguage),
    PL_MEMBER_PROPERTY("File", m_sFile)->AddAttributes(new plFileBrowserAttribute("Select Sound", "*.wav;*.mp3;*.flac")),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioClipAssetProperties, 1, plRTTIDefaultAllocator<plAudioClipAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("SourceFile", m_sSourceFile)->AddAttributes(new plFileBrowserAttribute("Select Sound", "*.wav;*.mp3;*.flac")),
    PL_ENUM_MEMBER_PROPERTY("LoadMode", plAudioClipLoadMode, m_eLoadMode)->AddAttributes(new plGroupAttribute("Encoding")),
    PL_ENUM_MEMBER_PROPERTY("Codec", plAudioClipCodec, m_eCodec),
    PL_MEMBER_PROPERTY("OpusKbps", m_uiOpusKbps)->AddAttributes(new plDefaultValueAttribute(96), new plClampValueAttribute(6, 510), new plSuffixAttribute(" kbps")),
    PL_MEMBER_PROPERTY("SampleRate", m_uiSampleRate)->AddAttributes(new plClampValueAttribute(0, 192000), new plSuffixAttribute(" Hz")),
    PL_MEMBER_PROPERTY("ForceMono", m_bForceMono)->AddAttributes(new plGroupAttribute("Processing")),
    PL_MEMBER_PROPERTY("NormaliseLufs", m_fNormaliseLufs)->AddAttributes(new plDefaultValueAttribute(-18.0f), new plClampValueAttribute(-60.0f, 0.0f), new plSuffixAttribute(" LUFS")),
    PL_MEMBER_PROPERTY("TrimSilence", m_bTrimSilence),
    PL_MEMBER_PROPERTY("OverrideLoop", m_bOverrideLoop)->AddAttributes(new plGroupAttribute("Loop")),
    PL_MEMBER_PROPERTY("LoopStart", m_fLoopStart)->AddAttributes(new plClampValueAttribute(0.0f, 36000.0f), new plSuffixAttribute(" sec")),
    PL_MEMBER_PROPERTY("LoopEnd", m_fLoopEnd)->AddAttributes(new plClampValueAttribute(0.0f, 36000.0f), new plSuffixAttribute(" sec")),
    PL_ARRAY_MEMBER_PROPERTY("PlatformOverrides", m_PlatformOverrides),
    PL_ARRAY_MEMBER_PROPERTY("Languages", m_Languages)->AddAttributes(new plGroupAttribute("Localisation")),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioClipAssetMetaInfo, 1, plRTTIDefaultAllocator<plAudioClipAssetMetaInfo>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("SourceFile", m_sSourceFile),
    PL_MEMBER_PROPERTY("Duration", m_fDuration),
    PL_MEMBER_PROPERTY("SampleRate", m_uiSampleRate),
    PL_MEMBER_PROPERTY("Channels", m_uiChannels),
    PL_MEMBER_PROPERTY("HasLoop", m_bHasLoop),
    PL_MEMBER_PROPERTY("LoopStart", m_fLoopStart),
    PL_MEMBER_PROPERTY("LoopEnd", m_fLoopEnd),
    PL_MEMBER_PROPERTY("ForceMono", m_bForceMono),
    PL_ARRAY_MEMBER_PROPERTY("Platforms", m_Platforms),
    PL_ARRAY_MEMBER_PROPERTY("Languages", m_Languages),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioClipPlatformInfo, 1, plRTTIDefaultAllocator<plAudioClipPlatformInfo>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Profile", m_sProfile),
    PL_ENUM_MEMBER_PROPERTY("LoadMode", plAudioClipLoadMode, m_eLoadMode),
    PL_ENUM_MEMBER_PROPERTY("Codec", plAudioClipCodec, m_eCodec),
    PL_MEMBER_PROPERTY("SampleRate", m_uiSampleRate),
    PL_MEMBER_PROPERTY("OpusKbps", m_uiOpusKbps),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioClipLanguageInfo, 1, plRTTIDefaultAllocator<plAudioClipLanguageInfo>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Language", m_sLanguage),
    PL_MEMBER_PROPERTY("Duration", m_fDuration),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioClipAssetDocument, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  plUInt32 ReadLE32(const plUInt8* pData)
  {
    return static_cast<plUInt32>(pData[0]) | (static_cast<plUInt32>(pData[1]) << 8) | (static_cast<plUInt32>(pData[2]) << 16) | (static_cast<plUInt32>(pData[3]) << 24);
  }

  /// First frame and (inclusive) last frame of the first loop in the smpl chunk of a WAV file.
  bool ReadWavSmplLoop(plStringView sPath, plUInt32& out_uiStart, plUInt32& out_uiLast)
  {
    plOSFile file;
    if (file.Open(sPath, plFileOpenMode::Read).Failed())
      return false;

    plUInt8 riff[12];
    if (file.Read(riff, 12) != 12 || plMemoryUtils::Compare(riff, reinterpret_cast<const plUInt8*>("RIFF"), 4) != 0 ||
        plMemoryUtils::Compare(riff + 8, reinterpret_cast<const plUInt8*>("WAVE"), 4) != 0)
      return false;

    plUInt8 chunk[8];
    while (file.Read(chunk, 8) == 8)
    {
      const plUInt32 uiSize = ReadLE32(chunk + 4);

      if (plMemoryUtils::Compare(chunk, reinterpret_cast<const plUInt8*>("smpl"), 4) == 0)
      {
        // 36 bytes of sampler data, then 24 bytes per loop: cue id, type, start, end, fraction, play count
        plUInt8 smpl[36 + 24];
        if (uiSize < sizeof(smpl) || file.Read(smpl, sizeof(smpl)) != sizeof(smpl))
          return false;

        if (ReadLE32(smpl + 28) == 0)
          return false;

        out_uiStart = ReadLE32(smpl + 36 + 8);
        out_uiLast = ReadLE32(smpl + 36 + 12);
        return true;
      }

      file.SetFilePosition(static_cast<plInt64>(uiSize) + (uiSize & 1), plFileSeekMode::FromCurrent);
    }

    return false;
  }

  struct SourceInfoCacheEntry
  {
    plInt64 m_iModificationTime = 0;
    bool m_bValid = false;
    plAudioClipAssetMetaInfo m_Info;
  };

  plMutex s_SourceInfoMutex;
  plHashTable<plString, SourceInfoCacheEntry> s_SourceInfoCache;
} // namespace

//////////////////////////////////////////////////////////////////////////
// plAudioClipAssetUtils
//////////////////////////////////////////////////////////////////////////

const plAudioClipPlatformOverride* plAudioClipAssetUtils::FindOverride(const plAudioClipAssetProperties& props, const plPlatformProfile* pProfile)
{
  if (pProfile == nullptr)
    return nullptr;

  for (const plAudioClipPlatformOverride& o : props.m_PlatformOverrides)
  {
    if (o.m_sProfile.IsEqual_NoCase(pProfile->GetConfigName()))
      return &o;
  }

  return nullptr;
}

plAudioClipBuildSettings plAudioClipAssetUtils::MakeBuildSettings(const plAudioClipAssetProperties& props, const plPlatformProfile* pProfile, plUInt32 uiSourceSampleRate)
{
  return MakeBuildSettings(props, FindOverride(props, pProfile), pProfile, uiSourceSampleRate);
}

plAudioClipBuildSettings plAudioClipAssetUtils::MakeBuildSettings(const plAudioClipAssetProperties& props, const plAudioClipPlatformOverride* pOverride, const plPlatformProfile* pProfile, plUInt32 uiSourceSampleRate)
{
  plAudioClipBuildSettings settings;
  settings.m_eLoadMode = props.m_eLoadMode;
  settings.m_eCodec = props.m_eCodec;
  settings.m_uiOpusBitrateKbps = props.m_uiOpusKbps;
  settings.m_uiTargetSampleRate = props.m_uiSampleRate;

  if (pOverride != nullptr)
  {
    settings.m_eLoadMode = pOverride->m_eLoadMode;
    settings.m_eCodec = pOverride->m_eCodec;
    settings.m_uiOpusBitrateKbps = pOverride->m_uiOpusKbps;

    if (pOverride->m_uiSampleRate != 0)
      settings.m_uiTargetSampleRate = pOverride->m_uiSampleRate;
  }

  if (pProfile != nullptr)
  {
    if (const plAudioClipProfileConfig* pConfig = pProfile->GetTypeConfig<plAudioClipProfileConfig>())
    {
      const plUInt32 uiRate = settings.m_uiTargetSampleRate != 0 ? settings.m_uiTargetSampleRate : uiSourceSampleRate;
      if (pConfig->m_uiMaxSampleRate != 0 && uiRate > pConfig->m_uiMaxSampleRate)
        settings.m_uiTargetSampleRate = pConfig->m_uiMaxSampleRate;

      settings.m_uiOpusBitrateKbps = static_cast<plUInt32>(plMath::Clamp(plMath::Round(settings.m_uiOpusBitrateKbps * pConfig->m_fOpusBitrateScale), 6.0f, 510.0f));

      if (pConfig->m_bPreferAdpcmOverPcm && settings.m_eCodec == plAudioClipCodec::Pcm16)
        settings.m_eCodec = plAudioClipCodec::ImaAdpcm;
    }
  }

  settings.m_bForceMono = props.m_bForceMono;
  settings.m_fNormaliseLufs = props.m_fNormaliseLufs;
  settings.m_fTrimSilenceDb = props.m_bTrimSilence ? TrimSilenceDb : -plMath::Infinity<float>();

  if (props.m_bOverrideLoop && uiSourceSampleRate > 0)
  {
    const float fStart = plMath::Max(props.m_fLoopStart, 0.0f);
    settings.m_iLoopStart = static_cast<plInt64>(plMath::Round(static_cast<double>(fStart) * uiSourceSampleRate));

    // an end at or before the start loops to the end of the clip
    settings.m_iLoopEnd = props.m_fLoopEnd > fStart ? static_cast<plInt64>(plMath::Round(static_cast<double>(props.m_fLoopEnd) * uiSourceSampleRate)) : -1;
  }

  return settings;
}

plResult plAudioClipAssetUtils::ReadSourceInfo(plStringView sAbsolutePath, plAudioClipAssetMetaInfo& out_info)
{
  const plStringBuilder sPath = sAbsolutePath;

  plFileStats stats;
  if (plOSFile::GetFileStats(sPath, stats).Failed())
    return PL_FAILURE;

  const plInt64 iModTime = stats.m_LastModificationTime.GetInt64(plSIUnitOfTime::Microsecond);

  {
    PL_LOCK(s_SourceInfoMutex);
    if (const SourceInfoCacheEntry* pEntry = s_SourceInfoCache.GetValue(sPath); pEntry != nullptr && pEntry->m_iModificationTime == iModTime)
    {
      out_info = pEntry->m_Info;
      return pEntry->m_bValid ? PL_SUCCESS : PL_FAILURE;
    }
  }

  SourceInfoCacheEntry entry;
  entry.m_iModificationTime = iModTime;

  {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;

#if PL_ENABLED(PL_PLATFORM_WINDOWS)
    const plStringWChar sPathW(sPath);
    const ma_result initResult = ma_decoder_init_file_w(sPathW.GetData(), &cfg, &decoder);
#else
    const ma_result initResult = ma_decoder_init_file(sPath.GetData(), &cfg, &decoder);
#endif

    if (initResult == MA_SUCCESS)
    {
      ma_format format = ma_format_unknown;
      ma_uint32 uiChannels = 0;
      ma_uint32 uiSampleRate = 0;
      ma_decoder_get_data_format(&decoder, &format, &uiChannels, &uiSampleRate, nullptr, 0);

      ma_uint64 uiFrames = 0;
      ma_decoder_get_length_in_pcm_frames(&decoder, &uiFrames);
      ma_decoder_uninit(&decoder);

      if (uiSampleRate > 0 && uiChannels > 0 && uiChannels <= 255)
      {
        entry.m_bValid = true;
        entry.m_Info.m_uiSampleRate = uiSampleRate;
        entry.m_Info.m_uiChannels = static_cast<plUInt8>(uiChannels);
        entry.m_Info.m_fDuration = static_cast<double>(uiFrames) / uiSampleRate;

        plUInt32 uiLoopStart = 0;
        plUInt32 uiLoopLast = 0;
        if (plPathUtils::HasExtension(sPath, ".wav") && ReadWavSmplLoop(sPath, uiLoopStart, uiLoopLast) && uiLoopLast >= uiLoopStart)
        {
          entry.m_Info.m_bHasLoop = true;
          entry.m_Info.m_fLoopStart = static_cast<double>(uiLoopStart) / uiSampleRate;
          entry.m_Info.m_fLoopEnd = plMath::Min(static_cast<double>(uiLoopLast + 1) / uiSampleRate, entry.m_Info.m_fDuration);
        }
      }
    }
  }

  {
    PL_LOCK(s_SourceInfoMutex);
    s_SourceInfoCache[sPath] = entry;
  }

  out_info = entry.m_Info;
  return entry.m_bValid ? PL_SUCCESS : PL_FAILURE;
}

plStatus plAudioClipAssetUtils::FindOrCreateClip(plStringView sSourceFile, plString& out_sClipGuid)
{
  auto pApp = plQtEditorApp::GetSingleton();

  plStringBuilder sSourceAbs = sSourceFile;
  if (!pApp->MakeDataDirectoryRelativePathAbsolute(sSourceAbs))
    return plStatus(plFmt("Failed to make sound file path absolute: '{0}'", sSourceFile));

  sSourceAbs.MakeCleanPath();

  plStringBuilder sClipPath = sSourceAbs;
  sClipPath.ChangeFileExtension("plAudioClipAsset");

  plStringBuilder sGuid;

  // a clip the curator knows about
  {
    const auto pSubAsset = plAssetCurator::GetSingleton()->FindSubAsset(sClipPath);
    if (pSubAsset.isValid())
    {
      out_sClipGuid = plConversionUtils::ToString(pSubAsset->m_Data.m_Guid, sGuid);
      return plStatus(PL_SUCCESS);
    }
  }

  const plDocumentTypeDescriptor* pTypeDesc = nullptr;
  if (plDocumentManager::FindDocumentTypeFromPath(sClipPath, false, pTypeDesc).Failed())
    return plStatus("The Audio Clip document type is not registered.");

  // a clip that is open, or on disk but not yet picked up by the curator: its document GUID is the asset GUID
  if (plDocument* pOpen = pTypeDesc->m_pManager->GetDocumentByPath(sClipPath))
  {
    out_sClipGuid = plConversionUtils::ToString(pOpen->GetGuid(), sGuid);
    return plStatus(PL_SUCCESS);
  }

  if (plOSFile::ExistsFile(sClipPath))
  {
    plDocument* pDoc = pApp->OpenDocument(sClipPath, plDocumentFlags::None);
    if (pDoc == nullptr)
      return plStatus(plFmt("Could not open the audio clip '{0}'", sClipPath));

    out_sClipGuid = plConversionUtils::ToString(pDoc->GetGuid(), sGuid);
    pDoc->GetDocumentManager()->CloseDocument(pDoc);
    plAssetCurator::GetSingleton()->NotifyOfFileChange(sClipPath);
    return plStatus(PL_SUCCESS);
  }

  plStringBuilder sSourceRel = sSourceAbs;
  if (!pApp->MakePathDataDirectoryRelative(sSourceRel))
    return plStatus(plFmt("The sound file is outside all data directories: '{0}'", sSourceAbs));

  plDocument* pDoc = pApp->CreateDocument(sClipPath, plDocumentFlags::None);
  if (pDoc == nullptr)
    return plStatus(plFmt("Could not create the audio clip '{0}'", sClipPath));

  plAudioClipAssetDocument* pClipDoc = plDynamicCast<plAudioClipAssetDocument*>(pDoc);
  if (pClipDoc == nullptr)
  {
    pDoc->GetDocumentManager()->CloseDocument(pDoc);
    return plStatus(plFmt("'{0}' is not an Audio Clip document", sClipPath));
  }

  {
    plObjectCommandAccessor ca(pClipDoc->GetCommandHistory());
    ca.StartTransaction("Init Values");
    ca.SetValueByName(pClipDoc->GetPropertyObject(), "SourceFile", sSourceRel.GetView()).AssertSuccess();
    ca.FinishTransaction();
  }

  const plStatus saveStatus = pClipDoc->SaveDocument(true);
  out_sClipGuid = plConversionUtils::ToString(pClipDoc->GetGuid(), sGuid);
  pClipDoc->GetDocumentManager()->CloseDocument(pClipDoc);

  if (saveStatus.Failed())
    return plStatus(plFmt("Could not save the audio clip '{0}': {1}", sClipPath, saveStatus.m_sMessage));

  plAssetCurator::GetSingleton()->NotifyOfFileChange(sClipPath);
  plLog::Success("Created audio clip: '{}'", sClipPath);
  return plStatus(PL_SUCCESS);
}

bool plAudioClipAssetUtils::GetClipInfo(plStringView sClipGuid, plAudioClipAssetMetaInfo& out_info, plString* out_pDisplayName)
{
  if (sClipGuid.IsEmpty())
    return false;

  const auto pSubAsset = plAssetCurator::GetSingleton()->FindSubAsset(sClipGuid);
  if (!pSubAsset.isValid() || pSubAsset->m_pAssetInfo == nullptr || pSubAsset->m_pAssetInfo->m_Info == nullptr)
    return false;

  if (out_pDisplayName != nullptr)
    *out_pDisplayName = pSubAsset->GetName();

  const plAudioClipAssetMetaInfo* pMeta = pSubAsset->m_pAssetInfo->m_Info->GetMetaInfo<plAudioClipAssetMetaInfo>();
  if (pMeta == nullptr)
    return false;

  out_info = *pMeta;
  return true;
}

bool plAudioClipAssetUtils::ReadBuiltHeader(plStringView sClipGuid, plAudioClipHeader& out_header, const plPlatformProfile* pProfile)
{
  if (sClipGuid.IsEmpty())
    return false;

  plStringBuilder sOutputFile;
  {
    const auto pSubAsset = plAssetCurator::GetSingleton()->FindSubAsset(sClipGuid);
    if (!pSubAsset.isValid() || pSubAsset->m_pAssetInfo == nullptr || pSubAsset->m_pAssetInfo->m_pDocumentTypeDescriptor == nullptr)
      return false;

    const plAssetDocumentTypeDescriptor* pTypeDesc = pSubAsset->m_pAssetInfo->m_pDocumentTypeDescriptor;
    const auto* pManager = static_cast<const plAssetDocumentManager*>(pTypeDesc->m_pManager);
    sOutputFile = pManager->GetAbsoluteOutputFileName(pTypeDesc, pSubAsset->m_pAssetInfo->m_Path.GetAbsolutePath(), "", pProfile);
  }

  // the built clip starts with the asset file header and the clip header; the payload after them is not read
  plFileReader file;
  if (file.Open(sOutputFile).Failed())
    return false;

  plAssetFileHeader assetHeader;
  if (assetHeader.Read(file).Failed())
    return false;

  return out_header.Read(file).Succeeded();
}

void plAudioClipAssetUtils::MakeMetaInfo(const plAudioClipAssetProperties& props, plAudioClipAssetMetaInfo& out_info)
{
  out_info = plAudioClipAssetMetaInfo();

  plStringBuilder sAbsPath = props.m_sSourceFile;
  if (!sAbsPath.IsEmpty() && plQtEditorApp::GetSingleton()->MakeDataDirectoryRelativePathAbsolute(sAbsPath))
  {
    ReadSourceInfo(sAbsPath, out_info).IgnoreResult();
  }

  out_info.m_sSourceFile = props.m_sSourceFile;

  if (props.m_bOverrideLoop)
  {
    out_info.m_bHasLoop = true;
    out_info.m_fLoopStart = plMath::Max(props.m_fLoopStart, 0.0f);
    out_info.m_fLoopEnd = props.m_fLoopEnd > props.m_fLoopStart ? static_cast<double>(props.m_fLoopEnd) : out_info.m_fDuration;
  }

  // what the bank window estimates the clip's memory from on every platform and in every language
  out_info.m_bForceMono = props.m_bForceMono;

  plAssetCurator* pCurator = plAssetCurator::GetSingleton();
  for (plUInt32 i = 0; i < pCurator->GetNumAssetProfiles(); ++i)
  {
    const plPlatformProfile* pProfile = pCurator->GetAssetProfile(i);
    const plAudioClipBuildSettings settings = MakeBuildSettings(props, pProfile, out_info.m_uiSampleRate);

    plAudioClipPlatformInfo& platform = out_info.m_Platforms.ExpandAndGetRef();
    platform.m_sProfile = pProfile->GetConfigName();
    platform.m_eLoadMode = settings.m_eLoadMode;
    platform.m_eCodec = settings.m_eCodec;
    platform.m_uiSampleRate = settings.m_uiTargetSampleRate;
    platform.m_uiOpusKbps = settings.m_uiOpusBitrateKbps;
  }

  for (const plAudioClipLanguageVariant& variant : props.m_Languages)
  {
    plAudioClipLanguageInfo& language = out_info.m_Languages.ExpandAndGetRef();
    language.m_sLanguage = variant.m_sLanguage;
    language.m_fDuration = out_info.m_fDuration;

    plStringBuilder sVariantPath = variant.m_sFile;
    plAudioClipAssetMetaInfo variantInfo;
    if (!sVariantPath.IsEmpty() && plQtEditorApp::GetSingleton()->MakeDataDirectoryRelativePathAbsolute(sVariantPath) && ReadSourceInfo(sVariantPath, variantInfo).Succeeded())
    {
      language.m_fDuration = variantInfo.m_fDuration;
    }
  }
}

plResult plAudioClipAssetUtils::ReadProperties(plStringView sDocumentPath, plAudioClipAssetProperties& out_props)
{
  plOSFile file;
  PL_SUCCEED_OR_RETURN(file.Open(sDocumentPath, plFileOpenMode::Read));

  plDynamicArray<plUInt8> content;
  file.ReadAll(content);
  file.Close();

  plRawMemoryStreamReader reader(content.GetData(), content.GetCount());

  plUniquePtr<plAbstractObjectGraph> pHeader, pObjects, pTypes;
  PL_SUCCEED_OR_RETURN(plAbstractGraphDdlSerializer::ReadDocument(reader, pHeader, pObjects, pTypes, true));

  const plRTTI* pType = plGetStaticRTTI<plAudioClipAssetProperties>();
  for (auto it = pObjects->GetAllNodes().GetIterator(); it.IsValid(); ++it)
  {
    if (it.Value()->GetType() != pType->GetTypeName())
      continue;

    plRttiConverterContext context;
    plRttiConverterReader converter(pObjects.Borrow(), &context);
    converter.ApplyPropertiesToObject(it.Value(), pType, &out_props);
    return PL_SUCCESS;
  }

  return PL_FAILURE;
}

plAudioClipMemoryEstimate plAudioClipAssetUtils::EstimateMemory(const plAudioClipAssetMetaInfo& info, plStringView sProfile, plStringView sLanguage)
{
  plAudioClipMemoryEstimate estimate;

  // the profile's settings; a profile added after the clip was saved gets the first profile's, a clip saved before
  // they were published the default settings
  plAudioClipPlatformInfo platform;
  platform.m_eLoadMode = plAudioClipLoadMode::Default;
  platform.m_eCodec = plAudioClipCodec::Default;
  estimate.m_bAssumedSettings = info.m_Platforms.IsEmpty();

  if (!info.m_Platforms.IsEmpty())
    platform = info.m_Platforms[0];

  for (const plAudioClipPlatformInfo& candidate : info.m_Platforms)
  {
    if (candidate.m_sProfile.IsEqual_NoCase(sProfile))
      platform = candidate;
  }

  // the builder pads every language to the longest one
  double fDuration = info.m_fDuration;
  estimate.m_bLocalized = !info.m_Languages.IsEmpty();
  estimate.m_bHasLanguage = sLanguage.IsEmpty() || !estimate.m_bLocalized;

  for (const plAudioClipLanguageInfo& language : info.m_Languages)
  {
    fDuration = plMath::Max(fDuration, language.m_fDuration);
    if (language.m_sLanguage.IsEqual_NoCase(sLanguage))
      estimate.m_bHasLanguage = true;
  }

  const plAudioClipCodec::Enum codec = platform.m_eCodec;
  plUInt64 uiChannels = info.m_bForceMono ? 1 : plMath::Max<plUInt64>(info.m_uiChannels, 1);
  plUInt64 uiRate = platform.m_uiSampleRate != 0 ? platform.m_uiSampleRate : info.m_uiSampleRate;

  if (codec == plAudioClipCodec::Opus)
  {
    uiRate = 48000;
    uiChannels = plMath::Min<plUInt64>(uiChannels, 2);
  }

  const plUInt64 uiFrames = static_cast<plUInt64>(plMath::Ceil(fDuration * uiRate));
  const plUInt64 uiPcmBytes = uiFrames * uiChannels * sizeof(plInt16);

  plUInt64 uiPayloadBytes = uiPcmBytes;
  if (codec == plAudioClipCodec::ImaAdpcm)
  {
    uiPayloadBytes = (uiFrames + plImaAdpcmCodec::FramesPerBlock - 1) / plImaAdpcmCodec::FramesPerBlock * plImaAdpcmCodec::GetBlockBytes(static_cast<plUInt32>(uiChannels));
  }
  else if (codec == plAudioClipCodec::Opus)
  {
    // the bitrate, plus the length prefix of every 20 ms packet
    const plUInt64 uiPackets = static_cast<plUInt64>(plMath::Ceil(fDuration * 50.0));
    uiPayloadBytes = static_cast<plUInt64>(fDuration * platform.m_uiOpusKbps * 125.0) + uiPackets * 2;
  }

  estimate.m_eLoadMode = platform.m_eLoadMode;
  switch (platform.m_eLoadMode)
  {
    case plAudioClipLoadMode::DecompressOnLoad:
      estimate.m_uiResidentBytes = uiPcmBytes;
      break;
    case plAudioClipLoadMode::CompressedInMemory:
      estimate.m_uiResidentBytes = uiPayloadBytes;
      break;
    case plAudioClipLoadMode::Streamed:
      estimate.m_uiResidentBytes = plMath::Min<plUInt64>(plAudioClipConstants::PrimingBytes, uiPayloadBytes) * (info.m_bHasLoop ? 2 : 1);
      break;
  }

  return estimate;
}

plAudioClipMemoryEstimate plAudioClipAssetUtils::EstimateMemory(const plAudioClipHeader& header, plStringView sLanguage)
{
  plAudioClipMemoryEstimate estimate;
  estimate.m_eLoadMode = header.m_eLoadMode;
  estimate.m_bLocalized = header.m_Languages.GetCount() > 1;
  estimate.m_bHasLanguage = sLanguage.IsEmpty() || !estimate.m_bLocalized;

  if (header.m_Languages.IsEmpty())
    return estimate;

  const plAudioClipLanguageEntry* pLanguage = &header.m_Languages[0];
  for (const plAudioClipLanguageEntry& language : header.m_Languages)
  {
    if (!sLanguage.IsEmpty() && language.m_sLanguage.IsEqual_NoCase(sLanguage))
    {
      pLanguage = &language;
      estimate.m_bHasLanguage = true;
    }
  }

  switch (header.m_eLoadMode)
  {
    case plAudioClipLoadMode::DecompressOnLoad:
      // every language is padded to the longest one
      estimate.m_uiResidentBytes = header.m_uiFrames * header.m_uiChannels * sizeof(plInt16);
      break;
    case plAudioClipLoadMode::CompressedInMemory:
      estimate.m_uiResidentBytes = pLanguage->m_uiPayloadBytes;
      break;
    case plAudioClipLoadMode::Streamed:
      estimate.m_uiResidentBytes = static_cast<plUInt64>(pLanguage->m_uiPrimingBytes) + pLanguage->m_uiLoopPrimingBytes;
      break;
  }

  return estimate;
}

//////////////////////////////////////////////////////////////////////////
// plAudioClipAssetProperties
//////////////////////////////////////////////////////////////////////////

void plAudioClipAssetProperties::PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  const plRTTI* pType = e.m_pObject->GetTypeAccessor().GetType();

  if (pType == plGetStaticRTTI<plAudioClipAssetProperties>())
  {
    auto& props = *e.m_pPropertyStates;

    const plInt64 iCodec = e.m_pObject->GetTypeAccessor().GetValue("Codec").ConvertTo<plInt64>();
    props["OpusKbps"].m_Visibility = iCodec == plAudioClipCodec::Opus ? plPropertyUiState::Default : plPropertyUiState::Invisible;

    const bool bOverrideLoop = e.m_pObject->GetTypeAccessor().GetValue("OverrideLoop").ConvertTo<bool>();
    props["LoopStart"].m_Visibility = bOverrideLoop ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["LoopEnd"].m_Visibility = bOverrideLoop ? plPropertyUiState::Default : plPropertyUiState::Invisible;

    // edited through the platform tabs of the Audio Clip window
    props["PlatformOverrides"].m_Visibility = plPropertyUiState::Invisible;
  }

  if (pType == plGetStaticRTTI<plAudioClipPlatformOverride>())
  {
    auto& props = *e.m_pPropertyStates;

    const plInt64 iCodec = e.m_pObject->GetTypeAccessor().GetValue("Codec").ConvertTo<plInt64>();
    props["OpusKbps"].m_Visibility = iCodec == plAudioClipCodec::Opus ? plPropertyUiState::Default : plPropertyUiState::Invisible;

    // the profile is the tab the override is shown in
    props["Profile"].m_Visibility = plPropertyUiState::Disabled;
  }
}

//////////////////////////////////////////////////////////////////////////
// plAudioClipAssetDocument
//////////////////////////////////////////////////////////////////////////

plAudioClipAssetDocument::plAudioClipAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plAudioClipAssetProperties>(sDocumentPath, plAssetDocEngineConnection::Simple)
{
}

void plAudioClipAssetDocument::HandleEngineMessage(const plEditorEngineDocumentMsg* pMsg)
{
  if (const auto* pStatus = plDynamicCast<const plAudioAuditionStatusMsgToEditor*>(pMsg))
  {
    m_AuditionStatus = *pStatus;
    m_AuditionEvents.Broadcast(m_AuditionStatus);
    return;
  }

  plSimpleAssetDocument<plAudioClipAssetProperties>::HandleEngineMessage(pMsg);
}

plStatus plAudioClipAssetDocument::WriteAuditionData(plStreamWriter& inout_stream, bool bLoop) const
{
  // what an event resource file holds: its path, the asset file header, then the event
  plStringBuilder sPath = GetDocumentPath();
  sPath.ChangeFileExtension("plAudioEvent");
  inout_stream << sPath;

  plAssetFileHeader header;
  header.SetFileHashAndVersion(plAssetCurator::GetSingleton()->GetAssetTransformHash(GetGuid()), GetAssetTypeVersion());
  PL_SUCCEED_OR_RETURN(header.Write(inout_stream));

  // one instrument with one entry: the built clip, as the game plays it
  plAudioEventDesc desc;
  desc.m_bSpatialize = false;
  desc.m_StopFadeOut = plTime::MakeFromMilliseconds(20);

  plAudioEventInstrumentDesc& instrument = desc.m_Instruments.ExpandAndGetRef();
  instrument.m_sName = "Clip";
  instrument.m_bLoop = bLoop;

  plStringBuilder sGuid;
  plConversionUtils::ToString(GetGuid(), sGuid);
  instrument.m_Playlist.ExpandAndGetRef().m_sClip = sGuid;

  PL_SUCCEED_OR_RETURN(desc.Save(inout_stream));

  // no embedded samples
  inout_stream << plUInt32(0);
  return plStatus(PL_SUCCESS);
}

void plAudioClipAssetDocument::UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const
{
  SUPER::UpdateAssetDocumentInfo(pInfo);

  const plAudioClipAssetProperties* pProp = GetProperties();

  if (!pProp->m_sSourceFile.IsEmpty())
    pInfo->m_TransformDependencies.Insert(pProp->m_sSourceFile);

  for (const plAudioClipLanguageVariant& variant : pProp->m_Languages)
  {
    if (!variant.m_sFile.IsEmpty())
      pInfo->m_TransformDependencies.Insert(variant.m_sFile);
  }

  plAudioClipAssetMetaInfo* pMeta = PL_DEFAULT_NEW(plAudioClipAssetMetaInfo);
  plAudioClipAssetUtils::MakeMetaInfo(*pProp, *pMeta);
  pInfo->m_MetaInfo.PushBack(pMeta);
}

plTransformStatus plAudioClipAssetDocument::InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  const plAudioClipAssetProperties* pProp = GetProperties();

  if (pProp->m_sSourceFile.IsEmpty())
    return plStatus("The audio clip has no source file.");

  auto DecodeFile = [](plStringView sFile, plAudioClipSourcePcm& out_source) -> plStatus
  {
    plStringBuilder sAbsPath = sFile;
    if (!plQtEditorApp::GetSingleton()->MakeDataDirectoryRelativePathAbsolute(sAbsPath))
      return plStatus(plFmt("Failed to make sound file path absolute: '{0}'", sFile));

    if (plAudioClipBuilder::DecodeSourceFile(sAbsPath, out_source).Failed())
      return plStatus(plFmt("Could not decode sound file '{0}'", sAbsPath));

    return plStatus(PL_SUCCESS);
  };

  plDynamicArray<plAudioClipSourcePcm> sources;
  sources.Reserve(1 + pProp->m_Languages.GetCount());

  PL_SUCCEED_OR_RETURN(DecodeFile(pProp->m_sSourceFile, sources.ExpandAndGetRef()));

  plAudioClipBuildSettings settings = plAudioClipAssetUtils::MakeBuildSettings(*pProp, pAssetProfile, sources[0].m_uiSampleRate);

  if (!pProp->m_Languages.IsEmpty())
  {
    settings.m_Languages.PushBack(plString());

    for (const plAudioClipLanguageVariant& variant : pProp->m_Languages)
    {
      if (variant.m_sLanguage.IsEmpty())
        return plStatus("A language variant has no language name.");

      if (variant.m_sFile.IsEmpty())
        return plStatus(plFmt("Language variant '{0}' has no sound file.", variant.m_sLanguage));

      if (settings.m_Languages.Contains(variant.m_sLanguage))
        return plStatus(plFmt("Language '{0}' is listed more than once.", variant.m_sLanguage));

      PL_SUCCEED_OR_RETURN(DecodeFile(variant.m_sFile, sources.ExpandAndGetRef()));
      settings.m_Languages.PushBack(variant.m_sLanguage);
    }
  }

  plHybridArray<const plAudioClipSourcePcm*, 4> sourcePtrs;
  for (const plAudioClipSourcePcm& source : sources)
    sourcePtrs.PushBack(&source);

  if (plAudioClipBuilder::Build(plArrayPtr<const plAudioClipSourcePcm* const>(sourcePtrs.GetData(), sourcePtrs.GetCount()), settings, stream).Failed())
    return plStatus("Building the audio clip failed, see the log for details.");

  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////
// plAudioClipAssetDocumentGenerator
//////////////////////////////////////////////////////////////////////////

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioClipAssetDocumentGenerator, 1, plRTTIDefaultAllocator<plAudioClipAssetDocumentGenerator>)
PL_END_DYNAMIC_REFLECTED_TYPE;

plAudioClipAssetDocumentGenerator::plAudioClipAssetDocumentGenerator()
{
  AddSupportedFileType("wav");
  AddSupportedFileType("mp3");
  AddSupportedFileType("flac");
}

plAudioClipAssetDocumentGenerator::~plAudioClipAssetDocumentGenerator() = default;

void plAudioClipAssetDocumentGenerator::GetImportModes(plStringView sAbsInputFile, plDynamicArray<plAssetDocumentGenerator::ImportMode>& out_modes) const
{
  {
    plAssetDocumentGenerator::ImportMode& info = out_modes.ExpandAndGetRef();
    info.m_Priority = plAssetDocGeneratorPriority::HighPriority;
    info.m_sName = "Audio_Clip";
    info.m_sIcon = ":/AssetIcons/AudioClip.svg";
  }

  {
    plAssetDocumentGenerator::ImportMode& info = out_modes.ExpandAndGetRef();
    info.m_Priority = plAssetDocGeneratorPriority::DefaultPriority;
    info.m_sName = "Audio_Clip_And_Event";
    info.m_sIcon = ":/AssetIcons/AudioEvent.svg";
  }
}

plStatus plAudioClipAssetDocumentGenerator::Generate(plStringView sInputFileAbs, plStringView sMode, plDynamicArray<plDocument*>& out_generatedDocuments)
{
  plStringBuilder sOutFile = sInputFileAbs;
  sOutFile.ChangeFileExtension(GetDocumentExtension());

  plString sClipGuid;

  if (plOSFile::ExistsFile(sOutFile))
  {
    plLog::Info("Skipping audio clip import, file has been imported before: '{}'", sOutFile);
  }
  else
  {
    auto pApp = plQtEditorApp::GetSingleton();

    plStringBuilder sInputFileRel = sInputFileAbs;
    pApp->MakePathDataDirectoryRelative(sInputFileRel);

    plDocument* pDoc = pApp->CreateDocument(sOutFile, plDocumentFlags::None);
    if (pDoc == nullptr)
      return plStatus("Could not create target document");

    out_generatedDocuments.PushBack(pDoc);

    plAudioClipAssetDocument* pAssetDoc = plDynamicCast<plAudioClipAssetDocument*>(pDoc);
    if (pAssetDoc == nullptr)
      return plStatus("Target document is not a valid plAudioClipAssetDocument");

    plObjectCommandAccessor ca(pAssetDoc->GetCommandHistory());
    ca.StartTransaction("Init Values");
    ca.SetValueByName(pAssetDoc->GetPropertyObject(), "SourceFile", sInputFileRel.GetView()).AssertSuccess();
    ca.FinishTransaction();

    plStringBuilder sGuid;
    sClipGuid = plConversionUtils::ToString(pAssetDoc->GetGuid(), sGuid);

    plLog::Success("Imported audio clip: '{}'", sOutFile);
  }

  if (sMode == "Audio_Clip_And_Event")
  {
    // the clip imported before: look up its GUID
    if (sClipGuid.IsEmpty())
      PL_SUCCEED_OR_RETURN(plAudioClipAssetUtils::FindOrCreateClip(sInputFileAbs, sClipGuid));

    // the event's playlist entry references the clip
    PL_SUCCEED_OR_RETURN(plAudioEventAssetDocumentGenerator::GenerateForClip(sInputFileAbs, sClipGuid, out_generatedDocuments));
  }

  return plStatus(PL_SUCCESS);
}
