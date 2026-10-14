#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Codecs/AudioByteSource.h>
#include <PlasmaAudioPlugin/Codecs/OpusCodec.h>
#include <PlasmaAudioPlugin/Codecs/OpusDataSource.h>

#include <opus.h>

namespace
{
  constexpr plUInt64 s_uiNoPacket = 0xFFFFFFFFFFFFFFFFull;

  /// Reads the length prefix of the packet at uiOffset. False when the bytes are not available.
  bool ReadPacketBytes(plOpusDataSource* pSource, plUInt64 uiOffset, plUInt32& out_uiBytes)
  {
    plUInt8 prefix[2];
    if (pSource->m_pBytes->Read(uiOffset, prefix, 2) != 2)
      return false;

    out_uiBytes = static_cast<plUInt32>(prefix[0]) | (static_cast<plUInt32>(prefix[1]) << 8);
    return true;
  }

  /// Moves the decoder to the pre-roll start of uiTargetPacket: the packet offset is found by walking the length
  /// prefixes from the last seek table entry at or before it. Nothing changes when the prefixes are not available.
  ///
  /// The decoder state is only reset when decoding restarts at packet 0 (which then matches the decode-through
  /// exactly). Otherwise the pre-roll packets are fed to the state left by the previous position: the CELT band
  /// energies are predicted from the previous packet and converge by about 6 dB per packet, starting from the energies
  /// of the old position instead of from a reset (silent) state. At a loop jump the old position is the loop end, whose
  /// level matches the loop start in a seamless loop, so the seam converges within the pre-roll.
  bool Reposition(plOpusDataSource* pSource, plUInt64 uiTargetPacket)
  {
    const plUInt64 uiStartPacket = uiTargetPacket > plOpusCodec::PreRollPackets ? uiTargetPacket - plOpusCodec::PreRollPackets : 0;

    plUInt64 uiPacket = 0;
    plUInt64 uiOffset = 0;
    for (const plAudioClipSeekEntry& entry : pSource->m_SeekTable)
    {
      if (entry.m_uiFrame > uiStartPacket * plOpusCodec::FramesPerPacket)
        break;

      uiPacket = entry.m_uiFrame / plOpusCodec::FramesPerPacket;
      uiOffset = entry.m_uiByteOffset;
    }

    while (uiPacket < uiStartPacket)
    {
      plUInt32 uiBytes = 0;
      if (!ReadPacketBytes(pSource, uiOffset, uiBytes))
        return false;

      uiOffset += 2 + uiBytes;
      ++uiPacket;
    }

    if (uiStartPacket == 0)
    {
      opus_decoder_ctl(pSource->m_pDecoder, OPUS_RESET_STATE);
    }

    pSource->m_uiNextPacket = uiStartPacket;
    pSource->m_uiNextPacketOffset = uiOffset;
    pSource->m_uiCachedPacket = s_uiNoPacket;
    return true;
  }

  /// Makes uiPacket the cached packet. False when the bytes of a needed packet are not available; the packets
  /// decoded up to then are kept.
  bool DecodeUpTo(plOpusDataSource* pSource, plUInt64 uiPacket)
  {
    if (uiPacket < pSource->m_uiNextPacket || uiPacket > pSource->m_uiNextPacket + plOpusCodec::PreRollPackets)
    {
      if (!Reposition(pSource, uiPacket))
        return false;
    }

    while (pSource->m_uiNextPacket <= uiPacket)
    {
      plUInt32 uiBytes = 0;
      if (!ReadPacketBytes(pSource, pSource->m_uiNextPacketOffset, uiBytes))
        return false;

      int iDecoded = -1;
      if (uiBytes <= plOpusCodec::MaxPacketBytes)
      {
        if (pSource->m_pBytes->Read(pSource->m_uiNextPacketOffset + 2, pSource->m_PacketData.GetData(), uiBytes) != uiBytes)
          return false;

        iDecoded = opus_decode(pSource->m_pDecoder, pSource->m_PacketData.GetData(), static_cast<opus_int32>(uiBytes), pSource->m_PacketPcm.GetData(), plOpusCodec::FramesPerPacket, 0);
      }

      if (iDecoded != static_cast<int>(plOpusCodec::FramesPerPacket))
      {
        // a damaged packet plays as silence
        plMemoryUtils::ZeroFill(pSource->m_PacketPcm.GetData(), pSource->m_PacketPcm.GetCount());
      }

      pSource->m_uiCachedPacket = pSource->m_uiNextPacket;
      pSource->m_uiNextPacketOffset += 2 + uiBytes;
      ++pSource->m_uiNextPacket;
    }

    return true;
  }

  ma_result OpusRead(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 uiFrameCount, ma_uint64* pFramesRead)
  {
    auto* pSource = static_cast<plOpusDataSource*>(pDataSource);
    plInt16* pOut = static_cast<plInt16*>(pFramesOut);
    const plUInt32 uiChannels = pSource->m_uiChannels;

    ma_uint64 uiDone = 0;

    while (uiDone < uiFrameCount && pSource->m_uiCursor < pSource->m_uiFrames)
    {
      const plUInt64 uiDecoderFrame = pSource->m_uiCursor + pSource->m_uiPreSkip;
      const plUInt64 uiPacket = uiDecoderFrame / plOpusCodec::FramesPerPacket;

      if (uiPacket != pSource->m_uiCachedPacket && !DecodeUpTo(pSource, uiPacket))
      {
        // data not there yet: silence for the rest of the request, the cursor stays where it is
        if (pOut != nullptr)
        {
          ma_silence_pcm_frames(pOut + uiDone * uiChannels, uiFrameCount - uiDone, ma_format_s16, uiChannels);
        }

        pSource->m_iUnderruns.Increment();

        if (pFramesRead != nullptr)
          *pFramesRead = uiFrameCount;
        return MA_SUCCESS;
      }

      const plUInt64 uiInPacket = uiDecoderFrame - uiPacket * plOpusCodec::FramesPerPacket;
      const plUInt64 uiCount = plMath::Min<plUInt64>(plOpusCodec::FramesPerPacket - uiInPacket, uiFrameCount - uiDone, pSource->m_uiFrames - pSource->m_uiCursor);

      if (pOut != nullptr)
      {
        plMemoryUtils::Copy(pOut + uiDone * uiChannels, pSource->m_PacketPcm.GetData() + uiInPacket * uiChannels, static_cast<size_t>(uiCount * uiChannels));
      }

      uiDone += uiCount;
      pSource->m_uiCursor += uiCount;
    }

    if (pFramesRead != nullptr)
      *pFramesRead = uiDone;

    return uiDone > 0 ? MA_SUCCESS : MA_AT_END;
  }

  ma_result OpusSeek(ma_data_source* pDataSource, ma_uint64 uiFrameIndex)
  {
    auto* pSource = static_cast<plOpusDataSource*>(pDataSource);
    if (uiFrameIndex > pSource->m_uiFrames)
      return MA_INVALID_ARGS;

    pSource->m_uiCursor = uiFrameIndex;
    return MA_SUCCESS;
  }

  ma_result OpusGetDataFormat(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t uiChannelMapCap)
  {
    auto* pSource = static_cast<plOpusDataSource*>(pDataSource);

    if (pFormat != nullptr)
      *pFormat = ma_format_s16;
    if (pChannels != nullptr)
      *pChannels = pSource->m_uiChannels;
    if (pSampleRate != nullptr)
      *pSampleRate = plOpusCodec::SampleRate;
    if (pChannelMap != nullptr)
      ma_channel_map_init_standard(ma_standard_channel_map_default, pChannelMap, uiChannelMapCap, pSource->m_uiChannels);

    return MA_SUCCESS;
  }

  ma_result OpusGetCursor(ma_data_source* pDataSource, ma_uint64* pCursor)
  {
    *pCursor = static_cast<plOpusDataSource*>(pDataSource)->m_uiCursor;
    return MA_SUCCESS;
  }

  ma_result OpusGetLength(ma_data_source* pDataSource, ma_uint64* pLength)
  {
    *pLength = static_cast<plOpusDataSource*>(pDataSource)->m_uiFrames;
    return MA_SUCCESS;
  }

  ma_data_source_vtable s_OpusVTable = {OpusRead, OpusSeek, OpusGetDataFormat, OpusGetCursor, OpusGetLength, nullptr, 0};
} // namespace

ma_result plOpusDataSource_Init(plAudioByteSource* pBytes, const plAudioClipHeader& header, plOpusDataSource* pSource)
{
  if (pBytes == nullptr || pSource == nullptr || header.m_eCodec != plAudioClipCodec::Opus || (header.m_uiChannels != 1 && header.m_uiChannels != 2) ||
      header.m_uiSampleRate != plOpusCodec::SampleRate || header.m_uiBlockFrames != plOpusCodec::FramesPerPacket)
    return MA_INVALID_ARGS;

  const int iDecoderBytes = opus_decoder_get_size(header.m_uiChannels);
  if (iDecoderBytes <= 0)
    return MA_ERROR;

  // the array keeps its capacity from earlier uses of the source
  pSource->m_DecoderState.SetCountUninitialized(static_cast<plUInt32>(iDecoderBytes));
  OpusDecoder* pDecoder = reinterpret_cast<OpusDecoder*>(pSource->m_DecoderState.GetData());
  if (opus_decoder_init(pDecoder, plOpusCodec::SampleRate, header.m_uiChannels) != OPUS_OK)
    return MA_ERROR;

  pSource->m_pBytes = pBytes;
  pSource->m_pDecoder = pDecoder;
  pSource->m_uiChannels = header.m_uiChannels;
  pSource->m_uiPreSkip = header.m_uiPreSkip;
  pSource->m_uiFrames = header.m_uiFrames;
  pSource->m_uiCursor = 0;
  pSource->m_uiNextPacket = 0;
  pSource->m_uiNextPacketOffset = 0;
  pSource->m_uiCachedPacket = s_uiNoPacket;
  pSource->m_iUnderruns = 0;
  pSource->m_SeekTable = header.m_SeekTable;
  pSource->m_PacketPcm.SetCountUninitialized(plOpusCodec::FramesPerPacket * header.m_uiChannels);
  pSource->m_PacketData.SetCountUninitialized(plOpusCodec::MaxPacketBytes);

  ma_data_source_config cfg = ma_data_source_config_init();
  cfg.vtable = &s_OpusVTable;
  const ma_result res = ma_data_source_init(&cfg, &pSource->m_Base);

  if (res != MA_SUCCESS)
  {
    pSource->m_pDecoder = nullptr;
    pSource->m_pBytes = nullptr;
  }

  return res;
}

void plOpusDataSource_Uninit(plOpusDataSource* pSource)
{
  if (pSource == nullptr || pSource->m_pBytes == nullptr)
    return;

  // the arrays keep their memory for the next plOpusDataSource_Init
  ma_data_source_uninit(&pSource->m_Base);
  pSource->m_pDecoder = nullptr;
  pSource->m_pBytes = nullptr;
  pSource->m_SeekTable.Clear();
  pSource->m_PacketPcm.Clear();
  pSource->m_PacketData.Clear();
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Codecs_OpusDataSource);
