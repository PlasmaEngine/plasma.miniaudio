#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Types/ArrayPtr.h>

struct plAudioClipSeekEntry;

/// \brief Opus payloads of audio clips (libopus, compiled into the plugin).
///
/// Payload layout: a sequence of Opus packets, each stored as a u16 (little endian) byte count followed by the packet
/// bytes. Every packet holds FramesPerPacket frames at 48 kHz; there is no Ogg framing.
///
/// Decoder timeline: packet k decodes the decoder frames [k * FramesPerPacket, (k + 1) * FramesPerPacket). The first
/// uiPreSkip decoder frames are the encoder lookahead, so clip frame f is decoder frame f + uiPreSkip. The encoder is
/// fed the clip padded with silence to a whole number of packets that covers all clip frames plus the pre-skip.
///
/// Seek table entries of an Opus clip point at packet boundaries; their m_uiFrame is on the decoder timeline
/// (packet index * FramesPerPacket), not a clip frame.
///
/// Decoding can start at any packet with a reset decoder, but the output converges to the continuous decode only
/// after a few packets. Every random access therefore starts PreRollPackets packets before the packet that holds the
/// target and discards what those produce (see GetDecodeStartPacket()).
struct PL_PLASMAAUDIOPLUGIN_DLL plOpusCodec
{
  static constexpr plUInt32 SampleRate = 48000;
  static constexpr plUInt32 FramesPerPacket = 960; ///< 20 ms
  static constexpr plUInt32 PreRollPackets = 4;    ///< 80 ms
  static constexpr plUInt32 SeekIntervalPackets = 50; ///< one seek table entry per second
  static constexpr plUInt32 MaxPacketBytes = 1275; ///< largest packet of one 20 ms Opus frame

  /// \brief Number of packets of a clip with uiFrames frames.
  static plUInt64 GetPacketCount(plUInt64 uiFrames, plUInt32 uiPreSkip) { return (uiFrames + uiPreSkip + FramesPerPacket - 1) / FramesPerPacket; }

  /// \brief Packet that holds clip frame uiFrame.
  static plUInt64 GetPacketOfFrame(plUInt64 uiFrame, plUInt32 uiPreSkip) { return (uiFrame + uiPreSkip) / FramesPerPacket; }

  /// \brief Packet at which decoding starts to reach clip frame uiFrame with PreRollPackets packets of pre-roll.
  static plUInt64 GetDecodeStartPacket(plUInt64 uiFrame, plUInt32 uiPreSkip)
  {
    const plUInt64 uiPacket = GetPacketOfFrame(uiFrame, uiPreSkip);
    return uiPacket > PreRollPackets ? uiPacket - PreRollPackets : 0;
  }

  /// \brief Encodes interleaved float samples (mono or stereo, 48 kHz) to the payload layout above.
  ///
  /// uiBitrate is the total bitrate in bits per second (all channels together); the encoder runs in VBR mode at
  /// complexity 10. Writes the pre-skip (the encoder lookahead) and one seek table entry every SeekIntervalPackets
  /// packets, starting with packet 0.
  static plResult Encode(plArrayPtr<const float> samples, plUInt8 uiChannels, plUInt32 uiBitrate, plDynamicArray<plUInt8>& out_payload, plUInt32& out_uiPreSkip, plDynamicArray<plAudioClipSeekEntry>& out_seekTable);

  /// \brief Decodes a whole payload to uiFrames interleaved s16 frames, dropping the pre-skip.
  static plResult Decode(plArrayPtr<const plUInt8> payload, plUInt8 uiChannels, plUInt32 uiPreSkip, plUInt64 uiFrames, plDynamicArray<plInt16>& out_pcm);
};
