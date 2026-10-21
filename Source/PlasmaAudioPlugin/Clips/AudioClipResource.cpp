#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Types/ScopeExit.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResource.h>
#include <PlasmaAudioPlugin/Clips/AudioClipVoiceSource.h>
#include <PlasmaAudioPlugin/Codecs/ImaAdpcm.h>
#include <PlasmaAudioPlugin/Codecs/OpusCodec.h>
#include <PlasmaAudioPlugin/Streaming/AudioStreamService.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioClipResource, 1, plRTTIDefaultAllocator<plAudioClipResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plAudioClipResource);

namespace
{
  plResult ReadBytes(plStreamReader& inout_stream, plDynamicArray<plUInt8>& out_data, plUInt64 uiBytes)
  {
    if (uiBytes > plMath::MaxValue<plUInt32>())
      return PL_FAILURE;

    out_data.SetCountUninitialized(static_cast<plUInt32>(uiBytes));
    return inout_stream.ReadBytes(out_data.GetData(), uiBytes) == uiBytes ? PL_SUCCESS : PL_FAILURE;
  }
} // namespace

plAudioClipResource::plAudioClipResource()
  : plResource(DoUpdate::OnAnyThread, 1)
{
  ModifyMemoryUsage().m_uiMemoryCPU = sizeof(plAudioClipResource);
}

plAtomicInteger64 plAudioClipResource::s_iResidentBytes;
plMutex plAudioClipResource::s_LoadedMutex;
plAudioClipResource* plAudioClipResource::s_pFirstLoaded = nullptr;

plAudioClipResource::~plAudioClipResource()
{
  UnlinkLoaded();
  s_iResidentBytes.Subtract(static_cast<plInt64>(m_uiResidentBytes));
}

void plAudioClipResource::RecordResidentBytes()
{
  const plUInt64 uiBytes = m_Pcm.GetHeapMemoryUsage() + m_Payload.GetHeapMemoryUsage() + m_Priming.GetHeapMemoryUsage() + m_LoopPriming.GetHeapMemoryUsage();

  PL_LOCK(s_LoadedMutex);

  s_iResidentBytes.Add(static_cast<plInt64>(uiBytes) - static_cast<plInt64>(m_uiResidentBytes));
  m_uiResidentBytes = uiBytes;

  if (m_bContentLoaded && !m_bLinkedLoaded)
  {
    m_pPrevLoaded = nullptr;
    m_pNextLoaded = s_pFirstLoaded;
    if (s_pFirstLoaded != nullptr)
    {
      s_pFirstLoaded->m_pPrevLoaded = this;
    }
    s_pFirstLoaded = this;
    m_bLinkedLoaded = true;
  }
}

void plAudioClipResource::UnlinkLoaded()
{
  PL_LOCK(s_LoadedMutex);

  if (!m_bLinkedLoaded)
    return;

  if (m_pPrevLoaded != nullptr)
  {
    m_pPrevLoaded->m_pNextLoaded = m_pNextLoaded;
  }
  else
  {
    s_pFirstLoaded = m_pNextLoaded;
  }

  if (m_pNextLoaded != nullptr)
  {
    m_pNextLoaded->m_pPrevLoaded = m_pPrevLoaded;
  }

  m_pPrevLoaded = nullptr;
  m_pNextLoaded = nullptr;
  m_bLinkedLoaded = false;
}

void plAudioClipResource::GatherLoadedClips(plDynamicArray<plAudioClipMemorySnapshotEntry>& out_clips, const plAudioStreamService* pStreams)
{
  out_clips.Clear();

  // the data of a listed clip is not modified while the mutex is held: loading and clearing unlink it first
  PL_LOCK(s_LoadedMutex);

  for (const plAudioClipResource* pClip = s_pFirstLoaded; pClip != nullptr; pClip = pClip->m_pNextLoaded)
  {
    plAudioClipMemorySnapshotEntry& entry = out_clips.ExpandAndGetRef();
    entry.m_sName = !pClip->GetResourceDescription().IsEmpty() ? pClip->GetResourceDescription() : pClip->GetResourceID();
    entry.m_uiLoadMode = static_cast<plUInt8>(pClip->m_Header.m_eLoadMode);
    entry.m_uiCodec = static_cast<plUInt8>(pClip->m_Header.m_eCodec);
    entry.m_uiResidentBytes = pClip->m_uiResidentBytes;
    entry.m_uiStreams = 0;

    // the stream service mutex is never held while this registry is locked elsewhere, so the nesting cannot deadlock
    if (pStreams != nullptr && pClip->m_Header.m_eLoadMode == plAudioClipLoadMode::Streamed)
    {
      entry.m_uiStreams = pStreams->GetActiveStreamCount(pClip->GetResourceID());
    }
  }
}

void plAudioClipResource::GatherLoadedLocalizedClips(plDynamicArray<plString>& out_clipIds)
{
  out_clipIds.Clear();

  PL_LOCK(s_LoadedMutex);

  for (const plAudioClipResource* pClip = s_pFirstLoaded; pClip != nullptr; pClip = pClip->m_pNextLoaded)
  {
    if (pClip->m_Header.m_Languages.GetCount() > 1)
    {
      out_clipIds.PushBack(pClip->GetResourceID());
    }
  }
}

void plAudioClipResource::GatherMemoryUsage(plAudioMemoryUsage& out_usage)
{
  out_usage.m_uiDecodedBytes = 0;
  out_usage.m_uiCompressedBytes = 0;
  out_usage.m_uiPrimingBytes = 0;

  PL_LOCK(s_LoadedMutex);

  for (const plAudioClipResource* pClip = s_pFirstLoaded; pClip != nullptr; pClip = pClip->m_pNextLoaded)
  {
    switch (pClip->m_Header.m_eLoadMode)
    {
      case plAudioClipLoadMode::DecompressOnLoad:
        out_usage.m_uiDecodedBytes += pClip->m_uiResidentBytes;
        break;
      case plAudioClipLoadMode::CompressedInMemory:
        out_usage.m_uiCompressedBytes += pClip->m_uiResidentBytes;
        break;
      case plAudioClipLoadMode::Streamed:
        out_usage.m_uiPrimingBytes += pClip->m_uiResidentBytes;
        break;
    }
  }
}

plTime plAudioClipResource::GetDuration() const
{
  if (m_Header.m_uiSampleRate == 0)
    return plTime::MakeZero();

  return plTime::MakeFromSeconds(static_cast<double>(m_Header.m_uiFrames) / m_Header.m_uiSampleRate);
}

bool plAudioClipResource::GetLoopPoints(plUInt64& out_uiStart, plUInt64& out_uiEnd) const
{
  if (!m_Header.m_bHasLoop)
    return false;

  out_uiStart = m_Header.m_uiLoopStart;
  out_uiEnd = m_Header.m_uiLoopEnd;
  return true;
}

plResult plAudioClipResource::CreateVoiceSource(plAudioClipVoiceSource& out_source, plAudioStreamService* pStreams) const
{
  out_source.Uninit();

  const plUInt8 uiChannels = m_Header.m_uiChannels;

  switch (m_Header.m_eLoadMode)
  {
    case plAudioClipLoadMode::DecompressOnLoad:
    {
      if (m_Pcm.GetCount() < m_Header.m_uiFrames * uiChannels)
      {
        plLog::Error("Audio clip '{}' has no decoded samples", GetResourceIdOrDescription());
        return PL_FAILURE;
      }

      if (ma_audio_buffer_ref_init(ma_format_s16, uiChannels, m_Pcm.GetData(), m_Header.m_uiFrames, &out_source.m_PcmRef) != MA_SUCCESS)
        return PL_FAILURE;

      out_source.m_PcmRef.sampleRate = m_Header.m_uiSampleRate;
      out_source.m_Kind = plAudioClipVoiceSource::Kind::PcmBuffer;
      out_source.m_pSource = &out_source.m_PcmRef;
      return PL_SUCCESS;
    }

    case plAudioClipLoadMode::CompressedInMemory:
    {
      out_source.m_MemBytes.SetData(m_Payload);

      if (m_Header.m_eCodec == plAudioClipCodec::ImaAdpcm)
      {
        if (plAdpcmDataSource_Init(&out_source.m_MemBytes, m_Header.m_uiBlockFrames, uiChannels, m_Header.m_uiSampleRate, m_Header.m_uiFrames, &out_source.m_Adpcm) != MA_SUCCESS)
        {
          out_source.m_MemBytes.SetData({});
          return PL_FAILURE;
        }

        out_source.m_Kind = plAudioClipVoiceSource::Kind::Adpcm;
        out_source.m_pSource = &out_source.m_Adpcm;
        return PL_SUCCESS;
      }

      if (m_Header.m_eCodec == plAudioClipCodec::Pcm16)
      {
        if (plPcmByteDataSource_Init(&out_source.m_MemBytes, uiChannels, m_Header.m_uiSampleRate, m_Header.m_uiFrames, &out_source.m_PcmBytes) != MA_SUCCESS)
        {
          out_source.m_MemBytes.SetData({});
          return PL_FAILURE;
        }

        out_source.m_Kind = plAudioClipVoiceSource::Kind::PcmBytes;
        out_source.m_pSource = &out_source.m_PcmBytes;
        return PL_SUCCESS;
      }

      if (plOpusDataSource_Init(&out_source.m_MemBytes, m_Header, &out_source.m_Opus) != MA_SUCCESS)
      {
        out_source.m_MemBytes.SetData({});
        plLog::Error("Audio clip '{}': the Opus decoder could not be created", GetResourceIdOrDescription());
        return PL_FAILURE;
      }

      out_source.m_Kind = plAudioClipVoiceSource::Kind::Opus;
      out_source.m_pSource = &out_source.m_Opus;
      return PL_SUCCESS;
    }

    case plAudioClipLoadMode::Streamed:
    {
      if (pStreams == nullptr || !pStreams->IsRunning())
      {
        plLog::Warning("Audio clip '{}' is streamed, but no stream service is available", GetResourceIdOrDescription());
        return PL_FAILURE;
      }

      // the stream reads the priming arrays of this resource, which the caller keeps acquired for the voice
      plAudioStream* pStream = pStreams->AcquireStream(*this);
      if (pStream == nullptr)
        return PL_FAILURE;

      out_source.m_pStream = pStream;
      out_source.m_pStreamService = pStreams;

      if (m_Header.m_eCodec == plAudioClipCodec::ImaAdpcm)
      {
        if (plAdpcmDataSource_Init(pStream, m_Header.m_uiBlockFrames, uiChannels, m_Header.m_uiSampleRate, m_Header.m_uiFrames, &out_source.m_Adpcm) != MA_SUCCESS)
        {
          out_source.Uninit();
          return PL_FAILURE;
        }

        out_source.m_Kind = plAudioClipVoiceSource::Kind::Adpcm;
        out_source.m_pSource = &out_source.m_Adpcm;
        return PL_SUCCESS;
      }

      if (m_Header.m_eCodec == plAudioClipCodec::Opus)
      {
        if (plOpusDataSource_Init(pStream, m_Header, &out_source.m_Opus) != MA_SUCCESS)
        {
          out_source.Uninit();
          return PL_FAILURE;
        }

        out_source.m_Kind = plAudioClipVoiceSource::Kind::Opus;
        out_source.m_pSource = &out_source.m_Opus;
        return PL_SUCCESS;
      }

      if (plPcmByteDataSource_Init(pStream, uiChannels, m_Header.m_uiSampleRate, m_Header.m_uiFrames, &out_source.m_PcmBytes) != MA_SUCCESS)
      {
        out_source.Uninit();
        return PL_FAILURE;
      }

      out_source.m_Kind = plAudioClipVoiceSource::Kind::PcmBytes;
      out_source.m_pSource = &out_source.m_PcmBytes;
      return PL_SUCCESS;
    }
  }

  return PL_FAILURE;
}

plResult plAudioClipResource::DecodePayloadToPcm()
{
  const plUInt8 uiChannels = m_Header.m_uiChannels;
  const plUInt64 uiSamples = m_Header.m_uiFrames * uiChannels;

  if (uiSamples > plMath::MaxValue<plUInt32>())
    return PL_FAILURE;

  switch (m_Header.m_eCodec)
  {
    case plAudioClipCodec::Pcm16:
    {
      if (m_Payload.GetCount() < uiSamples * sizeof(plInt16))
        return PL_FAILURE;

      m_Pcm.SetCountUninitialized(static_cast<plUInt32>(uiSamples));
      plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(m_Pcm.GetData()), m_Payload.GetData(), static_cast<size_t>(uiSamples * sizeof(plInt16)));
      break;
    }

    case plAudioClipCodec::ImaAdpcm:
    {
      const plUInt32 uiBlockBytes = plImaAdpcmCodec::GetBlockBytes(uiChannels);
      const plUInt64 uiBlocks = plImaAdpcmCodec::GetBlockCount(m_Header.m_uiFrames);

      if (m_Header.m_uiBlockFrames != plImaAdpcmCodec::FramesPerBlock || m_Payload.GetCount() < uiBlocks * uiBlockBytes)
        return PL_FAILURE;

      m_Pcm.SetCountUninitialized(static_cast<plUInt32>(uiSamples));

      plDynamicArray<plInt16> block;
      block.SetCountUninitialized(plImaAdpcmCodec::FramesPerBlock * uiChannels);

      for (plUInt64 b = 0; b < uiBlocks; ++b)
      {
        plImaAdpcmCodec::DecodeBlock(m_Payload.GetData() + b * uiBlockBytes, uiChannels, block.GetData());

        const plUInt64 uiFirstFrame = b * plImaAdpcmCodec::FramesPerBlock;
        const plUInt64 uiFrames = plMath::Min<plUInt64>(plImaAdpcmCodec::FramesPerBlock, m_Header.m_uiFrames - uiFirstFrame);
        plMemoryUtils::Copy(m_Pcm.GetData() + uiFirstFrame * uiChannels, block.GetData(), static_cast<size_t>(uiFrames * uiChannels));
      }
      break;
    }

    case plAudioClipCodec::Opus:
    {
      if (m_Header.m_uiSampleRate != plOpusCodec::SampleRate || plOpusCodec::Decode(m_Payload, uiChannels, m_Header.m_uiPreSkip, m_Header.m_uiFrames, m_Pcm).Failed())
      {
        plLog::Error("Audio clip '{}': the Opus payload could not be decoded", GetResourceIdOrDescription());
        return PL_FAILURE;
      }
      break;
    }

    default:
      plLog::Error("Audio clip '{}': unknown codec", GetResourceIdOrDescription());
      return PL_FAILURE;
  }

  m_Payload.Clear();
  m_Payload.Compact();
  return PL_SUCCESS;
}

void plAudioClipResource::ClearData()
{
  // out of the registry before the data changes
  UnlinkLoaded();
  m_bContentLoaded = false;

  m_Header = plAudioClipHeader();
  m_Pcm.Clear();
  m_Pcm.Compact();
  m_Payload.Clear();
  m_Payload.Compact();
  m_Priming.Clear();
  m_Priming.Compact();
  m_LoopPriming.Clear();
  m_LoopPriming.Compact();
  m_uiPayloadFileOffset = 0;
  m_uiPayloadBytes = 0;
  m_sOsFilePath.Clear();
  m_sActiveLanguage.Clear();
  m_uiLanguageGeneration = 0;
}

plResourceLoadDesc plAudioClipResource::UnloadData(Unload WhatToUnload)
{
  PL_IGNORE_UNUSED(WhatToUnload);

  ClearData();
  RecordResidentBytes();

  ModifyMemoryUsage().m_uiMemoryCPU = sizeof(plAudioClipResource);

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Unloaded;
  return res;
}

plResourceLoadDesc plAudioClipResource::UpdateContent(plStreamReader* pStream)
{
  PL_LOG_BLOCK("plAudioClipResource::UpdateContent", GetResourceIdOrDescription());

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::LoadedResourceMissing;

  if (pStream == nullptr)
    return res;

  ClearData();

  // on every return: a failed load keeps what it read until the next ClearData
  PL_SCOPE_EXIT(RecordResidentBytes());

  // stream layout written by plAudioClipResourceLoader
  plUInt32 uiHeaderBytes = 0;
  *pStream >> m_sOsFilePath;
  *pStream >> m_sActiveLanguage;
  *pStream >> m_uiLanguageGeneration;
  *pStream >> m_uiPayloadFileOffset;
  *pStream >> m_uiPayloadBytes;
  *pStream >> uiHeaderBytes;

  {
    plDynamicArray<plUInt8> headerData;
    if (ReadBytes(*pStream, headerData, uiHeaderBytes).Failed())
      return res;

    plRawMemoryStreamReader headerReader(headerData);
    if (m_Header.Read(headerReader).Failed())
    {
      plLog::Error("Invalid audio clip header");
      return res;
    }
  }

  const plUInt32 uiLanguage = m_Header.FindLanguage(m_sActiveLanguage);
  m_Header.SelectLanguage(uiLanguage);

  const plAudioClipLanguageEntry& language = m_Header.m_Languages[uiLanguage];

  if (ReadBytes(*pStream, m_Priming, language.m_uiPrimingBytes).Failed() ||
      ReadBytes(*pStream, m_LoopPriming, language.m_uiLoopPrimingBytes).Failed())
    return res;

  if (m_Header.m_eLoadMode != plAudioClipLoadMode::Streamed)
  {
    if (ReadBytes(*pStream, m_Payload, m_uiPayloadBytes).Failed())
      return res;
  }

  if (m_Header.m_eLoadMode == plAudioClipLoadMode::DecompressOnLoad)
  {
    if (DecodePayloadToPcm().Failed())
      return res;
  }

  // RecordResidentBytes on the way out adds the clip to the registry of loaded clips
  m_bContentLoaded = true;
  res.m_State = plResourceState::Loaded;
  return res;
}

void plAudioClipResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plAudioClipResource);
  out_NewMemoryUsage.m_uiMemoryCPU += m_Header.m_Peaks.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryCPU += m_Header.m_SeekTable.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryCPU += m_Header.m_Languages.GetHeapMemoryUsage();
  for (const plAudioClipLanguageEntry& language : m_Header.m_Languages)
  {
    out_NewMemoryUsage.m_uiMemoryCPU += language.m_SeekTable.GetHeapMemoryUsage();
  }
  out_NewMemoryUsage.m_uiMemoryCPU += m_Pcm.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryCPU += m_Payload.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryCPU += m_Priming.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryCPU += m_LoopPriming.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryGPU = 0;
}

PL_RESOURCE_IMPLEMENT_CREATEABLE(plAudioClipResource, plAudioClipResourceDescriptor)
{
  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::LoadedResourceMissing;

  ClearData();
  PL_SCOPE_EXIT(RecordResidentBytes());

  m_Header = std::move(descriptor.m_Header);
  m_Pcm = std::move(descriptor.m_Pcm);
  m_Payload = std::move(descriptor.m_Payload);
  m_uiPayloadBytes = m_Payload.GetCount();

  if (m_Header.m_Languages.IsEmpty())
  {
    m_Header.m_Languages.ExpandAndGetRef().m_uiPayloadBytes = m_uiPayloadBytes;
  }

  switch (m_Header.m_eLoadMode)
  {
    case plAudioClipLoadMode::DecompressOnLoad:
      if (m_Pcm.IsEmpty() && DecodePayloadToPcm().Failed())
        return res;
      m_Payload.Clear();
      m_Payload.Compact();
      break;

    case plAudioClipLoadMode::CompressedInMemory:
      if (m_Payload.IsEmpty())
        return res;
      break;

    case plAudioClipLoadMode::Streamed:
      plLog::Error("Streamed audio clips cannot be created from memory");
      return res;
  }

  m_bContentLoaded = true;
  res.m_State = plResourceState::Loaded;
  return res;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Clips_AudioClipResource);
