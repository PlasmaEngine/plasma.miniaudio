#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Codecs/ImaAdpcm.h>

namespace
{
  constexpr plInt32 s_IndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

  constexpr plInt32 s_StepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

  constexpr plInt32 s_iMaxStepIndex = 88;

  /// Applies one nibble to the channel state. This is the reference (shift based) reconstruction, the encoder
  /// uses it too so both sides stay in lock step.
  PL_ALWAYS_INLINE plInt32 DecodeNibble(plUInt8 uiNibble, plInt32& inout_iPredictor, plInt32& inout_iStepIndex)
  {
    const plInt32 iStep = s_StepTable[inout_iStepIndex];

    plInt32 iDiff = iStep >> 3;
    if (uiNibble & 1)
      iDiff += iStep >> 2;
    if (uiNibble & 2)
      iDiff += iStep >> 1;
    if (uiNibble & 4)
      iDiff += iStep;
    if (uiNibble & 8)
      iDiff = -iDiff;

    inout_iPredictor = plMath::Clamp(inout_iPredictor + iDiff, -32768, 32767);
    inout_iStepIndex = plMath::Clamp(inout_iStepIndex + s_IndexTable[uiNibble], 0, s_iMaxStepIndex);
    return inout_iPredictor;
  }

  PL_ALWAYS_INLINE plUInt8 EncodeNibble(plInt32 iSample, plInt32& inout_iPredictor, plInt32& inout_iStepIndex)
  {
    plInt32 iStep = s_StepTable[inout_iStepIndex];
    plInt32 iDiff = iSample - inout_iPredictor;

    plUInt8 uiNibble = 0;
    if (iDiff < 0)
    {
      uiNibble = 8;
      iDiff = -iDiff;
    }

    if (iDiff >= iStep)
    {
      uiNibble |= 4;
      iDiff -= iStep;
    }
    iStep >>= 1;
    if (iDiff >= iStep)
    {
      uiNibble |= 2;
      iDiff -= iStep;
    }
    iStep >>= 1;
    if (iDiff >= iStep)
    {
      uiNibble |= 1;
    }

    DecodeNibble(uiNibble, inout_iPredictor, inout_iStepIndex);
    return uiNibble;
  }

  /// Byte that holds nibble uiNibbleIndex (0 = second frame of the block) of channel uiChannel.
  PL_ALWAYS_INLINE plUInt32 GetNibbleByte(plUInt32 uiNibbleIndex, plUInt32 uiChannel, plUInt32 uiChannels)
  {
    return 4 * uiChannels + (uiNibbleIndex / 8) * 4 * uiChannels + uiChannel * 4 + (uiNibbleIndex % 8) / 2;
  }

  PL_ALWAYS_INLINE plInt32 GetSample(const plInt16* pPcm, plUInt32 uiNibbleIndex, plUInt32 uiFrames, plUInt32 uiChannel, plUInt32 uiChannels)
  {
    // frames past the end of the input repeat the last frame
    const plUInt32 uiFrame = plMath::Min(uiNibbleIndex + 1, uiFrames - 1);
    return pPcm[uiFrame * uiChannels + uiChannel];
  }

  /// Squared error of encoding one channel of a block with the given starting step index.
  plUInt64 MeasureChannel(const plInt16* pPcm, plUInt32 uiFrames, plUInt32 uiChannel, plUInt32 uiChannels, plInt32 iStepIndex)
  {
    plInt32 iPredictor = pPcm[uiChannel];
    plUInt64 uiError = 0;

    for (plUInt32 n = 0; n < plImaAdpcmCodec::FramesPerBlock - 1; ++n)
    {
      const plInt32 iSample = GetSample(pPcm, n, uiFrames, uiChannel, uiChannels);
      EncodeNibble(iSample, iPredictor, iStepIndex);
      const plInt64 iDiff = iSample - iPredictor;
      uiError += static_cast<plUInt64>(iDiff * iDiff);
    }

    return uiError;
  }

  /// The header of every block stores its step index, so the encoder may start a block with any index. Besides
  /// the index carried over from the previous block, indices around the size of the first sample step are tried
  /// (this removes the slow step ramp-up at the start of a clip and after sudden onsets).
  plInt32 ChooseStepIndex(const plInt16* pPcm, plUInt32 uiFrames, plUInt32 uiChannel, plUInt32 uiChannels, plInt32 iCarried)
  {
    const plInt32 iFirstStep = plMath::Abs(GetSample(pPcm, 0, uiFrames, uiChannel, uiChannels) - pPcm[uiChannel]);

    plInt32 iEstimate = 0;
    while (iEstimate < s_iMaxStepIndex && s_StepTable[iEstimate] < iFirstStep)
      ++iEstimate;

    plInt32 iBest = iCarried;
    plUInt64 uiBestError = MeasureChannel(pPcm, uiFrames, uiChannel, uiChannels, iCarried);

    auto Try = [&](plInt32 iCandidate)
    {
      iCandidate = plMath::Clamp(iCandidate, 0, s_iMaxStepIndex);
      if (iCandidate == iBest)
        return;

      const plUInt64 uiError = MeasureChannel(pPcm, uiFrames, uiChannel, uiChannels, iCandidate);
      if (uiError < uiBestError)
      {
        uiBestError = uiError;
        iBest = iCandidate;
      }
    };

    for (plInt32 iOffset = -2; iOffset <= 2; ++iOffset)
    {
      Try(iCarried + iOffset);
      Try(iEstimate + iOffset);
    }

    return iBest;
  }
} // namespace

void plImaAdpcmCodec::EncodeBlock(const plInt16* pPcm, plUInt32 uiFrames, plUInt8 uiChannels, plInt16* inout_pPredictors, plUInt8* inout_pStepIndices, plUInt8* out_pBlock)
{
  PL_ASSERT_DEV(uiFrames > 0 && uiFrames <= FramesPerBlock, "Invalid frame count {}", uiFrames);

  plMemoryUtils::ZeroFill(out_pBlock, GetBlockBytes(uiChannels));

  for (plUInt32 c = 0; c < uiChannels; ++c)
  {
    plInt32 iPredictor = pPcm[c];
    plInt32 iStepIndex = ChooseStepIndex(pPcm, uiFrames, c, uiChannels, plMath::Clamp<plInt32>(inout_pStepIndices[c], 0, s_iMaxStepIndex));

    plUInt8* pHeader = out_pBlock + 4 * c;
    const plUInt16 uiPredictorBits = static_cast<plUInt16>(static_cast<plInt16>(iPredictor));
    pHeader[0] = static_cast<plUInt8>(uiPredictorBits & 0xFF);
    pHeader[1] = static_cast<plUInt8>(uiPredictorBits >> 8);
    pHeader[2] = static_cast<plUInt8>(iStepIndex);
    pHeader[3] = 0;

    for (plUInt32 n = 0; n < FramesPerBlock - 1; ++n)
    {
      const plUInt8 uiNibble = EncodeNibble(GetSample(pPcm, n, uiFrames, c, uiChannels), iPredictor, iStepIndex);

      plUInt8& ref_byte = out_pBlock[GetNibbleByte(n, c, uiChannels)];
      ref_byte |= (n & 1) ? static_cast<plUInt8>(uiNibble << 4) : uiNibble;
    }

    inout_pPredictors[c] = static_cast<plInt16>(iPredictor);
    inout_pStepIndices[c] = static_cast<plUInt8>(iStepIndex);
  }
}

bool plImaAdpcmCodec::DecodeBlock(const plUInt8* pBlock, plUInt8 uiChannels, plInt16* out_pPcm)
{
  for (plUInt32 c = 0; c < uiChannels; ++c)
  {
    const plUInt8* pHeader = pBlock + 4 * c;
    if (pHeader[2] > s_iMaxStepIndex)
    {
      plMemoryUtils::ZeroFill(out_pPcm, FramesPerBlock * uiChannels);
      return false;
    }
  }

  for (plUInt32 c = 0; c < uiChannels; ++c)
  {
    const plUInt8* pHeader = pBlock + 4 * c;
    plInt32 iPredictor = static_cast<plInt16>(static_cast<plUInt16>(pHeader[0] | (pHeader[1] << 8)));
    plInt32 iStepIndex = pHeader[2];

    out_pPcm[c] = static_cast<plInt16>(iPredictor);

    for (plUInt32 n = 0; n < FramesPerBlock - 1; ++n)
    {
      const plUInt8 uiByte = pBlock[GetNibbleByte(n, c, uiChannels)];
      const plUInt8 uiNibble = (n & 1) ? static_cast<plUInt8>(uiByte >> 4) : static_cast<plUInt8>(uiByte & 0x0F);
      out_pPcm[(n + 1) * uiChannels + c] = static_cast<plInt16>(DecodeNibble(uiNibble, iPredictor, iStepIndex));
    }
  }

  return true;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Codecs_ImaAdpcm);
