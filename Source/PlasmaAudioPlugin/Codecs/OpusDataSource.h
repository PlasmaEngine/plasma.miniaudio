#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <MiniAudio/miniaudio.h>
#include <PlasmaAudioPlugin/Clips/AudioClipDesc.h>

class plAudioByteSource;
struct OpusDecoder;

/// \brief A miniaudio data source that decodes the Opus payload of a clip (see plOpusCodec) from a plAudioByteSource.
///
/// Outputs s16 frames at 48 kHz. The last decoded packet is cached. The decoder state runs forward packet by packet;
/// reading on from the cursor, or seeking forward by at most plOpusCodec::PreRollPackets packets, just decodes the
/// packets in between. Any other target (a backward seek, a loop jump, a long forward seek) repositions: decoding
/// restarts with a reset decoder at plOpusCodec::GetDecodeStartPacket() of the target, reached by walking the packet
/// length prefixes from the last seek table entry at or before it, and the pre-roll packets are decoded and
/// discarded. Seek itself only moves the cursor; the work happens on the next read.
///
/// When the byte source cannot deliver a packet that is needed (a streamed clip whose data has not arrived yet), the
/// read outputs silence for all requested frames, leaves the cursor where it is and counts an underrun, so playback
/// resumes at the same position once the data is there. Packets decoded before the shortage are kept.
///
/// The struct must not move after plOpusDataSource_Init (miniaudio keeps a pointer to it). Its arrays, including the
/// decoder state, keep their memory across plOpusDataSource_Uninit, so re-initializing a source for another clip does
/// not allocate unless it needs more (a stereo clip after mono ones); the memory is freed with the struct.
struct PL_PLASMAAUDIOPLUGIN_DLL plOpusDataSource
{
  ma_data_source_base m_Base;
  plAudioByteSource* m_pBytes = nullptr;
  OpusDecoder* m_pDecoder = nullptr;     ///< points into m_DecoderState while initialized
  plDynamicArray<plUInt8> m_DecoderState; ///< opus_decoder_get_size() bytes, set up with opus_decoder_init()
  plUInt8 m_uiChannels = 0;
  plUInt32 m_uiPreSkip = 0;
  plUInt64 m_uiFrames = 0;
  plUInt64 m_uiCursor = 0;                               ///< clip frame
  plUInt64 m_uiNextPacket = 0;                           ///< packet the decoder state continues with
  plUInt64 m_uiNextPacketOffset = 0;                     ///< payload offset of m_uiNextPacket
  plUInt64 m_uiCachedPacket = 0xFFFFFFFFFFFFFFFFull;     ///< packet whose frames are in m_PacketPcm
  plAtomicInteger32 m_iUnderruns;
  plDynamicArray<plAudioClipSeekEntry> m_SeekTable;
  plDynamicArray<plInt16> m_PacketPcm;  ///< decoded frames of m_uiCachedPacket
  plDynamicArray<plUInt8> m_PacketData; ///< encoded bytes of the packet being decoded
};

/// \brief Sets up the source for the Opus clip described by header. Memory is only allocated here, and only when the
/// arrays kept from a previous use are too small; reads do not allocate.
PL_PLASMAAUDIOPLUGIN_DLL ma_result plOpusDataSource_Init(plAudioByteSource* pBytes, const plAudioClipHeader& header, plOpusDataSource* pSource);

PL_PLASMAAUDIOPLUGIN_DLL void plOpusDataSource_Uninit(plOpusDataSource* pSource);
