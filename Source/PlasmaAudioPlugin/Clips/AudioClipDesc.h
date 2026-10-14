#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Strings/String.h>
#include <PlasmaAudioPlugin/Codecs/AudioCodec.h>

class plStreamReader;
class plStreamWriter;

/// \brief A point at which decoding of a clip payload can start.
struct plAudioClipSeekEntry
{
  plUInt64 m_uiByteOffset = 0; ///< relative to the start of the language payload
  plUInt64 m_uiFrame = 0;      ///< first frame decoded from m_uiByteOffset
};

/// \brief The section of one language in a clip file.
///
/// In the file, the sections follow the header in the order of plAudioClipHeader::m_Languages, each laid out as
/// [start priming][loop priming][payload].
struct plAudioClipLanguageEntry
{
  plString m_sLanguage;              ///< "" is the default language and always the first entry
  plUInt32 m_uiPrimingBytes = 0;     ///< copy of the first payload bytes (streamed clips only)
  plUInt32 m_uiLoopPrimingBytes = 0; ///< copy of the payload from GetLoopPrimingOffset() on (streamed looping clips only)
  plUInt64 m_uiPayloadBytes = 0;
  plDynamicArray<plAudioClipSeekEntry> m_SeekTable; ///< seek table of this language's payload

  plUInt64 GetSectionBytes() const { return static_cast<plUInt64>(m_uiPrimingBytes) + m_uiLoopPrimingBytes + m_uiPayloadBytes; }
};

/// \brief The header of a compiled audio clip (.plBinAudioClip), written after the plAssetFileHeader.
///
/// Binary layout (version 3):
///   u8 codec, u8 loadMode, u8 channels, u8 flags (bit 0: has loop)
///   u32 sampleRate, u64 frames, u64 loopStart, u64 loopEnd
///   float measuredLufs, float appliedGainDb
///   u32 blockFrames (ADPCM 505, Opus 960, PCM 0), u32 blockBytes (ADPCM 256 * channels, otherwise 0)
///   u32 preSkip (Opus encoder lookahead, otherwise 0)
///   u32 peakBuckets, then peakBuckets * channels * (i8 min, i8 max), bucket major
///   u32 languageCount, then per language: string language, u32 primingBytes, u32 loopPrimingBytes, u64 payloadBytes,
///     u32 seekEntries, then (u64 byteOffset, u64 frame) per entry
///
/// Older versions still read: version 2 has one seek table for all languages, written as u32 seekEntries plus
/// entries between the peaks and languageCount, and no seek table per language; version 1 additionally has no preSkip
/// (read as 0).
///
/// All languages share the frame count, loop points and peaks: the builder pads every language to the longest one.
/// Each language has its own seek table (the Opus packet sizes, and therefore the byte offsets, differ per
/// language). m_SeekTable is the table of the selected language, see SelectLanguage().
///
/// Opus seek table entries count frames on the decoder timeline (see plOpusCodec): entry frame = clip frame + preSkip.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioClipHeader
{
  plAudioClipCodec::Enum m_eCodec = plAudioClipCodec::Pcm16;
  plAudioClipLoadMode::Enum m_eLoadMode = plAudioClipLoadMode::DecompressOnLoad;
  plUInt8 m_uiChannels = 1;
  bool m_bHasLoop = false;

  plUInt32 m_uiSampleRate = 48000;
  plUInt64 m_uiFrames = 0;
  plUInt64 m_uiLoopStart = 0;
  plUInt64 m_uiLoopEnd = 0; ///< exclusive

  float m_fMeasuredLufs = 0.0f; ///< integrated loudness of the default language before gain, -infinity when not measurable
  float m_fAppliedGainDb = 0.0f;

  plUInt32 m_uiBlockFrames = 0;
  plUInt32 m_uiBlockBytes = 0;

  plUInt32 m_uiPreSkip = 0; ///< Opus: decoder frames before clip frame 0

  plUInt32 m_uiPeakBuckets = 0;
  plDynamicArray<plInt8> m_Peaks; ///< m_uiPeakBuckets * m_uiChannels * 2, (min, max) scaled by 127

  /// \brief Seek table of the selected language, used by the offset functions below. Read() selects language 0.
  plDynamicArray<plAudioClipSeekEntry> m_SeekTable;
  plDynamicArray<plAudioClipLanguageEntry> m_Languages;

  /// \brief Writes version 3, with the seek table of every language entry (m_SeekTable itself is not written).
  void Write(plStreamWriter& inout_stream) const;
  plResult Read(plStreamReader& inout_stream);

  /// \brief Index of the language entry for sLanguage, or 0 (the default language) when there is none.
  plUInt32 FindLanguage(plStringView sLanguage) const;

  /// \brief Copies the seek table of language uiLanguage into m_SeekTable.
  void SelectLanguage(plUInt32 uiLanguage);

  /// \brief Bytes per frame of a PCM16 payload.
  plUInt32 GetPcmFrameBytes() const { return m_uiChannels * static_cast<plUInt32>(sizeof(plInt16)); }

  /// \brief Payload offset at which decoding must start to reach uiFrame. For Opus this is the seek entry at or before
  /// the pre-roll start (plOpusCodec::GetDecodeStartPacket()) of uiFrame.
  plUInt64 GetPayloadOffsetForFrame(plUInt64 uiFrame) const;

  /// \brief First frame decoded when decoding starts at GetPayloadOffsetForFrame(uiFrame). For Opus this is a frame on
  /// the decoder timeline (clip frame + preSkip).
  plUInt64 GetDecodeStartFrame(plUInt64 uiFrame) const;

  /// \brief Payload offset the loop priming data starts at: where decoding starts for the loop start
  /// (GetPayloadOffsetForFrame()), i.e. the block of the loop start for ADPCM, the seek entry at or before the pre-roll
  /// start of the loop start for Opus, or the exact frame offset of the loop start for PCM16. ADPCM clips of versions 1
  /// and 2 anchor it at the seek entry at or before the loop start instead, as they were built that way.
  plUInt64 GetLoopPrimingOffset() const;

  /// \brief The version the header was read from (Read()), or the current version.
  plUInt16 m_uiVersion = 3;
};
