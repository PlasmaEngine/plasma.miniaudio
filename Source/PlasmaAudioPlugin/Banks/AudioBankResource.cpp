#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/Containers/HashSet.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <PlasmaAudioPlugin/Banks/AudioBankResource.h>

static constexpr plTypeVersion kAudioBankDescVersion = 2;

namespace
{
  // A corrupt count or string length must not allocate an unbounded amount of memory.
  constexpr plUInt32 MaxBankItems = 65536;
  constexpr plUInt32 MaxResourceIdBytes = 4096;

  plResult ReadBankAssetHeader(plStreamReader& stream, plAssetFileHeader& out_header)
  {
    // plAssetFileHeader::Read asserts on malformed input. Banks are external files: validate the current
    // asset-header format (v3: tag, hash, asset version, generator) with checked reads instead.
    const plUInt8 expectedTag[] = {'p', 'l', 'A', 's', 's', 'e', 't', 3};
    plUInt8 tag[PL_ARRAY_SIZE(expectedTag)];
    if (stream.ReadBytes(tag, sizeof(tag)) != sizeof(tag) || !plMemoryUtils::IsEqual(tag, expectedTag, PL_ARRAY_SIZE(tag)))
      return PL_FAILURE;

    plUInt64 uiHash = 0;
    plUInt16 uiVersion = 0;
    plString sGenerator;
    PL_SUCCEED_OR_RETURN(stream.ReadQWordValue(&uiHash));
    PL_SUCCEED_OR_RETURN(stream.ReadWordValue(&uiVersion));
    PL_SUCCEED_OR_RETURN(stream.ReadString(sGenerator, MaxResourceIdBytes));
    if (uiHash == plMath::MaxValue<plUInt64>())
      return PL_FAILURE;

    out_header.SetFileHashAndVersion(uiHash, uiVersion);
    out_header.SetGenerator(sGenerator);
    return PL_SUCCESS;
  }

  plResult ReadResourceList(plStreamReader& stream, plDynamicArray<plString>& out_resources)
  {
    plUInt32 uiCount = 0;
    PL_SUCCEED_OR_RETURN(stream.ReadDWordValue(&uiCount));
    if (uiCount > MaxBankItems)
      return PL_FAILURE;

    out_resources.SetCount(uiCount);
    for (plString& sResource : out_resources)
    {
      PL_SUCCEED_OR_RETURN(stream.ReadString(sResource, MaxResourceIdBytes));
      if (sResource.IsEmpty())
        return PL_FAILURE;
    }
    return PL_SUCCESS;
  }

  /// Forwards reads to the file and counts the bytes, so the loader knows where the packed data starts.
  class CountingReader final : public plStreamReader
  {
  public:
    explicit CountingReader(plStreamReader& ref_source)
      : m_Source(ref_source)
    {
    }

    virtual plUInt64 ReadBytes(void* pBuffer, plUInt64 uiBytesToRead) override
    {
      const plUInt64 uiRead = m_Source.ReadBytes(pBuffer, uiBytesToRead);
      m_uiPosition += uiRead;
      return uiRead;
    }

    plUInt64 m_uiPosition = 0;

  private:
    plStreamReader& m_Source;
  };

  struct BankLoadData
  {
    plDefaultMemoryStreamStorage m_Storage;
    plMemoryStreamReader m_Reader;
  };

  /// "960 B", "12.5 KB", "3.5 MB"
  void AppendSize(plStringBuilder& inout_sText, plUInt64 uiBytes)
  {
    if (uiBytes < 1024)
      inout_sText.AppendFormat("{} B", uiBytes);
    else if (uiBytes < 1024 * 1024)
      inout_sText.AppendFormat("{} KB", plArgF(uiBytes / 1024.0, 1));
    else
      inout_sText.AppendFormat("{} MB", plArgF(uiBytes / (1024.0 * 1024.0), 1));
  }

  void AppendExcess(plStringBuilder& inout_sText, plStringView sCategory, plUInt64 uiUsed, plUInt64 uiLimit)
  {
    if (uiLimit == 0 || uiUsed <= uiLimit)
      return;

    if (!inout_sText.IsEmpty())
      inout_sText.Append(", ");

    inout_sText.AppendFormat("{} ", sCategory);
    AppendSize(inout_sText, uiUsed);
    inout_sText.Append(" of ");
    AppendSize(inout_sText, uiLimit);
  }
} // namespace

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioBankResource, 1, plRTTIDefaultAllocator<plAudioBankResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plAudioBankResource);

void plAudioMemoryUsage::DescribeExcess(const plAudioMemoryBudget& budget, plStringBuilder& out_sText) const
{
  out_sText.Clear();
  AppendExcess(out_sText, "decoded", m_uiDecodedBytes, budget.m_uiDecodedBytes);
  AppendExcess(out_sText, "compressed", m_uiCompressedBytes, budget.m_uiCompressedBytes);
  AppendExcess(out_sText, "streaming", GetStreamingBytes(), budget.m_uiStreamingBytes);
  AppendExcess(out_sText, "total", GetTotalBytes(), budget.m_uiTotalBytes);
}

plResult plAudioBankDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream.WriteVersion(kAudioBankDescVersion);

  inout_stream << m_Events.GetCount();
  for (const plString& sEvent : m_Events)
    inout_stream << sEvent;

  inout_stream << m_Clips.GetCount();
  for (const plString& sClip : m_Clips)
    inout_stream << sClip;

  inout_stream << m_Budget.m_uiDecodedBytes;
  inout_stream << m_Budget.m_uiCompressedBytes;
  inout_stream << m_Budget.m_uiStreamingBytes;
  inout_stream << m_Budget.m_uiTotalBytes;

  inout_stream << m_Packed.GetCount();
  for (const plAudioBankPackedEntry& entry : m_Packed)
  {
    inout_stream << entry.m_sResourceId;
    inout_stream << static_cast<plUInt8>(entry.m_bClip ? 1 : 0);
    inout_stream << entry.m_uiOffset;
    inout_stream << entry.m_uiBytes;
  }

  return PL_SUCCESS;
}

plResult plAudioBankDesc::Load(plStreamReader& inout_stream)
{
  plTypeVersion uiVersion = 0;
  PL_SUCCEED_OR_RETURN(inout_stream.ReadWordValue(&uiVersion));
  if (uiVersion == 0 || uiVersion > kAudioBankDescVersion)
    return PL_FAILURE;

  plAudioBankDesc loaded;
  PL_SUCCEED_OR_RETURN(ReadResourceList(inout_stream, loaded.m_Events));
  PL_SUCCEED_OR_RETURN(ReadResourceList(inout_stream, loaded.m_Clips));

  if (uiVersion >= 2)
  {
    PL_SUCCEED_OR_RETURN(inout_stream.ReadQWordValue(&loaded.m_Budget.m_uiDecodedBytes));
    PL_SUCCEED_OR_RETURN(inout_stream.ReadQWordValue(&loaded.m_Budget.m_uiCompressedBytes));
    PL_SUCCEED_OR_RETURN(inout_stream.ReadQWordValue(&loaded.m_Budget.m_uiStreamingBytes));
    PL_SUCCEED_OR_RETURN(inout_stream.ReadQWordValue(&loaded.m_Budget.m_uiTotalBytes));

    plUInt32 uiCount = 0;
    PL_SUCCEED_OR_RETURN(inout_stream.ReadDWordValue(&uiCount));
    if (uiCount > MaxBankItems)
      return PL_FAILURE;

    loaded.m_Packed.SetCount(uiCount);
    for (plAudioBankPackedEntry& entry : loaded.m_Packed)
    {
      plUInt8 uiClip = 0;
      PL_SUCCEED_OR_RETURN(inout_stream.ReadString(entry.m_sResourceId, MaxResourceIdBytes));
      if (inout_stream.ReadBytes(&uiClip, sizeof(uiClip)) != sizeof(uiClip) || uiClip > 1)
        return PL_FAILURE;
      PL_SUCCEED_OR_RETURN(inout_stream.ReadQWordValue(&entry.m_uiOffset));
      PL_SUCCEED_OR_RETURN(inout_stream.ReadQWordValue(&entry.m_uiBytes));
      entry.m_bClip = uiClip != 0;
    }
  }

  PL_SUCCEED_OR_RETURN(loaded.Validate());
  *this = std::move(loaded);
  return PL_SUCCESS;
}

plResult plAudioBankDesc::Validate() const
{
  if (m_Events.GetCount() > MaxBankItems || m_Clips.GetCount() > MaxBankItems || m_Packed.GetCount() > MaxBankItems)
    return PL_FAILURE;

  for (const plString& sId : m_Events)
  {
    if (sId.IsEmpty() || sId.GetElementCount() > MaxResourceIdBytes)
      return PL_FAILURE;
  }
  for (const plString& sId : m_Clips)
  {
    if (sId.IsEmpty() || sId.GetElementCount() > MaxResourceIdBytes)
      return PL_FAILURE;
  }

  plHashSet<plString> packedIds;
  for (const plAudioBankPackedEntry& entry : m_Packed)
  {
    const auto& resources = entry.m_bClip ? m_Clips : m_Events;
    if (!resources.Contains(entry.m_sResourceId) || packedIds.Insert(entry.m_sResourceId) || entry.m_uiBytes == 0 ||
        entry.m_uiOffset > plMath::MaxValue<plUInt64>() - entry.m_uiBytes)
      return PL_FAILURE;
  }

  return PL_SUCCESS;
}

plAudioBankResource::plAudioBankResource()
  : plResource(DoUpdate::OnAnyThread, 1)
{
  ModifyMemoryUsage().m_uiMemoryCPU = sizeof(plAudioBankResource);
}

plAudioBankResource::~plAudioBankResource() = default;

plResourceLoadDesc plAudioBankResource::UnloadData(Unload WhatToUnload)
{
  PL_IGNORE_UNUSED(WhatToUnload);

  m_Desc = plAudioBankDesc();
  m_sFilePath.Clear();
  m_uiPackedDataOffset = 0;

  ModifyMemoryUsage().m_uiMemoryCPU = sizeof(plAudioBankResource);

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Unloaded;
  return res;
}

plResourceLoadDesc plAudioBankResource::UpdateContent(plStreamReader* pStream)
{
  PL_LOG_BLOCK("plAudioBankResource::UpdateContent", GetResourceIdOrDescription());

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::LoadedResourceMissing;

  if (pStream == nullptr)
    return res;

  // see plAudioBankResourceLoader
  if (pStream->ReadString(m_sFilePath, MaxResourceIdBytes).Failed() || pStream->ReadQWordValue(&m_uiPackedDataOffset).Failed())
    return res;

  plAssetFileHeader assetHeader;
  if (ReadBankAssetHeader(*pStream, assetHeader).Failed())
    return res;

  m_Desc = plAudioBankDesc();
  if (m_Desc.Load(*pStream).Failed())
    return res;

  res.m_State = plResourceState::Loaded;
  return res;
}

void plAudioBankResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plAudioBankResource);
  out_NewMemoryUsage.m_uiMemoryCPU += m_Desc.m_Events.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryCPU += m_Desc.m_Clips.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryCPU += m_Desc.m_Packed.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryGPU = 0;
}

PL_RESOURCE_IMPLEMENT_CREATEABLE(plAudioBankResource, plAudioBankResourceDescriptor)
{
  m_Desc = std::move(descriptor.m_Desc);
  m_sFilePath.Clear();
  m_uiPackedDataOffset = 0;

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  // A descriptor-created resource has no file from which packed entries could be read.
  res.m_State = m_Desc.Validate().Succeeded() && m_Desc.m_Packed.IsEmpty() ? plResourceState::Loaded : plResourceState::LoadedResourceMissing;
  return res;
}

//////////////////////////////////////////////////////////////////////////
// plAudioBankResourceLoader

plResourceLoadData plAudioBankResourceLoader::OpenDataStream(const plResource* pResource)
{
  PL_PROFILE_SCOPE("ReadAudioBank");

  plResourceLoadData res;

  plFileReader file;
  if (file.Open(pResource->GetResourceID().GetData()).Failed())
    return res;

  res.m_sResourceDescription = file.GetFilePathRelative().GetData();

#if PL_ENABLED(PL_SUPPORTS_FILE_STATS)
  plFileStats stat;
  if (plFileSystem::GetFileStats(pResource->GetResourceID(), stat).Succeeded())
  {
    res.m_LoadedFileModificationDate = stat.m_LastModificationTime;
  }
#endif

  // the header and the desc; the packed files after them are read by the loaders of the events and clips
  CountingReader reader(file);

  plAssetFileHeader assetHeader;
  plAudioBankDesc desc;
  if (ReadBankAssetHeader(reader, assetHeader).Failed() || desc.Load(reader).Failed())
    return res;

  const plUInt64 uiFileBytes = file.GetFileSize();
  if (reader.m_uiPosition > uiFileBytes)
    return res;
  const plUInt64 uiPackedBytes = uiFileBytes - reader.m_uiPosition;
  for (const plAudioBankPackedEntry& entry : desc.m_Packed)
  {
    if (entry.m_uiOffset > uiPackedBytes || entry.m_uiBytes > uiPackedBytes - entry.m_uiOffset)
      return res;
  }

  BankLoadData* pData = PL_DEFAULT_NEW(BankLoadData);
  {
    plMemoryStreamWriter w(&pData->m_Storage);
    w << pResource->GetResourceID();
    w << reader.m_uiPosition;
    assetHeader.Write(w).AssertSuccess();
    desc.Save(w).AssertSuccess();
  }

  pData->m_Reader.SetStorage(&pData->m_Storage);

  res.m_pDataStream = &pData->m_Reader;
  res.m_pCustomLoaderData = pData;
  return res;
}

void plAudioBankResourceLoader::CloseDataStream(const plResource* pResource, const plResourceLoadData& loaderData)
{
  PL_IGNORE_UNUSED(pResource);

  BankLoadData* pData = static_cast<BankLoadData*>(loaderData.m_pCustomLoaderData);
  PL_DEFAULT_DELETE(pData);
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Banks_AudioBankResource);
