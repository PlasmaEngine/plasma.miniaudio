#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Clips/AudioClipDesc.h>
#include <PlasmaAudioPlugin/Codecs/AudioCodecBenchmark.h>
#include <PlasmaAudioPlugin/Codecs/ImaAdpcm.h>
#include <PlasmaAudioPlugin/Codecs/OpusCodec.h>

#if defined(PL_OPUS_HAS_ARCH_CAP)
#  include <plasma_opus_arch.h>
#endif

namespace
{
  constexpr plUInt32 s_uiSignalSeconds = 4;
  constexpr plUInt8 s_uiChannels = 2;

  /// A stereo test signal: two tones that drift apart, with some noise, so the coder has work to do.
  void MakeSignal(plDynamicArray<float>& out_samples)
  {
    const plUInt32 uiFrames = s_uiSignalSeconds * plOpusCodec::SampleRate;
    out_samples.SetCountUninitialized(uiFrames * s_uiChannels);

    plUInt32 uiNoise = 0x12345678u;
    for (plUInt32 i = 0; i < uiFrames; ++i)
    {
      const double t = static_cast<double>(i) / plOpusCodec::SampleRate;
      uiNoise = uiNoise * 1664525u + 1013904223u;
      const float fNoise = (static_cast<float>(uiNoise >> 8) / 16777216.0f - 0.5f) * 0.05f;

      out_samples[i * 2 + 0] = 0.3f * static_cast<float>(plMath::Sin(plAngle::MakeFromRadian(static_cast<float>(t * 2.0 * plMath::Pi<double>() * 220.0)))) + fNoise;
      out_samples[i * 2 + 1] = 0.3f * static_cast<float>(plMath::Sin(plAngle::MakeFromRadian(static_cast<float>(t * 2.0 * plMath::Pi<double>() * (330.0 + t * 20.0))))) + fNoise;
    }
  }

  /// Runs pDecode again and again for budget and returns the voices per 1 % of one core.
  template <typename DECODE>
  float Measure(plTime budget, DECODE decode)
  {
    plUInt32 uiPasses = 0;
    const plTime tStart = plTime::Now();
    plTime tElapsed;

    do
    {
      if (decode().Failed())
        return 0.0f;

      ++uiPasses;
      tElapsed = plTime::Now() - tStart;
    } while (tElapsed < budget);

    const double fRealTimeFactor = (static_cast<double>(uiPasses) * s_uiSignalSeconds) / plMath::Max(tElapsed.GetSeconds(), 1e-6);
    return static_cast<float>(fRealTimeFactor / 100.0);
  }
} // namespace

plString plAudioCodecBenchmark::GetOpusSimdName()
{
#if defined(PL_OPUS_HAS_ARCH_CAP)
  static const char* s_szNames[] = {"plain C", "SSE", "SSE2", "SSE4.1", "AVX2"};
  const int iArch = plMath::Clamp(opus_plasma_detected_arch(), 0, 4);
  return s_szNames[iArch];
#else
  return "plain C";
#endif
}

plResult plAudioCodecBenchmark::Run(plTime budget, Result& out_result)
{
  out_result = Result();
  out_result.m_sSimd = GetOpusSimdName();

  plDynamicArray<float> signal;
  MakeSignal(signal);

  const plUInt64 uiFrames = signal.GetCount() / s_uiChannels;

  // Opus at the bitrate clips build with by default (96 kbps)
  plDynamicArray<plUInt8> opusPayload;
  plDynamicArray<plAudioClipSeekEntry> seekTable;
  plUInt32 uiPreSkip = 0;
  PL_SUCCEED_OR_RETURN(plOpusCodec::Encode(signal, s_uiChannels, 96000, opusPayload, uiPreSkip, seekTable));

  plDynamicArray<plInt16> pcm;
  auto decodeOpus = [&]()
  { return plOpusCodec::Decode(opusPayload, s_uiChannels, uiPreSkip, uiFrames, pcm); };

  out_result.m_fOpusSimdVoices = Measure(budget, decodeOpus);

#if defined(PL_OPUS_HAS_ARCH_CAP)
  // the decoder picks its kernels when it is created, which every Decode does
  const int iPreviousCap = opus_plasma_max_arch;
  opus_plasma_max_arch = 0;
  out_result.m_fOpusPlainVoices = Measure(budget, decodeOpus);
  opus_plasma_max_arch = iPreviousCap;
#endif

  // IMA ADPCM blocks of the same signal
  plDynamicArray<plInt16> pcm16;
  pcm16.SetCountUninitialized(signal.GetCount());
  for (plUInt32 i = 0; i < signal.GetCount(); ++i)
  {
    pcm16[i] = static_cast<plInt16>(plMath::Clamp(signal[i], -1.0f, 1.0f) * 32767.0f);
  }

  const plUInt64 uiBlocks = plImaAdpcmCodec::GetBlockCount(uiFrames);
  const plUInt32 uiBlockBytes = plImaAdpcmCodec::GetBlockBytes(s_uiChannels);

  plDynamicArray<plUInt8> adpcm;
  adpcm.SetCount(static_cast<plUInt32>(uiBlocks * uiBlockBytes));

  plInt16 predictors[s_uiChannels] = {};
  plUInt8 stepIndices[s_uiChannels] = {};
  plDynamicArray<plInt16> blockPcm;
  blockPcm.SetCount(plImaAdpcmCodec::FramesPerBlock * s_uiChannels);

  for (plUInt64 uiBlock = 0; uiBlock < uiBlocks; ++uiBlock)
  {
    const plUInt64 uiFirst = uiBlock * plImaAdpcmCodec::FramesPerBlock;
    const plUInt32 uiBlockFrames = static_cast<plUInt32>(plMath::Min<plUInt64>(plImaAdpcmCodec::FramesPerBlock, uiFrames - uiFirst));

    plMemoryUtils::ZeroFill(blockPcm.GetData(), blockPcm.GetCount());
    plMemoryUtils::Copy(blockPcm.GetData(), pcm16.GetData() + uiFirst * s_uiChannels, uiBlockFrames * s_uiChannels);
    plImaAdpcmCodec::EncodeBlock(blockPcm.GetData(), plImaAdpcmCodec::FramesPerBlock, s_uiChannels, predictors, stepIndices, adpcm.GetData() + uiBlock * uiBlockBytes);
  }

  out_result.m_fAdpcmVoices = Measure(budget, [&]() -> plResult
    {
      for (plUInt64 uiBlock = 0; uiBlock < uiBlocks; ++uiBlock)
      {
        plImaAdpcmCodec::DecodeBlock(adpcm.GetData() + uiBlock * uiBlockBytes, s_uiChannels, blockPcm.GetData());
      }
      return PL_SUCCESS;
    });

  return PL_SUCCESS;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Codecs_AudioCodecBenchmark);
