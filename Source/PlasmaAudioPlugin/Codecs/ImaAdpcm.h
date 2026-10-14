#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

/// \brief IMA ADPCM (4 bit per sample) in the standard WAV block layout (format tag 0x11).
///
/// A block holds BlockBytesPerChannel bytes per channel and FramesPerBlock frames:
/// - a header per channel: i16 predictor (the first frame of the block, stored exactly), u8 step index, u8 zero pad
/// - 4-byte words of 8 nibbles each, one word per channel in turn (channel 0 word, channel 1 word, ...), low nibble first
///
/// The decoder takes the step index from the block header, so any block decodes on its own. The encoder starts
/// each block with the index carried over from the previous block or, when that gives a smaller error, with an
/// index close to it or close to the block's first sample step.
class PL_PLASMAAUDIOPLUGIN_DLL plImaAdpcmCodec
{
public:
  static constexpr plUInt32 BlockBytesPerChannel = 256;
  static constexpr plUInt32 FramesPerBlock = 505; ///< 1 header frame + (256 - 4) * 2 nibbles

  static constexpr plUInt32 GetBlockBytes(plUInt32 uiChannels) { return BlockBytesPerChannel * uiChannels; }

  /// \brief Number of blocks needed to hold uiFrames frames.
  static constexpr plUInt64 GetBlockCount(plUInt64 uiFrames) { return (uiFrames + FramesPerBlock - 1) / FramesPerBlock; }

  /// \brief Encodes one block.
  ///
  /// \param pPcm uiFrames interleaved frames; when uiFrames < FramesPerBlock, the block is padded with the last frame.
  /// \param inout_pPredictors per channel: receives the last reconstructed sample of the block.
  /// \param inout_pStepIndices per channel: step index at the end of the previous block (0 for the first block),
  /// receives the step index at the end of this block.
  /// \param out_pBlock GetBlockBytes(uiChannels) bytes.
  static void EncodeBlock(const plInt16* pPcm, plUInt32 uiFrames, plUInt8 uiChannels, plInt16* inout_pPredictors, plUInt8* inout_pStepIndices, plUInt8* out_pBlock);

  /// \brief Decodes one block into FramesPerBlock interleaved frames.
  ///
  /// Returns false (and writes silence) when a block header holds an invalid step index.
  static bool DecodeBlock(const plUInt8* pBlock, plUInt8 uiChannels, plInt16* out_pPcm);
};
