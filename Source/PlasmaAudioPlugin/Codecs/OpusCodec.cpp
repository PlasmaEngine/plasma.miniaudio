#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Clips/AudioClipDesc.h>
#include <PlasmaAudioPlugin/Codecs/OpusCodec.h>

#include <opus.h>

plResult plOpusCodec::Encode(plArrayPtr<const float> samples, plUInt8 uiChannels, plUInt32 uiBitrate, plDynamicArray<plUInt8>& out_payload, plUInt32& out_uiPreSkip, plDynamicArray<plAudioClipSeekEntry>& out_seekTable)
{
  out_payload.Clear();
  out_seekTable.Clear();
  out_uiPreSkip = 0;

  if (uiChannels != 1 && uiChannels != 2)
  {
    plLog::Error("Opus audio clips must be mono or stereo, the source has {} channels", uiChannels);
    return PL_FAILURE;
  }

  int iError = OPUS_OK;
  OpusEncoder* pEncoder = opus_encoder_create(SampleRate, uiChannels, OPUS_APPLICATION_AUDIO, &iError);
  if (pEncoder == nullptr || iError != OPUS_OK)
  {
    plLog::Error("Creating the Opus encoder failed: {}", opus_strerror(iError));
    return PL_FAILURE;
  }

  PL_SCOPE_EXIT(opus_encoder_destroy(pEncoder));

  opus_int32 iLookahead = 0;
  if (opus_encoder_ctl(pEncoder, OPUS_SET_BITRATE(static_cast<opus_int32>(uiBitrate))) != OPUS_OK ||
      opus_encoder_ctl(pEncoder, OPUS_SET_COMPLEXITY(10)) != OPUS_OK ||
      opus_encoder_ctl(pEncoder, OPUS_GET_LOOKAHEAD(&iLookahead)) != OPUS_OK)
  {
    plLog::Error("Configuring the Opus encoder failed");
    return PL_FAILURE;
  }

  out_uiPreSkip = static_cast<plUInt32>(iLookahead);

  const plUInt64 uiFrames = samples.GetCount() / uiChannels;
  const plUInt64 uiPackets = GetPacketCount(uiFrames, out_uiPreSkip);

  plDynamicArray<float> packetIn;
  packetIn.SetCountUninitialized(FramesPerPacket * uiChannels);

  plUInt8 packetOut[MaxPacketBytes];

  for (plUInt64 p = 0; p < uiPackets; ++p)
  {
    if (p % SeekIntervalPackets == 0)
    {
      plAudioClipSeekEntry& entry = out_seekTable.ExpandAndGetRef();
      entry.m_uiByteOffset = out_payload.GetCount();
      entry.m_uiFrame = p * FramesPerPacket;
    }

    // the tail of the last packets is silence
    const plUInt64 uiFirst = p * FramesPerPacket;
    const plUInt64 uiValid = uiFirst < uiFrames ? plMath::Min<plUInt64>(FramesPerPacket, uiFrames - uiFirst) : 0;
    plMemoryUtils::ZeroFill(packetIn.GetData(), packetIn.GetCount());
    if (uiValid > 0)
    {
      plMemoryUtils::Copy(packetIn.GetData(), samples.GetPtr() + uiFirst * uiChannels, static_cast<size_t>(uiValid * uiChannels));
    }

    const opus_int32 iBytes = opus_encode_float(pEncoder, packetIn.GetData(), FramesPerPacket, packetOut, MaxPacketBytes);
    if (iBytes < 0)
    {
      plLog::Error("Opus encoding failed: {}", opus_strerror(iBytes));
      return PL_FAILURE;
    }

    out_payload.PushBack(static_cast<plUInt8>(iBytes & 0xFF));
    out_payload.PushBack(static_cast<plUInt8>((iBytes >> 8) & 0xFF));
    out_payload.PushBackRange(plArrayPtr<const plUInt8>(packetOut, static_cast<plUInt32>(iBytes)));
  }

  return PL_SUCCESS;
}

plResult plOpusCodec::Decode(plArrayPtr<const plUInt8> payload, plUInt8 uiChannels, plUInt32 uiPreSkip, plUInt64 uiFrames, plDynamicArray<plInt16>& out_pcm)
{
  if (uiChannels != 1 && uiChannels != 2)
    return PL_FAILURE;

  if (uiFrames * uiChannels > plMath::MaxValue<plUInt32>())
    return PL_FAILURE;

  int iError = OPUS_OK;
  OpusDecoder* pDecoder = opus_decoder_create(SampleRate, uiChannels, &iError);
  if (pDecoder == nullptr || iError != OPUS_OK)
    return PL_FAILURE;

  PL_SCOPE_EXIT(opus_decoder_destroy(pDecoder));

  out_pcm.SetCount(static_cast<plUInt32>(uiFrames * uiChannels));

  plDynamicArray<plInt16> packetPcm;
  packetPcm.SetCountUninitialized(FramesPerPacket * uiChannels);

  const plUInt64 uiPackets = GetPacketCount(uiFrames, uiPreSkip);
  plUInt64 uiOffset = 0;

  for (plUInt64 p = 0; p < uiPackets; ++p)
  {
    if (uiOffset + 2 > payload.GetCount())
      return PL_FAILURE;

    const plUInt32 uiBytes = static_cast<plUInt32>(payload[static_cast<plUInt32>(uiOffset)]) | (static_cast<plUInt32>(payload[static_cast<plUInt32>(uiOffset + 1)]) << 8);
    uiOffset += 2;

    if (uiBytes > MaxPacketBytes || uiOffset + uiBytes > payload.GetCount())
      return PL_FAILURE;

    const int iDecoded = opus_decode(pDecoder, payload.GetPtr() + uiOffset, static_cast<opus_int32>(uiBytes), packetPcm.GetData(), FramesPerPacket, 0);
    uiOffset += uiBytes;

    if (iDecoded != static_cast<int>(FramesPerPacket))
      return PL_FAILURE;

    // decoder frames [p * 960, (p + 1) * 960) are clip frames shifted by the pre-skip
    const plInt64 iFirstClipFrame = static_cast<plInt64>(p * FramesPerPacket) - static_cast<plInt64>(uiPreSkip);
    const plInt64 iBegin = plMath::Max<plInt64>(iFirstClipFrame, 0);
    const plInt64 iEnd = plMath::Min<plInt64>(iFirstClipFrame + FramesPerPacket, static_cast<plInt64>(uiFrames));

    if (iEnd > iBegin)
    {
      plMemoryUtils::Copy(out_pcm.GetData() + iBegin * uiChannels, packetPcm.GetData() + (iBegin - iFirstClipFrame) * uiChannels, static_cast<size_t>((iEnd - iBegin) * uiChannels));
    }
  }

  return PL_SUCCESS;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Codecs_OpusCodec);
