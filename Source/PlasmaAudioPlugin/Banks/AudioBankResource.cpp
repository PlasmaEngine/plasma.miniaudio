#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <PlasmaAudioPlugin/Banks/AudioBankResource.h>

static constexpr plTypeVersion kAudioBankDescVersion = 2;

namespace
{
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
  const plTypeVersion uiVersion = inout_stream.ReadVersion(kAudioBankDescVersion);

  plUInt32 uiCount = 0;

  inout_stream >> uiCount;
  m_Events.SetCount(uiCount);
  for (plString& sEvent : m_Events)
    inout_stream >> sEvent;

  inout_stream >> uiCount;
  m_Clips.SetCount(uiCount);
  for (plString& sClip : m_Clips)
    inout_stream >> sClip;

  m_Budget = plAudioMemoryBudget();
  m_Packed.Clear();

  if (uiVersion < 2)
    return PL_SUCCESS;

  inout_stream >> m_Budget.m_uiDecodedBytes;
  inout_stream >> m_Budget.m_uiCompressedBytes;
  inout_stream >> m_Budget.m_uiStreamingBytes;
  inout_stream >> m_Budget.m_uiTotalBytes;

  inout_stream >> uiCount;
  m_Packed.SetCount(uiCount);
  for (plAudioBankPackedEntry& entry : m_Packed)
  {
    plUInt8 uiClip = 0;
    inout_stream >> entry.m_sResourceId;
    inout_stream >> uiClip;
    inout_stream >> entry.m_uiOffset;
    inout_stream >> entry.m_uiBytes;
    entry.m_bClip = uiClip != 0;
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
  (*pStream) >> m_sFilePath;
  (*pStream) >> m_uiPackedDataOffset;

  plAssetFileHeader assetHeader;
  if (assetHeader.Read(*pStream).Failed())
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
  res.m_State = plResourceState::Loaded;
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
  if (assetHeader.Read(reader).Failed() || desc.Load(reader).Failed())
    return res;

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
