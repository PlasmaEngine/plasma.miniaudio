#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Codecs/AdpcmDataSource.h>
#include <PlasmaAudioPlugin/Codecs/AudioByteSource.h>
#include <PlasmaAudioPlugin/Codecs/ImaAdpcm.h>

namespace
{
  void WriteFormat(plUInt8 uiChannels, plUInt32 uiSampleRate, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t uiChannelMapCap)
  {
    if (pFormat != nullptr)
      *pFormat = ma_format_s16;
    if (pChannels != nullptr)
      *pChannels = uiChannels;
    if (pSampleRate != nullptr)
      *pSampleRate = uiSampleRate;
    if (pChannelMap != nullptr)
      ma_channel_map_init_standard(ma_standard_channel_map_default, pChannelMap, uiChannelMapCap, uiChannels);
  }

  // -------------------------------------------------------------------------------------------
  // plAdpcmDataSource
  // -------------------------------------------------------------------------------------------

  ma_result AdpcmRead(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 uiFrameCount, ma_uint64* pFramesRead)
  {
    auto* pSource = static_cast<plAdpcmDataSource*>(pDataSource);
    plInt16* pOut = static_cast<plInt16*>(pFramesOut);
    const plUInt32 uiChannels = pSource->m_uiChannels;

    ma_uint64 uiDone = 0;

    while (uiDone < uiFrameCount && pSource->m_uiCursor < pSource->m_uiFrames)
    {
      const plUInt64 uiBlock = pSource->m_uiCursor / pSource->m_uiBlockFrames;

      if (uiBlock != pSource->m_uiCachedBlock)
      {
        const plUInt64 uiOffset = uiBlock * pSource->m_uiBlockBytes;

        if (!pSource->m_pBytes->IsAvailable(uiOffset, pSource->m_uiBlockBytes) ||
            pSource->m_pBytes->Read(uiOffset, pSource->m_BlockData.GetData(), pSource->m_uiBlockBytes) != pSource->m_uiBlockBytes)
        {
          // data not there yet: silence for the rest of the request, the cursor stays on this block
          if (pOut != nullptr)
          {
            ma_silence_pcm_frames(pOut + uiDone * uiChannels, uiFrameCount - uiDone, ma_format_s16, uiChannels);
          }

          pSource->m_iUnderruns.Increment();

          if (pFramesRead != nullptr)
            *pFramesRead = uiFrameCount;
          return MA_SUCCESS;
        }

        plImaAdpcmCodec::DecodeBlock(pSource->m_BlockData.GetData(), pSource->m_uiChannels, pSource->m_BlockPcm.GetData());
        pSource->m_uiCachedBlock = uiBlock;
      }

      const plUInt64 uiInBlock = pSource->m_uiCursor - uiBlock * pSource->m_uiBlockFrames;
      const plUInt64 uiCount = plMath::Min<plUInt64>(pSource->m_uiBlockFrames - uiInBlock, uiFrameCount - uiDone, pSource->m_uiFrames - pSource->m_uiCursor);

      if (pOut != nullptr)
      {
        plMemoryUtils::Copy(pOut + uiDone * uiChannels, pSource->m_BlockPcm.GetData() + uiInBlock * uiChannels, static_cast<size_t>(uiCount * uiChannels));
      }

      uiDone += uiCount;
      pSource->m_uiCursor += uiCount;
    }

    if (pFramesRead != nullptr)
      *pFramesRead = uiDone;

    return uiDone > 0 ? MA_SUCCESS : MA_AT_END;
  }

  ma_result AdpcmSeek(ma_data_source* pDataSource, ma_uint64 uiFrameIndex)
  {
    auto* pSource = static_cast<plAdpcmDataSource*>(pDataSource);
    if (uiFrameIndex > pSource->m_uiFrames)
      return MA_INVALID_ARGS;

    pSource->m_uiCursor = uiFrameIndex;
    return MA_SUCCESS;
  }

  ma_result AdpcmGetDataFormat(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t uiChannelMapCap)
  {
    auto* pSource = static_cast<plAdpcmDataSource*>(pDataSource);
    WriteFormat(pSource->m_uiChannels, pSource->m_uiSampleRate, pFormat, pChannels, pSampleRate, pChannelMap, uiChannelMapCap);
    return MA_SUCCESS;
  }

  ma_result AdpcmGetCursor(ma_data_source* pDataSource, ma_uint64* pCursor)
  {
    *pCursor = static_cast<plAdpcmDataSource*>(pDataSource)->m_uiCursor;
    return MA_SUCCESS;
  }

  ma_result AdpcmGetLength(ma_data_source* pDataSource, ma_uint64* pLength)
  {
    *pLength = static_cast<plAdpcmDataSource*>(pDataSource)->m_uiFrames;
    return MA_SUCCESS;
  }

  ma_data_source_vtable s_AdpcmVTable = {AdpcmRead, AdpcmSeek, AdpcmGetDataFormat, AdpcmGetCursor, AdpcmGetLength, nullptr, 0};

  // -------------------------------------------------------------------------------------------
  // plPcmByteDataSource
  // -------------------------------------------------------------------------------------------

  ma_result PcmBytesRead(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 uiFrameCount, ma_uint64* pFramesRead)
  {
    auto* pSource = static_cast<plPcmByteDataSource*>(pDataSource);
    const plUInt32 uiFrameBytes = pSource->m_uiChannels * sizeof(plInt16);

    const plUInt64 uiCount = plMath::Min<plUInt64>(uiFrameCount, pSource->m_uiFrames - pSource->m_uiCursor);

    if (uiCount == 0)
    {
      if (pFramesRead != nullptr)
        *pFramesRead = 0;
      return MA_AT_END;
    }

    const plUInt64 uiOffset = pSource->m_uiCursor * uiFrameBytes;
    const plUInt32 uiBytes = static_cast<plUInt32>(uiCount * uiFrameBytes);

    bool bAvailable = pSource->m_pBytes->IsAvailable(uiOffset, uiBytes);

    if (bAvailable && pFramesOut != nullptr)
    {
      bAvailable = pSource->m_pBytes->Read(uiOffset, pFramesOut, uiBytes) == uiBytes;
    }

    if (!bAvailable)
    {
      // data not there yet: silence for the whole request, the cursor stays where it is
      if (pFramesOut != nullptr)
      {
        ma_silence_pcm_frames(pFramesOut, uiFrameCount, ma_format_s16, pSource->m_uiChannels);
      }

      pSource->m_iUnderruns.Increment();

      if (pFramesRead != nullptr)
        *pFramesRead = uiFrameCount;
      return MA_SUCCESS;
    }

    pSource->m_uiCursor += uiCount;

    if (pFramesRead != nullptr)
      *pFramesRead = uiCount;
    return MA_SUCCESS;
  }

  ma_result PcmBytesSeek(ma_data_source* pDataSource, ma_uint64 uiFrameIndex)
  {
    auto* pSource = static_cast<plPcmByteDataSource*>(pDataSource);
    if (uiFrameIndex > pSource->m_uiFrames)
      return MA_INVALID_ARGS;

    pSource->m_uiCursor = uiFrameIndex;
    return MA_SUCCESS;
  }

  ma_result PcmBytesGetDataFormat(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t uiChannelMapCap)
  {
    auto* pSource = static_cast<plPcmByteDataSource*>(pDataSource);
    WriteFormat(pSource->m_uiChannels, pSource->m_uiSampleRate, pFormat, pChannels, pSampleRate, pChannelMap, uiChannelMapCap);
    return MA_SUCCESS;
  }

  ma_result PcmBytesGetCursor(ma_data_source* pDataSource, ma_uint64* pCursor)
  {
    *pCursor = static_cast<plPcmByteDataSource*>(pDataSource)->m_uiCursor;
    return MA_SUCCESS;
  }

  ma_result PcmBytesGetLength(ma_data_source* pDataSource, ma_uint64* pLength)
  {
    *pLength = static_cast<plPcmByteDataSource*>(pDataSource)->m_uiFrames;
    return MA_SUCCESS;
  }

  ma_data_source_vtable s_PcmBytesVTable = {PcmBytesRead, PcmBytesSeek, PcmBytesGetDataFormat, PcmBytesGetCursor, PcmBytesGetLength, nullptr, 0};
} // namespace

ma_result plAdpcmDataSource_Init(plAudioByteSource* pBytes, plUInt32 uiBlockFrames, plUInt8 uiChannels, plUInt32 uiSampleRate, plUInt64 uiFrames, plAdpcmDataSource* pSource)
{
  if (pBytes == nullptr || pSource == nullptr || uiChannels == 0 || uiBlockFrames != plImaAdpcmCodec::FramesPerBlock)
    return MA_INVALID_ARGS;

  pSource->m_pBytes = pBytes;
  pSource->m_uiBlockFrames = uiBlockFrames;
  pSource->m_uiBlockBytes = plImaAdpcmCodec::GetBlockBytes(uiChannels);
  pSource->m_uiChannels = uiChannels;
  pSource->m_uiSampleRate = uiSampleRate;
  pSource->m_uiFrames = uiFrames;
  pSource->m_uiCursor = 0;
  pSource->m_uiCachedBlock = 0xFFFFFFFFFFFFFFFFull;
  pSource->m_iUnderruns = 0;
  pSource->m_BlockPcm.SetCountUninitialized(uiBlockFrames * uiChannels);
  pSource->m_BlockData.SetCountUninitialized(pSource->m_uiBlockBytes);

  ma_data_source_config cfg = ma_data_source_config_init();
  cfg.vtable = &s_AdpcmVTable;
  return ma_data_source_init(&cfg, &pSource->m_Base);
}

void plAdpcmDataSource_Uninit(plAdpcmDataSource* pSource)
{
  if (pSource == nullptr || pSource->m_pBytes == nullptr)
    return;

  // the arrays keep their memory for the next plAdpcmDataSource_Init
  ma_data_source_uninit(&pSource->m_Base);
  pSource->m_pBytes = nullptr;
  pSource->m_BlockPcm.Clear();
  pSource->m_BlockData.Clear();
}

ma_result plPcmByteDataSource_Init(plAudioByteSource* pBytes, plUInt8 uiChannels, plUInt32 uiSampleRate, plUInt64 uiFrames, plPcmByteDataSource* pSource)
{
  if (pBytes == nullptr || pSource == nullptr || uiChannels == 0)
    return MA_INVALID_ARGS;

  pSource->m_pBytes = pBytes;
  pSource->m_uiChannels = uiChannels;
  pSource->m_uiSampleRate = uiSampleRate;
  pSource->m_uiFrames = uiFrames;
  pSource->m_uiCursor = 0;
  pSource->m_iUnderruns = 0;

  ma_data_source_config cfg = ma_data_source_config_init();
  cfg.vtable = &s_PcmBytesVTable;
  return ma_data_source_init(&cfg, &pSource->m_Base);
}

void plPcmByteDataSource_Uninit(plPcmByteDataSource* pSource)
{
  if (pSource == nullptr || pSource->m_pBytes == nullptr)
    return;

  ma_data_source_uninit(&pSource->m_Base);
  pSource->m_pBytes = nullptr;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Codecs_AdpcmDataSource);
