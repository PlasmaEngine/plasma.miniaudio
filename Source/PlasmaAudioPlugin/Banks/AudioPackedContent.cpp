#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Core/ResourceManager/Resource.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/Mutex.h>
#include <PlasmaAudioPlugin/Banks/AudioBankResource.h>
#include <PlasmaAudioPlugin/Banks/AudioPackedContent.h>

namespace
{
  struct Entry
  {
    plUInt64 m_uiOwner = 0;
    plString m_sResourceId;
    plAudioPackedContent::Location m_Location;
  };

  plMutex s_Mutex;
  plDynamicArray<Entry> s_Entries; ///< in registration order, so the first bank that packs a resource wins

  /// The packed event's stream, or what the file loader returned for an event that is not packed.
  struct EventLoadData
  {
    plResourceLoadData m_FileData;
    plDefaultMemoryStreamStorage m_Storage;
    plMemoryStreamReader m_Reader;
  };
} // namespace

void plAudioPackedContent::Register(plUInt64 uiOwner, const plAudioBankResource& bank)
{
  const plAudioBankDesc& desc = bank.GetDescriptor();
  if (desc.m_Packed.IsEmpty() || bank.GetFilePath().IsEmpty())
    return;

  PL_LOCK(s_Mutex);

  for (const plAudioBankPackedEntry& packed : desc.m_Packed)
  {
    Entry& entry = s_Entries.ExpandAndGetRef();
    entry.m_uiOwner = uiOwner;
    entry.m_sResourceId = packed.m_sResourceId;
    entry.m_Location.m_sFile = bank.GetFilePath();
    entry.m_Location.m_uiOffset = bank.GetPackedDataOffset() + packed.m_uiOffset;
    entry.m_Location.m_uiBytes = packed.m_uiBytes;
  }
}

void plAudioPackedContent::Unregister(plUInt64 uiOwner)
{
  PL_LOCK(s_Mutex);

  for (plUInt32 i = s_Entries.GetCount(); i > 0; --i)
  {
    if (s_Entries[i - 1].m_uiOwner == uiOwner)
      s_Entries.RemoveAtAndCopy(i - 1);
  }
}

bool plAudioPackedContent::Find(plStringView sResourceId, Location& out_location)
{
  PL_LOCK(s_Mutex);

  for (const Entry& entry : s_Entries)
  {
    if (entry.m_sResourceId == sResourceId)
    {
      out_location = entry.m_Location;
      return true;
    }
  }

  return false;
}

plUInt32 plAudioPackedContent::GetCount()
{
  PL_LOCK(s_Mutex);
  return s_Entries.GetCount();
}

//////////////////////////////////////////////////////////////////////////
// plAudioEventResourceLoader

plResourceLoadData plAudioEventResourceLoader::OpenDataStream(const plResource* pResource)
{
  plAudioPackedContent::Location location;
  if (!plAudioPackedContent::Find(pResource->GetResourceID(), location))
  {
    EventLoadData* pData = PL_DEFAULT_NEW(EventLoadData);
    pData->m_FileData = plResourceLoaderFromFile::OpenDataStream(pResource);

    plResourceLoadData res = pData->m_FileData;
    res.m_pCustomLoaderData = pData;
    return res;
  }

  PL_PROFILE_SCOPE("ReadPackedAudioEvent");

  plResourceLoadData res;

  plFileReader file;
  if (file.Open(location.m_sFile).Failed() || file.SkipBytes(location.m_uiOffset) != location.m_uiOffset)
    return res;

  res.m_sResourceDescription = file.GetFilePathRelative().GetData();

#if PL_ENABLED(PL_SUPPORTS_FILE_STATS)
  plFileStats stat;
  if (plFileSystem::GetFileStats(location.m_sFile, stat).Succeeded())
  {
    res.m_LoadedFileModificationDate = stat.m_LastModificationTime;
  }
#endif

  // the stream of plResourceLoaderFromFile: the file path, then the event's built file
  EventLoadData* pData = PL_DEFAULT_NEW(EventLoadData);
  {
    plMemoryStreamWriter w(&pData->m_Storage);
    w << file.GetFilePathAbsolute();

    plUInt8 buffer[4096];
    plUInt64 uiLeft = location.m_uiBytes;
    while (uiLeft > 0)
    {
      const plUInt64 uiChunk = plMath::Min<plUInt64>(uiLeft, sizeof(buffer));
      if (file.ReadBytes(buffer, uiChunk) != uiChunk)
      {
        PL_DEFAULT_DELETE(pData);
        return res;
      }

      w.WriteBytes(buffer, uiChunk).AssertSuccess();
      uiLeft -= uiChunk;
    }
  }

  pData->m_Reader.SetStorage(&pData->m_Storage);

  res.m_pDataStream = &pData->m_Reader;
  res.m_pCustomLoaderData = pData;
  return res;
}

void plAudioEventResourceLoader::CloseDataStream(const plResource* pResource, const plResourceLoadData& loaderData)
{
  EventLoadData* pData = static_cast<EventLoadData*>(loaderData.m_pCustomLoaderData);
  if (pData == nullptr)
    return;

  if (pData->m_FileData.m_pCustomLoaderData != nullptr)
    plResourceLoaderFromFile::CloseDataStream(pResource, pData->m_FileData);

  PL_DEFAULT_DELETE(pData);
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Banks_AudioPackedContent);
