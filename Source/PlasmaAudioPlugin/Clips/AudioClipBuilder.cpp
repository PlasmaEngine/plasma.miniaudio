#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/IO/OSFile.h>
#include <Foundation/IO/Stream.h>
#include <Foundation/Strings/PathUtils.h>
#include <Foundation/Strings/StringConversion.h>
#include <PlasmaAudioPlugin/Clips/AudioClipBuilder.h>
#include <PlasmaAudioPlugin/Clips/AudioClipDesc.h>
#include <PlasmaAudioPlugin/Codecs/ImaAdpcm.h>
#include <PlasmaAudioPlugin/Codecs/LoudnessMeter.h>
#include <PlasmaAudioPlugin/Codecs/OpusCodec.h>

#include <cmath>

namespace
{
  /// One language after processing, before encoding.
  struct ProcessedAudio
  {
    plDynamicArray<float> m_Samples;
    plUInt32 m_uiSampleRate = 0;
    plUInt8 m_uiChannels = 0;
    bool m_bHasLoop = false;
    plUInt64 m_uiLoopStart = 0;
    plUInt64 m_uiLoopEnd = 0;
    float m_fMeasuredLufs = -plMath::Infinity<float>();
    float m_fAppliedGainDb = 0.0f;

    plUInt64 GetFrames() const { return m_Samples.GetCount() / m_uiChannels; }
  };

  float GetPeak(plArrayPtr<const float> samples)
  {
    float fPeak = 0.0f;
    for (float f : samples)
      fPeak = plMath::Max(fPeak, plMath::Abs(f));
    return fPeak;
  }

  plUInt64 ScaleFrame(plUInt64 uiFrame, plUInt32 uiRateIn, plUInt32 uiRateOut)
  {
    return static_cast<plUInt64>(static_cast<double>(uiFrame) * uiRateOut / uiRateIn + 0.5);
  }

  plResult Process(const plAudioClipSourcePcm& source, const plAudioClipBuildSettings& settings, plUInt32 uiTargetRate, ProcessedAudio& out_audio)
  {
    if (source.m_uiChannels == 0 || source.m_uiSampleRate == 0 || source.GetFrames() == 0)
    {
      plLog::Error("Audio clip source is empty");
      return PL_FAILURE;
    }

    const plUInt64 uiSourceFrames = source.GetFrames();

    // downmix
    if (settings.m_bForceMono && source.m_uiChannels > 1)
    {
      out_audio.m_uiChannels = 1;
      out_audio.m_Samples.SetCountUninitialized(static_cast<plUInt32>(uiSourceFrames));

      const float fScale = 1.0f / source.m_uiChannels;
      for (plUInt64 f = 0; f < uiSourceFrames; ++f)
      {
        float fSum = 0.0f;
        for (plUInt32 c = 0; c < source.m_uiChannels; ++c)
          fSum += source.m_Samples[static_cast<plUInt32>(f * source.m_uiChannels + c)];

        out_audio.m_Samples[static_cast<plUInt32>(f)] = fSum * fScale;
      }
    }
    else
    {
      out_audio.m_uiChannels = source.m_uiChannels;
      out_audio.m_Samples = source.m_Samples;
    }

    // loop points in source frames
    plInt64 iLoopStart = settings.m_iLoopStart >= 0 ? settings.m_iLoopStart : (source.m_bHasLoop ? static_cast<plInt64>(source.m_uiLoopStart) : -1);
    plInt64 iLoopEnd = settings.m_iLoopEnd >= 0 ? settings.m_iLoopEnd : (source.m_bHasLoop ? static_cast<plInt64>(source.m_uiLoopEnd) : -1);

    if (iLoopStart >= 0 && iLoopEnd < 0)
      iLoopEnd = static_cast<plInt64>(uiSourceFrames);

    iLoopEnd = plMath::Min<plInt64>(iLoopEnd, static_cast<plInt64>(uiSourceFrames));
    out_audio.m_bHasLoop = iLoopStart >= 0 && iLoopEnd > iLoopStart;
    out_audio.m_uiLoopStart = out_audio.m_bHasLoop ? static_cast<plUInt64>(iLoopStart) : 0;
    out_audio.m_uiLoopEnd = out_audio.m_bHasLoop ? static_cast<plUInt64>(iLoopEnd) : 0;

    // resample
    out_audio.m_uiSampleRate = source.m_uiSampleRate;
    if (uiTargetRate != 0 && uiTargetRate != source.m_uiSampleRate)
    {
      plDynamicArray<float> resampled;
      PL_SUCCEED_OR_RETURN(plAudioClipBuilder::Resample(out_audio.m_Samples, out_audio.m_uiChannels, source.m_uiSampleRate, uiTargetRate, resampled));
      out_audio.m_Samples = std::move(resampled);
      out_audio.m_uiLoopStart = ScaleFrame(out_audio.m_uiLoopStart, source.m_uiSampleRate, uiTargetRate);
      out_audio.m_uiLoopEnd = plMath::Min(ScaleFrame(out_audio.m_uiLoopEnd, source.m_uiSampleRate, uiTargetRate), out_audio.GetFrames());
      out_audio.m_uiSampleRate = uiTargetRate;
    }

    const plUInt8 uiChannels = out_audio.m_uiChannels;

    // trim silence, never into the loop
    if (plMath::IsFinite(settings.m_fTrimSilenceDb))
    {
      const float fThreshold = plMath::Pow(10.0f, settings.m_fTrimSilenceDb / 20.0f);
      const plUInt64 uiFrames = out_audio.GetFrames();

      auto IsLoud = [&](plUInt64 uiFrame)
      {
        for (plUInt32 c = 0; c < uiChannels; ++c)
        {
          if (plMath::Abs(out_audio.m_Samples[static_cast<plUInt32>(uiFrame * uiChannels + c)]) > fThreshold)
            return true;
        }
        return false;
      };

      plUInt64 uiFirst = 0;
      while (uiFirst < uiFrames && !IsLoud(uiFirst))
        ++uiFirst;

      plUInt64 uiEnd = uiFrames;
      while (uiEnd > uiFirst && !IsLoud(uiEnd - 1))
        --uiEnd;

      if (out_audio.m_bHasLoop)
      {
        uiFirst = plMath::Min(uiFirst, out_audio.m_uiLoopStart);
        uiEnd = plMath::Max(uiEnd, out_audio.m_uiLoopEnd);
      }

      if (uiEnd <= uiFirst)
      {
        plLog::Error("Audio clip source is silent below the trim level of {} dB", settings.m_fTrimSilenceDb);
        return PL_FAILURE;
      }

      if (uiFirst > 0 || uiEnd < uiFrames)
      {
        plDynamicArray<float> trimmed;
        trimmed.SetCountUninitialized(static_cast<plUInt32>((uiEnd - uiFirst) * uiChannels));
        plMemoryUtils::Copy(trimmed.GetData(), out_audio.m_Samples.GetData() + uiFirst * uiChannels, static_cast<size_t>(trimmed.GetCount()));
        out_audio.m_Samples = std::move(trimmed);

        if (out_audio.m_bHasLoop)
        {
          out_audio.m_uiLoopStart -= uiFirst;
          out_audio.m_uiLoopEnd -= uiFirst;
        }
      }
    }

    // loudness, measured at 48 kHz
    {
      plLoudnessMeter meter(uiChannels);

      if (out_audio.m_uiSampleRate == plLoudnessMeter::SampleRate)
      {
        meter.Process(out_audio.m_Samples.GetData(), out_audio.GetFrames());
      }
      else
      {
        plDynamicArray<float> measureCopy;
        PL_SUCCEED_OR_RETURN(plAudioClipBuilder::Resample(out_audio.m_Samples, uiChannels, out_audio.m_uiSampleRate, plLoudnessMeter::SampleRate, measureCopy));
        meter.Process(measureCopy.GetData(), measureCopy.GetCount() / uiChannels);
      }

      out_audio.m_fMeasuredLufs = static_cast<float>(meter.GetIntegratedLufs());
    }

    // gain
    if (settings.m_fNormaliseLufs != 0.0f)
    {
      const float fPeak = GetPeak(out_audio.m_Samples);

      if (fPeak > 0.0f)
      {
        const float fPeakDb = 20.0f * std::log10(fPeak);

        float fGainDb = plMath::IsFinite(out_audio.m_fMeasuredLufs) ? settings.m_fNormaliseLufs - out_audio.m_fMeasuredLufs : settings.m_fPeakFallbackDb - fPeakDb;
        fGainDb = plMath::Min(fGainDb, -fPeakDb);

        const float fGain = plMath::Pow(10.0f, fGainDb / 20.0f);
        for (float& f : out_audio.m_Samples)
          f *= fGain;

        out_audio.m_fAppliedGainDb = fGainDb;
      }
    }

    return PL_SUCCESS;
  }

  void Quantise(plArrayPtr<const float> samples, plDynamicArray<plInt16>& out_pcm)
  {
    out_pcm.SetCountUninitialized(samples.GetCount());
    for (plUInt32 i = 0; i < samples.GetCount(); ++i)
    {
      const float f = plMath::Clamp(samples[i], -1.0f, 1.0f) * 32767.0f;
      out_pcm[i] = static_cast<plInt16>(plMath::Clamp<plInt32>(static_cast<plInt32>(std::lround(f)), -32768, 32767));
    }
  }

  void ComputePeaks(const ProcessedAudio& audio, plUInt32 uiBuckets, plDynamicArray<plInt8>& out_peaks)
  {
    const plUInt8 uiChannels = audio.m_uiChannels;
    const plUInt64 uiFrames = audio.GetFrames();

    out_peaks.SetCount(uiBuckets * uiChannels * 2);

    for (plUInt32 b = 0; b < uiBuckets; ++b)
    {
      const plUInt64 uiBegin = uiFrames * b / uiBuckets;
      const plUInt64 uiEnd = plMath::Max(uiFrames * (b + 1) / uiBuckets, plMath::Min(uiBegin + 1, uiFrames));

      for (plUInt32 c = 0; c < uiChannels; ++c)
      {
        // the true range of the bucket: a slow wave spends a whole bucket on one side of zero
        float fMin = plMath::MaxValue<float>();
        float fMax = -plMath::MaxValue<float>();

        for (plUInt64 f = uiBegin; f < uiEnd; ++f)
        {
          const float fValue = audio.m_Samples[static_cast<plUInt32>(f * uiChannels + c)];
          fMin = plMath::Min(fMin, fValue);
          fMax = plMath::Max(fMax, fValue);
        }

        if (uiBegin >= uiEnd)
        {
          fMin = 0.0f;
          fMax = 0.0f;
        }

        out_peaks[(b * uiChannels + c) * 2 + 0] = static_cast<plInt8>(std::lround(plMath::Clamp(fMin, -1.0f, 1.0f) * 127.0f));
        out_peaks[(b * uiChannels + c) * 2 + 1] = static_cast<plInt8>(std::lround(plMath::Clamp(fMax, -1.0f, 1.0f) * 127.0f));
      }
    }
  }

  void EncodeAdpcm(const plDynamicArray<plInt16>& pcm, plUInt8 uiChannels, plUInt64 uiFrames, plDynamicArray<plUInt8>& out_payload)
  {
    const plUInt32 uiBlockBytes = plImaAdpcmCodec::GetBlockBytes(uiChannels);
    const plUInt64 uiBlocks = plImaAdpcmCodec::GetBlockCount(uiFrames);

    out_payload.SetCountUninitialized(static_cast<plUInt32>(uiBlocks * uiBlockBytes));

    plHybridArray<plInt16, 8> predictors;
    plHybridArray<plUInt8, 8> stepIndices;
    predictors.SetCount(uiChannels);
    stepIndices.SetCount(uiChannels);

    for (plUInt64 b = 0; b < uiBlocks; ++b)
    {
      const plUInt64 uiFirst = b * plImaAdpcmCodec::FramesPerBlock;
      const plUInt32 uiCount = static_cast<plUInt32>(plMath::Min<plUInt64>(plImaAdpcmCodec::FramesPerBlock, uiFrames - uiFirst));
      plImaAdpcmCodec::EncodeBlock(pcm.GetData() + uiFirst * uiChannels, uiCount, uiChannels, predictors.GetData(), stepIndices.GetData(), out_payload.GetData() + b * uiBlockBytes);
    }
  }

  plUInt32 ReadLE32(const plUInt8* pData)
  {
    return static_cast<plUInt32>(pData[0]) | (static_cast<plUInt32>(pData[1]) << 8) | (static_cast<plUInt32>(pData[2]) << 16) | (static_cast<plUInt32>(pData[3]) << 24);
  }

  /// Walks the RIFF chunks of a WAV file and returns the first and the (inclusive) last frame of the first loop
  /// in its smpl chunk.
  bool ReadWavSmplLoop(plStringView sPath, plUInt32& out_uiStart, plUInt32& out_uiLast)
  {
    plOSFile file;
    if (file.Open(sPath, plFileOpenMode::Read).Failed())
      return false;

    plUInt8 riff[12];
    if (file.Read(riff, 12) != 12 || plMemoryUtils::Compare(riff, reinterpret_cast<const plUInt8*>("RIFF"), 4) != 0 ||
        plMemoryUtils::Compare(riff + 8, reinterpret_cast<const plUInt8*>("WAVE"), 4) != 0)
      return false;

    plUInt8 chunk[8];
    while (file.Read(chunk, 8) == 8)
    {
      const plUInt32 uiSize = ReadLE32(chunk + 4);

      if (plMemoryUtils::Compare(chunk, reinterpret_cast<const plUInt8*>("smpl"), 4) == 0)
      {
        // 36 bytes of sampler data, then 24 bytes per loop: cue id, type, start, end, fraction, play count
        plUInt8 smpl[36 + 24];
        if (uiSize < sizeof(smpl) || file.Read(smpl, sizeof(smpl)) != sizeof(smpl))
          return false;

        if (ReadLE32(smpl + 28) == 0)
          return false;

        out_uiStart = ReadLE32(smpl + 36 + 8);
        out_uiLast = ReadLE32(smpl + 36 + 12);
        return true;
      }

      file.SetFilePosition(static_cast<plInt64>(uiSize) + (uiSize & 1), plFileSeekMode::FromCurrent);
    }

    return false;
  }
} // namespace

plResult plAudioClipBuilder::Build(const plAudioClipSourcePcm& source, const plAudioClipBuildSettings& settings, plStreamWriter& inout_stream)
{
  const plAudioClipSourcePcm* sources[] = {&source};

  if (settings.m_Languages.GetCount() > 1)
  {
    plLog::Error("Several languages need one source each");
    return PL_FAILURE;
  }

  return Build(plArrayPtr<const plAudioClipSourcePcm* const>(sources), settings, inout_stream);
}

plResult plAudioClipBuilder::Build(plArrayPtr<const plAudioClipSourcePcm* const> sources, const plAudioClipBuildSettings& settings, plStreamWriter& inout_stream)
{
  if (sources.IsEmpty())
    return PL_FAILURE;

  if (!settings.m_Languages.IsEmpty())
  {
    if (settings.m_Languages.GetCount() != sources.GetCount() || !settings.m_Languages[0].IsEmpty())
    {
      plLog::Error("Audio clip languages must match the sources and start with the default language \"\"");
      return PL_FAILURE;
    }
  }
  else if (sources.GetCount() != 1)
  {
    plLog::Error("Several sources need language names");
    return PL_FAILURE;
  }

  const bool bOpus = settings.m_eCodec == plAudioClipCodec::Opus;

  const plUInt32 uiTargetRate = bOpus ? plOpusCodec::SampleRate : (settings.m_uiTargetSampleRate != 0 ? settings.m_uiTargetSampleRate : sources[0]->m_uiSampleRate);

  plDynamicArray<ProcessedAudio> languages;
  languages.SetCount(sources.GetCount());

  plUInt64 uiFrames = 0;
  for (plUInt32 i = 0; i < sources.GetCount(); ++i)
  {
    PL_SUCCEED_OR_RETURN(Process(*sources[i], settings, uiTargetRate, languages[i]));

    if (languages[i].m_uiChannels != languages[0].m_uiChannels)
    {
      plLog::Error("All languages of an audio clip need the same channel count");
      return PL_FAILURE;
    }

    uiFrames = plMath::Max(uiFrames, languages[i].GetFrames());
  }

  const ProcessedAudio& main = languages[0];
  const plUInt8 uiChannels = main.m_uiChannels;

  if (uiFrames * uiChannels > plMath::MaxValue<plUInt32>())
  {
    plLog::Error("Audio clip is too long");
    return PL_FAILURE;
  }

  // header
  plAudioClipHeader header;
  header.m_eCodec = settings.m_eCodec;
  header.m_eLoadMode = settings.m_eLoadMode;
  header.m_uiChannels = uiChannels;
  header.m_bHasLoop = main.m_bHasLoop;
  header.m_uiSampleRate = uiTargetRate;
  header.m_uiFrames = uiFrames;
  header.m_uiLoopStart = main.m_uiLoopStart;
  header.m_uiLoopEnd = main.m_uiLoopEnd;
  header.m_fMeasuredLufs = main.m_fMeasuredLufs;
  header.m_fAppliedGainDb = main.m_fAppliedGainDb;

  // the ADPCM seek table is the same for every language: one entry per second
  plDynamicArray<plAudioClipSeekEntry> adpcmSeekTable;

  if (settings.m_eCodec == plAudioClipCodec::ImaAdpcm)
  {
    header.m_uiBlockFrames = plImaAdpcmCodec::FramesPerBlock;
    header.m_uiBlockBytes = plImaAdpcmCodec::GetBlockBytes(uiChannels);

    for (plUInt64 uiFrame = 0; uiFrame < uiFrames; uiFrame += uiTargetRate)
    {
      const plUInt64 uiBlock = uiFrame / plImaAdpcmCodec::FramesPerBlock;
      plAudioClipSeekEntry& entry = adpcmSeekTable.ExpandAndGetRef();
      entry.m_uiByteOffset = uiBlock * header.m_uiBlockBytes;
      entry.m_uiFrame = uiBlock * plImaAdpcmCodec::FramesPerBlock;
    }
  }
  else if (bOpus)
  {
    // the seek tables and the pre-skip come from the encoder, see below
    header.m_uiBlockFrames = plOpusCodec::FramesPerPacket;
    header.m_uiBlockBytes = 0;
  }

  // about 16 frames per bucket, so a short clip still shows its waveform; capped because the peaks load with the clip
  header.m_uiPeakBuckets = static_cast<plUInt32>(plMath::Clamp<plUInt64>(main.GetFrames() / 16, plAudioClipConstants::PeakBuckets, 16384));
  ComputePeaks(main, header.m_uiPeakBuckets, header.m_Peaks);

  // payloads
  plDynamicArray<plDynamicArray<plUInt8>> payloads;
  payloads.SetCount(languages.GetCount());

  // payload offset of each language's loop priming data (the Opus seek tables differ per language)
  plDynamicArray<plUInt64> loopOffsets;
  loopOffsets.SetCount(languages.GetCount());

  for (plUInt32 i = 0; i < languages.GetCount(); ++i)
  {
    ProcessedAudio& audio = languages[i];
    audio.m_Samples.SetCount(static_cast<plUInt32>(uiFrames * uiChannels)); // pads with silence

    plDynamicArray<plInt16> pcm;
    Quantise(audio.m_Samples, pcm);

    plAudioClipLanguageEntry& entry = header.m_Languages.ExpandAndGetRef();
    entry.m_sLanguage = settings.m_Languages.IsEmpty() ? plString() : settings.m_Languages[i];

    if (settings.m_eCodec == plAudioClipCodec::ImaAdpcm)
    {
      EncodeAdpcm(pcm, uiChannels, uiFrames, payloads[i]);
      entry.m_SeekTable = adpcmSeekTable;
    }
    else if (bOpus)
    {
      plUInt32 uiPreSkip = 0;
      PL_SUCCEED_OR_RETURN(plOpusCodec::Encode(audio.m_Samples, uiChannels, settings.m_uiOpusBitrateKbps * 1000, payloads[i], uiPreSkip, entry.m_SeekTable));

      // the encoder lookahead depends on the encoder settings only, which all languages share
      if (i > 0 && uiPreSkip != header.m_uiPreSkip)
      {
        plLog::Error("The Opus pre-skip differs between the languages of the audio clip");
        return PL_FAILURE;
      }

      header.m_uiPreSkip = uiPreSkip;
    }
    else
    {
      payloads[i].SetCountUninitialized(pcm.GetCount() * sizeof(plInt16));
      plMemoryUtils::Copy(payloads[i].GetData(), reinterpret_cast<const plUInt8*>(pcm.GetData()), payloads[i].GetCount());
    }

    entry.m_uiPayloadBytes = payloads[i].GetCount();

    if (settings.m_eLoadMode == plAudioClipLoadMode::Streamed)
    {
      entry.m_uiPrimingBytes = static_cast<plUInt32>(plMath::Min<plUInt64>(plAudioClipConstants::PrimingBytes, entry.m_uiPayloadBytes));

      if (header.m_bHasLoop)
      {
        header.SelectLanguage(i);
        loopOffsets[i] = plMath::Min(header.GetLoopPrimingOffset(), entry.m_uiPayloadBytes);
        entry.m_uiLoopPrimingBytes = static_cast<plUInt32>(plMath::Min<plUInt64>(plAudioClipConstants::PrimingBytes, entry.m_uiPayloadBytes - loopOffsets[i]));
      }
    }
  }

  header.SelectLanguage(0);

  // write
  header.Write(inout_stream);

  for (plUInt32 i = 0; i < languages.GetCount(); ++i)
  {
    const plAudioClipLanguageEntry& entry = header.m_Languages[i];
    const plDynamicArray<plUInt8>& payload = payloads[i];

    PL_SUCCEED_OR_RETURN(inout_stream.WriteBytes(payload.GetData(), entry.m_uiPrimingBytes));
    PL_SUCCEED_OR_RETURN(inout_stream.WriteBytes(payload.GetData() + loopOffsets[i], entry.m_uiLoopPrimingBytes));
    PL_SUCCEED_OR_RETURN(inout_stream.WriteBytes(payload.GetData(), payload.GetCount()));
  }

  return PL_SUCCESS;
}

plResult plAudioClipBuilder::DecodeSourceFile(plStringView sAbsolutePath, plAudioClipSourcePcm& out_source)
{
  out_source = plAudioClipSourcePcm();

  const plStringBuilder sPath = sAbsolutePath;

#if PL_ENABLED(PL_PLATFORM_WINDOWS)
  const plStringWChar sPathW(sPath);
#endif

  {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;

#if PL_ENABLED(PL_PLATFORM_WINDOWS)
    const ma_result initResult = ma_decoder_init_file_w(sPathW.GetData(), &cfg, &decoder);
#else
    const ma_result initResult = ma_decoder_init_file(sPath.GetData(), &cfg, &decoder);
#endif

    if (initResult != MA_SUCCESS)
    {
      plLog::Error("Could not decode audio file '{}' (miniaudio error {})", sPath, static_cast<int>(initResult));
      return PL_FAILURE;
    }

    ma_format format = ma_format_unknown;
    ma_uint32 uiChannels = 0;
    ma_uint32 uiSampleRate = 0;
    ma_decoder_get_data_format(&decoder, &format, &uiChannels, &uiSampleRate, nullptr, 0);

    if (uiChannels == 0 || uiChannels > 255 || uiSampleRate == 0)
    {
      ma_decoder_uninit(&decoder);
      plLog::Error("Audio file '{}' has an unsupported format", sPath);
      return PL_FAILURE;
    }

    out_source.m_uiChannels = static_cast<plUInt8>(uiChannels);
    out_source.m_uiSampleRate = uiSampleRate;

    constexpr ma_uint64 uiChunkFrames = 4096;
    while (true)
    {
      const plUInt32 uiOld = out_source.m_Samples.GetCount();
      out_source.m_Samples.SetCountUninitialized(uiOld + static_cast<plUInt32>(uiChunkFrames * uiChannels));

      ma_uint64 uiRead = 0;
      const ma_result readResult = ma_decoder_read_pcm_frames(&decoder, out_source.m_Samples.GetData() + uiOld, uiChunkFrames, &uiRead);
      out_source.m_Samples.SetCount(uiOld + static_cast<plUInt32>(uiRead * uiChannels));

      if (uiRead == 0 || (readResult != MA_SUCCESS && readResult != MA_AT_END))
        break;
    }

    ma_decoder_uninit(&decoder);

    if (out_source.m_Samples.IsEmpty())
    {
      plLog::Error("Audio file '{}' contains no samples", sPath);
      return PL_FAILURE;
    }
  }

  if (plPathUtils::HasExtension(sPath, ".wav"))
  {
    plUInt32 uiLoopStart = 0;
    plUInt32 uiLoopLast = 0;
    if (ReadWavSmplLoop(sPath, uiLoopStart, uiLoopLast) && uiLoopLast >= uiLoopStart && uiLoopStart < out_source.GetFrames())
    {
      out_source.m_bHasLoop = true;
      out_source.m_uiLoopStart = uiLoopStart;
      out_source.m_uiLoopEnd = plMath::Min<plUInt64>(static_cast<plUInt64>(uiLoopLast) + 1, out_source.GetFrames());
    }
  }

  return PL_SUCCESS;
}

plResult plAudioClipBuilder::Resample(plArrayPtr<const float> input, plUInt8 uiChannels, plUInt32 uiRateIn, plUInt32 uiRateOut, plDynamicArray<float>& out_output)
{
  if (uiChannels == 0 || uiRateIn == 0 || uiRateOut == 0)
    return PL_FAILURE;

  const plUInt64 uiFramesIn = input.GetCount() / uiChannels;

  if (uiRateIn == uiRateOut)
  {
    out_output = input;
    return PL_SUCCESS;
  }

  ma_resampler_config cfg = ma_resampler_config_init(ma_format_f32, uiChannels, uiRateIn, uiRateOut, ma_resample_algorithm_linear);
  ma_resampler resampler;
  if (ma_resampler_init(&cfg, nullptr, &resampler) != MA_SUCCESS)
    return PL_FAILURE;

  const plUInt64 uiLatencyIn = ma_resampler_get_input_latency(&resampler);
  const plUInt64 uiLatencyOut = ma_resampler_get_output_latency(&resampler);
  const plUInt64 uiFramesOut = ScaleFrame(uiFramesIn, uiRateIn, uiRateOut);

  // the input is followed by enough silence to push the delayed tail out of the filter
  plDynamicArray<float> padded;
  padded.SetCount(static_cast<plUInt32>((uiFramesIn + uiLatencyIn + 2) * uiChannels));
  plMemoryUtils::Copy(padded.GetData(), input.GetPtr(), input.GetCount());

  plDynamicArray<float> output;
  output.SetCount(static_cast<plUInt32>((uiFramesOut + uiLatencyOut + 16) * uiChannels));

  ma_uint64 uiIn = padded.GetCount() / uiChannels;
  ma_uint64 uiOut = output.GetCount() / uiChannels;
  const ma_result res = ma_resampler_process_pcm_frames(&resampler, padded.GetData(), &uiIn, output.GetData(), &uiOut);
  ma_resampler_uninit(&resampler, nullptr);

  if (res != MA_SUCCESS)
    return PL_FAILURE;

  out_output.SetCount(static_cast<plUInt32>(uiFramesOut * uiChannels));

  const plUInt64 uiAvailable = uiOut > uiLatencyOut ? uiOut - uiLatencyOut : 0;
  const plUInt64 uiCopy = plMath::Min(uiAvailable, uiFramesOut);
  plMemoryUtils::Copy(out_output.GetData(), output.GetData() + uiLatencyOut * uiChannels, static_cast<size_t>(uiCopy * uiChannels));

  return PL_SUCCESS;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Clips_AudioClipBuilder);
