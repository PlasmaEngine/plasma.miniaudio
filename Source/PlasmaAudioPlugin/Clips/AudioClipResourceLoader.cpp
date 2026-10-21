#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Core/ResourceManager/Resource.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <PlasmaAudioPlugin/Banks/AudioPackedContent.h>
#include <PlasmaAudioPlugin/Clips/AudioClipDesc.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResourceLoader.h>

namespace
{
  plMutex s_LanguageMutex;
  plString s_sLanguage;
  plAtomicInteger32 s_iLanguageGeneration; ///< incremented under s_LanguageMutex whenever s_sLanguage changes

  /// Forwards reads to the file, counts the bytes and optionally keeps a copy of them.
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

      if (m_pCapture != nullptr && uiRead > 0)
      {
        const plUInt32 uiOld = m_pCapture->GetCount();
        m_pCapture->SetCountUninitialized(uiOld + static_cast<plUInt32>(uiRead));
        plMemoryUtils::Copy(m_pCapture->GetData() + uiOld, static_cast<const plUInt8*>(pBuffer), static_cast<size_t>(uiRead));
      }

      return uiRead;
    }

    virtual plUInt64 SkipBytes(plUInt64 uiBytesToSkip) override
    {
      PL_ASSERT_DEV(m_pCapture == nullptr, "Cannot skip while capturing");
      const plUInt64 uiSkipped = m_Source.SkipBytes(uiBytesToSkip);
      m_uiPosition += uiSkipped;
      return uiSkipped;
    }

    plUInt64 m_uiPosition = 0;
    plDynamicArray<plUInt8>* m_pCapture = nullptr;

  private:
    plStreamReader& m_Source;
  };

  struct LoadData
  {
    plDefaultMemoryStreamStorage m_Storage;
    plMemoryStreamReader m_Reader;
  };

  plResult CopyBytes(plStreamReader& inout_source, plStreamWriter& inout_target, plUInt64 uiBytes)
  {
    plUInt8 buffer[16 * 1024];

    while (uiBytes > 0)
    {
      const plUInt64 uiChunk = plMath::Min<plUInt64>(uiBytes, sizeof(buffer));
      if (inout_source.ReadBytes(buffer, uiChunk) != uiChunk)
        return PL_FAILURE;

      PL_SUCCEED_OR_RETURN(inout_target.WriteBytes(buffer, uiChunk));
      uiBytes -= uiChunk;
    }

    return PL_SUCCESS;
  }
} // namespace

void plAudioClipResourceLoader::SetLanguage(plStringView sLanguage)
{
  PL_LOCK(s_LanguageMutex);

  if (s_sLanguage != sLanguage)
  {
    s_sLanguage = sLanguage;
    s_iLanguageGeneration.Increment();
  }
}

plString plAudioClipResourceLoader::GetLanguage()
{
  PL_LOCK(s_LanguageMutex);
  return s_sLanguage;
}

plUInt32 plAudioClipResourceLoader::GetLanguageGeneration()
{
  return static_cast<plUInt32>(static_cast<plInt32>(s_iLanguageGeneration));
}

plResourceLoadData plAudioClipResourceLoader::OpenDataStream(const plResource* pResource)
{
  PL_PROFILE_SCOPE("ReadAudioClip");

  plResourceLoadData res;

  // a loaded packed bank that carries the clip has it at an offset of the bank file
  plAudioPackedContent::Location packed;
  const bool bPacked = plAudioPackedContent::Find(pResource->GetResourceID(), packed);
  const plStringView sFile = bPacked ? packed.m_sFile.GetView() : pResource->GetResourceID().GetView();

  plFileReader file;
  if (file.Open(sFile).Failed())
    return res;

  if (bPacked && file.SkipBytes(packed.m_uiOffset) != packed.m_uiOffset)
    return res;

  res.m_sResourceDescription = file.GetFilePathRelative().GetData();

#if PL_ENABLED(PL_SUPPORTS_FILE_STATS)
  plFileStats stat;
  if (plFileSystem::GetFileStats(sFile, stat).Succeeded())
  {
    res.m_LoadedFileModificationDate = stat.m_LastModificationTime;
  }
#endif

  CountingReader reader(file);

  plAssetFileHeader assetHeader;
  if (assetHeader.Read(reader).Failed())
    return res;

  plDynamicArray<plUInt8> headerData;
  plAudioClipHeader header;
  {
    reader.m_pCapture = &headerData;
    const plResult headerResult = header.Read(reader);
    reader.m_pCapture = nullptr;

    if (headerResult.Failed())
    {
      plLog::Error("Invalid audio clip header in '{}'", file.GetFilePathRelative().GetView());
      return res;
    }
  }

  // the stream service reads a streamed payload from the clip's own file; banks do not pack streamed clips
  if (bPacked && header.m_eLoadMode == plAudioClipLoadMode::Streamed)
  {
    plLog::Error("The streamed audio clip '{}' cannot be read from the packed bank '{}'", pResource->GetResourceID(), packed.m_sFile);
    return res;
  }

  plString sRequestedLanguage;
  plUInt32 uiLanguageGeneration = 0;
  {
    PL_LOCK(s_LanguageMutex);
    sRequestedLanguage = s_sLanguage;
    uiLanguageGeneration = static_cast<plUInt32>(static_cast<plInt32>(s_iLanguageGeneration));
  }

  const plUInt32 uiLanguage = header.FindLanguage(sRequestedLanguage);
  const plAudioClipLanguageEntry& language = header.m_Languages[uiLanguage];

  for (plUInt32 i = 0; i < uiLanguage; ++i)
  {
    const plUInt64 uiSection = header.m_Languages[i].GetSectionBytes();
    if (reader.SkipBytes(uiSection) != uiSection)
      return res;
  }

  const plUInt64 uiPayloadFileOffset = reader.m_uiPosition + language.m_uiPrimingBytes + language.m_uiLoopPrimingBytes;

  // Streamed payloads are read with plOSFile when the clip is a plain file on disk. In any other data directory
  // (a package archive) the path does not exist as a file and the stream reads through plFileReader instead.
  plString sOsFilePath = file.GetFilePathAbsolute().GetView();
  if (header.m_eLoadMode != plAudioClipLoadMode::Streamed || !plOSFile::ExistsFile(sOsFilePath))
  {
    sOsFilePath.Clear();
  }

  LoadData* pData = PL_DEFAULT_NEW(LoadData);
  {
    plMemoryStreamWriter w(&pData->m_Storage);
    w << sOsFilePath;
    w << language.m_sLanguage;
    w << uiLanguageGeneration;
    w << uiPayloadFileOffset;
    w << language.m_uiPayloadBytes;
    w << headerData.GetCount();
    w.WriteBytes(headerData.GetData(), headerData.GetCount()).AssertSuccess();

    plUInt64 uiBytes = static_cast<plUInt64>(language.m_uiPrimingBytes) + language.m_uiLoopPrimingBytes;
    if (header.m_eLoadMode != plAudioClipLoadMode::Streamed)
      uiBytes += language.m_uiPayloadBytes;

    if (CopyBytes(reader, w, uiBytes).Failed())
    {
      plLog::Error("Audio clip '{}' is truncated", file.GetFilePathRelative().GetView());
      PL_DEFAULT_DELETE(pData);
      return res;
    }
  }

  pData->m_Reader.SetStorage(&pData->m_Storage);

  res.m_pDataStream = &pData->m_Reader;
  res.m_pCustomLoaderData = pData;
  return res;
}

void plAudioClipResourceLoader::CloseDataStream(const plResource* pResource, const plResourceLoadData& loaderData)
{
  PL_IGNORE_UNUSED(pResource);

  LoadData* pData = static_cast<LoadData*>(loaderData.m_pCustomLoaderData);
  PL_DEFAULT_DELETE(pData);
}

bool plAudioClipResourceLoader::IsResourceOutdated(const plResource* pResource) const
{
  // if we cannot find the target file, there is no point in trying to reload it -> claim it's up to date
  if (plFileSystem::ResolvePath(pResource->GetResourceID(), nullptr, nullptr).Failed())
    return false;

#if PL_ENABLED(PL_SUPPORTS_FILE_STATS)

  if (pResource->GetLoadedFileModificationTime().IsValid())
  {
    plFileStats stat;
    if (plFileSystem::GetFileStats(pResource->GetResourceID(), stat).Failed())
      return false;

    return !stat.m_LastModificationTime.Compare(pResource->GetLoadedFileModificationTime(), plTimestamp::CompareMode::FileTimeEqual);
  }

#endif

  return true;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Clips_AudioClipResourceLoader);
