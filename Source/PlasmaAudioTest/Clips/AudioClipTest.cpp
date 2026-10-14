#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/IO/Archive/ArchiveBuilder.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/FileSystem/FileWriter.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <PlasmaAudioPlugin/Clips/AudioClipBuilder.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResource.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResourceLoader.h>
#include <PlasmaAudioPlugin/Clips/AudioClipVoiceSource.h>
#include <PlasmaAudioPlugin/Codecs/AdpcmDataSource.h>
#include <PlasmaAudioPlugin/Codecs/AudioByteSource.h>
#include <PlasmaAudioPlugin/Codecs/ImaAdpcm.h>
#include <PlasmaAudioPlugin/Codecs/LoudnessMeter.h>
#include <PlasmaAudioPlugin/Codecs/OpusCodec.h>
#include <PlasmaAudioPlugin/Streaming/AudioStreamService.h>
#include <Foundation/Configuration/CVar.h>
#include <TestFramework/Utilities/TestLogInterface.h>

#include <cmath>

PL_CREATE_SIMPLE_TEST_GROUP(AudioClip);

namespace
{
  constexpr plInt32 s_iAdpcmTolerance = 655; // 0.02 full scale

#define PL_CLIP_TEST_REPORT(...)                                        \
  do                                                                    \
  {                                                                     \
    plStringBuilder sReportMsg;                                         \
    sReportMsg.SetFormat(__VA_ARGS__);                                  \
    plTestFramework::Output(plTestOutput::Message, "%s", sReportMsg.GetData()); \
  } while (false)

  /// A sine per channel (frequency and phase differ per channel), interleaved.
  plDynamicArray<float> MakeSine(plUInt32 uiFrames, plUInt8 uiChannels, plUInt32 uiSampleRate, double fFrequency, float fAmplitude)
  {
    plDynamicArray<float> samples;
    samples.SetCountUninitialized(uiFrames * uiChannels);

    for (plUInt32 f = 0; f < uiFrames; ++f)
    {
      for (plUInt32 c = 0; c < uiChannels; ++c)
      {
        const double fPhase = 2.0 * plMath::Pi<double>() * (fFrequency * (1.0 + 0.5 * c)) * f / uiSampleRate + 0.7 * c;
        samples[f * uiChannels + c] = fAmplitude * static_cast<float>(std::sin(fPhase));
      }
    }

    return samples;
  }

  plDynamicArray<plInt16> ToPcm16(const plDynamicArray<float>& samples)
  {
    plDynamicArray<plInt16> pcm;
    pcm.SetCountUninitialized(samples.GetCount());
    for (plUInt32 i = 0; i < samples.GetCount(); ++i)
    {
      pcm[i] = static_cast<plInt16>(plMath::Clamp<plInt32>(static_cast<plInt32>(std::lround(plMath::Clamp(samples[i], -1.0f, 1.0f) * 32767.0f)), -32768, 32767));
    }
    return pcm;
  }

  void EncodeAdpcm(const plDynamicArray<plInt16>& pcm, plUInt8 uiChannels, plDynamicArray<plUInt8>& out_payload)
  {
    const plUInt64 uiFrames = pcm.GetCount() / uiChannels;
    const plUInt32 uiBlockBytes = plImaAdpcmCodec::GetBlockBytes(uiChannels);
    const plUInt32 uiBlocks = static_cast<plUInt32>(plImaAdpcmCodec::GetBlockCount(uiFrames));

    out_payload.SetCountUninitialized(uiBlocks * uiBlockBytes);

    plInt16 predictors[8] = {};
    plUInt8 stepIndices[8] = {};

    for (plUInt32 b = 0; b < uiBlocks; ++b)
    {
      const plUInt64 uiFirst = b * plImaAdpcmCodec::FramesPerBlock;
      const plUInt32 uiCount = static_cast<plUInt32>(plMath::Min<plUInt64>(plImaAdpcmCodec::FramesPerBlock, uiFrames - uiFirst));
      plImaAdpcmCodec::EncodeBlock(pcm.GetData() + uiFirst * uiChannels, uiCount, uiChannels, predictors, stepIndices, out_payload.GetData() + b * uiBlockBytes);
    }
  }

  plDynamicArray<plInt16> DecodeAdpcm(const plDynamicArray<plUInt8>& payload, plUInt8 uiChannels, plUInt64 uiFrames)
  {
    const plUInt32 uiBlockBytes = plImaAdpcmCodec::GetBlockBytes(uiChannels);
    const plUInt32 uiBlocks = payload.GetCount() / uiBlockBytes;

    plDynamicArray<plInt16> pcm;
    pcm.SetCountUninitialized(uiBlocks * plImaAdpcmCodec::FramesPerBlock * uiChannels);

    for (plUInt32 b = 0; b < uiBlocks; ++b)
    {
      plImaAdpcmCodec::DecodeBlock(payload.GetData() + b * uiBlockBytes, uiChannels, pcm.GetData() + b * plImaAdpcmCodec::FramesPerBlock * uiChannels);
    }

    pcm.SetCount(static_cast<plUInt32>(uiFrames * uiChannels));
    return pcm;
  }

  plInt32 MaxAbsDiff(plArrayPtr<const plInt16> a, plArrayPtr<const plInt16> b)
  {
    if (a.GetCount() != b.GetCount())
      return plMath::MaxValue<plInt32>();

    plInt32 iMax = 0;
    for (plUInt32 i = 0; i < a.GetCount(); ++i)
      iMax = plMath::Max(iMax, plMath::Abs(static_cast<plInt32>(a[i]) - static_cast<plInt32>(b[i])));
    return iMax;
  }

  void AppendU32(plDynamicArray<plUInt8>& ref_data, plUInt32 uiValue)
  {
    for (plUInt32 i = 0; i < 4; ++i)
      ref_data.PushBack(static_cast<plUInt8>((uiValue >> (8 * i)) & 0xFF));
  }

  void AppendU16(plDynamicArray<plUInt8>& ref_data, plUInt16 uiValue)
  {
    ref_data.PushBack(static_cast<plUInt8>(uiValue & 0xFF));
    ref_data.PushBack(static_cast<plUInt8>(uiValue >> 8));
  }

  void AppendTag(plDynamicArray<plUInt8>& ref_data, const char* szTag)
  {
    for (plUInt32 i = 0; i < 4; ++i)
      ref_data.PushBack(static_cast<plUInt8>(szTag[i]));
  }

  /// Wraps IMA ADPCM blocks in a WAV file (format tag 0x11) so miniaudio's WAV decoder (dr_wav) can read them.
  plDynamicArray<plUInt8> MakeImaWav(const plDynamicArray<plUInt8>& payload, plUInt8 uiChannels, plUInt32 uiSampleRate, plUInt32 uiFrames)
  {
    const plUInt32 uiBlockBytes = plImaAdpcmCodec::GetBlockBytes(uiChannels);

    plDynamicArray<plUInt8> wav;
    AppendTag(wav, "RIFF");
    AppendU32(wav, 4 + (8 + 20) + (8 + 4) + (8 + payload.GetCount()));
    AppendTag(wav, "WAVE");

    AppendTag(wav, "fmt ");
    AppendU32(wav, 20);
    AppendU16(wav, 0x11);
    AppendU16(wav, uiChannels);
    AppendU32(wav, uiSampleRate);
    AppendU32(wav, uiSampleRate * uiBlockBytes / plImaAdpcmCodec::FramesPerBlock);
    AppendU16(wav, static_cast<plUInt16>(uiBlockBytes));
    AppendU16(wav, 4);
    AppendU16(wav, 2);
    AppendU16(wav, static_cast<plUInt16>(plImaAdpcmCodec::FramesPerBlock));

    AppendTag(wav, "fact");
    AppendU32(wav, 4);
    AppendU32(wav, uiFrames);

    AppendTag(wav, "data");
    AppendU32(wav, payload.GetCount());
    wav.PushBackRange(payload);

    return wav;
  }

  /// Reads a data source to its end as s16.
  plDynamicArray<plInt16> ReadAll(ma_data_source* pSource, plUInt8 uiChannels)
  {
    plDynamicArray<plInt16> out;
    plInt16 buffer[1000 * 8];
    const ma_uint64 uiChunk = 1000;

    while (true)
    {
      ma_uint64 uiRead = 0;
      const ma_result res = ma_data_source_read_pcm_frames(pSource, buffer, uiChunk, &uiRead);
      for (ma_uint64 i = 0; i < uiRead * uiChannels; ++i)
        out.PushBack(buffer[i]);

      if (res != MA_SUCCESS || uiRead == 0)
        break;
    }

    return out;
  }

  plDynamicArray<plUInt8> BuildClip(const plAudioClipSourcePcm& source, const plAudioClipBuildSettings& settings)
  {
    plDynamicArray<plUInt8> bytes;
    plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&bytes);
    plMemoryStreamWriter writer(&storage);
    PL_TEST_RESULT(plAudioClipBuilder::Build(source, settings, writer));
    return bytes;
  }

  /// Reads the header and the default language payload of a built clip into a descriptor.
  plResult ParseClip(const plDynamicArray<plUInt8>& bytes, plAudioClipResourceDescriptor& out_desc)
  {
    plRawMemoryStreamReader reader(bytes);
    PL_SUCCEED_OR_RETURN(out_desc.m_Header.Read(reader));

    const plAudioClipLanguageEntry& language = out_desc.m_Header.m_Languages[0];
    reader.SkipBytes(static_cast<plUInt64>(language.m_uiPrimingBytes) + language.m_uiLoopPrimingBytes);

    out_desc.m_Payload.SetCountUninitialized(static_cast<plUInt32>(language.m_uiPayloadBytes));
    return reader.ReadBytes(out_desc.m_Payload.GetData(), language.m_uiPayloadBytes) == language.m_uiPayloadBytes ? PL_SUCCESS : PL_FAILURE;
  }

  plAudioClipSourcePcm MakeSource(plUInt32 uiFrames, plUInt8 uiChannels, double fFrequency = 440.0, float fAmplitude = 0.5f)
  {
    plAudioClipSourcePcm source;
    source.m_uiChannels = uiChannels;
    source.m_uiSampleRate = 48000;
    source.m_Samples = MakeSine(uiFrames, uiChannels, 48000, fFrequency, fAmplitude);
    return source;
  }

  plResult WriteClipFile(plStringView sPath, const plDynamicArray<plUInt8>& clip)
  {
    plFileWriter file;
    PL_SUCCEED_OR_RETURN(file.Open(sPath));

    plAssetFileHeader assetHeader;
    assetHeader.SetFileHashAndVersion(1, 1);
    PL_SUCCEED_OR_RETURN(assetHeader.Write(file));

    return file.WriteBytes(clip.GetData(), clip.GetCount());
  }

  /// Writes header in the layout of version 1 or 2: one seek table (that of language 0) for all languages, stored
  /// before the language entries; version 1 has no pre-skip.
  plDynamicArray<plUInt8> WriteLegacyHeader(const plAudioClipHeader& header, plUInt16 uiVersion)
  {
    plDynamicArray<plUInt8> bytes;
    plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&bytes);
    plMemoryStreamWriter writer(&storage);

    writer.WriteVersion(uiVersion);
    writer << static_cast<plUInt8>(header.m_eCodec);
    writer << static_cast<plUInt8>(header.m_eLoadMode);
    writer << header.m_uiChannels;
    writer << static_cast<plUInt8>(header.m_bHasLoop ? 1 : 0);
    writer << header.m_uiSampleRate;
    writer << header.m_uiFrames;
    writer << header.m_uiLoopStart;
    writer << header.m_uiLoopEnd;
    writer << header.m_fMeasuredLufs;
    writer << header.m_fAppliedGainDb;
    writer << header.m_uiBlockFrames;
    writer << header.m_uiBlockBytes;

    if (uiVersion >= 2)
    {
      writer << header.m_uiPreSkip;
    }

    writer << header.m_uiPeakBuckets;
    writer.WriteBytes(header.m_Peaks.GetData(), header.m_Peaks.GetCount()).AssertSuccess();

    const plDynamicArray<plAudioClipSeekEntry>& seekTable = header.m_Languages[0].m_SeekTable;
    writer << seekTable.GetCount();
    for (const plAudioClipSeekEntry& entry : seekTable)
    {
      writer << entry.m_uiByteOffset;
      writer << entry.m_uiFrame;
    }

    writer << header.m_Languages.GetCount();
    for (const plAudioClipLanguageEntry& entry : header.m_Languages)
    {
      writer << entry.m_sLanguage;
      writer << entry.m_uiPrimingBytes;
      writer << entry.m_uiLoopPrimingBytes;
      writer << entry.m_uiPayloadBytes;
    }

    return bytes;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(AudioClip, ImaAdpcm)
{
  for (plUInt8 uiChannels = 1; uiChannels <= 2; ++uiChannels)
  {
    plStringBuilder sBlock;
    sBlock.SetFormat("{} channel(s)", uiChannels);

    PL_TEST_BLOCK(plTestBlock::Enabled, sBlock.GetData())
    {
      const plUInt32 uiFrames = 48000;
      const plDynamicArray<plInt16> pcm = ToPcm16(MakeSine(uiFrames, uiChannels, 48000, 440.0, 0.5f));

      plDynamicArray<plUInt8> payload;
      EncodeAdpcm(pcm, uiChannels, payload);
      PL_TEST_INT(payload.GetCount(), plImaAdpcmCodec::GetBlockCount(uiFrames) * plImaAdpcmCodec::GetBlockBytes(uiChannels));

      const plDynamicArray<plInt16> decoded = DecodeAdpcm(payload, uiChannels, uiFrames);
      const plInt32 iMaxError = MaxAbsDiff(decoded, pcm);
      PL_CLIP_TEST_REPORT("IMA ADPCM {} ch, 1 s 440 Hz sine at 0.5: max abs error {} ({} full scale)", uiChannels, iMaxError, iMaxError / 32768.0);
      PL_TEST_BOOL(iMaxError < s_iAdpcmTolerance);

      // miniaudio's WAV decoder reads the same blocks to the same samples
      const plDynamicArray<plUInt8> wav = MakeImaWav(payload, uiChannels, 48000, uiFrames);

      ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
      ma_decoder decoder;
      if (PL_TEST_BOOL(ma_decoder_init_memory(wav.GetData(), wav.GetCount(), &cfg, &decoder) == MA_SUCCESS))
      {
        ma_uint32 uiDecChannels = 0;
        ma_decoder_get_data_format(&decoder, nullptr, &uiDecChannels, nullptr, nullptr, 0);
        PL_TEST_INT(uiDecChannels, uiChannels);

        const plDynamicArray<plInt16> reference = ReadAll(&decoder, uiChannels);
        ma_decoder_uninit(&decoder);

        // dr_wav ignores the fact chunk of IMA files and also returns the padding frames of the last block
        if (PL_TEST_BOOL(reference.GetCount() >= decoded.GetCount()))
        {
          PL_TEST_INT(MaxAbsDiff(reference.GetArrayPtr().GetSubArray(0, decoded.GetCount()), decoded), 0);
        }
      }
    }
  }
}

PL_CREATE_SIMPLE_TEST(AudioClip, Loudness)
{
  // BS.1770: a 0 dBFS (peak) 997 Hz / 1 kHz sine in one channel reads -3.01 LKFS (the -0.691 offset cancels the
  // K-weighting gain at 1 kHz, and the mean square of a sine is half its peak squared). At -20 dBFS peak that is
  // -23.01 LUFS for a mono signal, and -20.0 LUFS when both channels of a stereo signal carry it.
  PL_TEST_BLOCK(plTestBlock::Enabled, "Mono -20 dBFS sine")
  {
    const plDynamicArray<float> sine = MakeSine(5 * 48000, 1, 48000, 1000.0, 0.1f);
    plLoudnessMeter meter(1);
    meter.Process(sine.GetData(), 5 * 48000);

    PL_TEST_BOOL(meter.HasIntegratedLoudness());
    PL_CLIP_TEST_REPORT("-20 dBFS 1 kHz mono sine: {} LUFS (expected -23.01)", meter.GetIntegratedLufs());
    PL_TEST_DOUBLE(meter.GetIntegratedLufs(), -23.01, 0.1);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Stereo -20 dBFS sine")
  {
    plDynamicArray<float> stereo;
    const plDynamicArray<float> sine = MakeSine(5 * 48000, 1, 48000, 1000.0, 0.1f);
    for (float f : sine)
    {
      stereo.PushBack(f);
      stereo.PushBack(f);
    }

    plLoudnessMeter meter(2);
    meter.Process(stereo.GetData(), 5 * 48000);
    PL_CLIP_TEST_REPORT("-20 dBFS 1 kHz stereo sine: {} LUFS (expected -20.0)", meter.GetIntegratedLufs());
    PL_TEST_DOUBLE(meter.GetIntegratedLufs(), -20.0, 0.1);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Too short")
  {
    const plDynamicArray<float> sine = MakeSine(48000 / 4, 1, 48000, 1000.0, 0.1f);
    plLoudnessMeter meter(1);
    meter.Process(sine.GetData(), sine.GetCount());
    PL_TEST_BOOL(!meter.HasIntegratedLoudness());
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Builder normalisation")
  {
    // 2 s at 44.1 kHz: measured on a 48 kHz copy, normalised to -16 LUFS
    plAudioClipSourcePcm source;
    source.m_uiSampleRate = 44100;
    source.m_uiChannels = 1;
    source.m_Samples = MakeSine(2 * 44100, 1, 44100, 1000.0, 0.1f);

    plAudioClipBuildSettings settings;
    settings.m_eCodec = plAudioClipCodec::Pcm16;
    settings.m_fNormaliseLufs = -16.0f;

    plAudioClipResourceDescriptor desc;
    if (PL_TEST_RESULT(ParseClip(BuildClip(source, settings), desc)))
    {
      PL_CLIP_TEST_REPORT("Builder: measured {} LUFS, gain {} dB", desc.m_Header.m_fMeasuredLufs, desc.m_Header.m_fAppliedGainDb);
      PL_TEST_FLOAT(desc.m_Header.m_fMeasuredLufs, -23.01f, 0.2f);
      PL_TEST_FLOAT(desc.m_Header.m_fAppliedGainDb, 7.01f, 0.2f);
      PL_TEST_INT(desc.m_Header.m_uiSampleRate, 44100);
    }
  }
}

PL_CREATE_SIMPLE_TEST(AudioClip, VoiceSource)
{
  const plUInt32 uiFrames = 48000 + 123;

  for (plUInt8 uiChannels = 1; uiChannels <= 2; ++uiChannels)
  {
    const plAudioClipSourcePcm source = MakeSource(uiFrames, uiChannels);
    const plDynamicArray<plInt16> expected = ToPcm16(source.m_Samples);

    struct Case
    {
      const char* m_szName;
      plAudioClipCodec::Enum m_eCodec;
      plAudioClipLoadMode::Enum m_eLoadMode;
    };

    const Case cases[] = {
      {"PCM16 DecompressOnLoad", plAudioClipCodec::Pcm16, plAudioClipLoadMode::DecompressOnLoad},
      {"PCM16 CompressedInMemory", plAudioClipCodec::Pcm16, plAudioClipLoadMode::CompressedInMemory},
      {"ADPCM DecompressOnLoad", plAudioClipCodec::ImaAdpcm, plAudioClipLoadMode::DecompressOnLoad},
      {"ADPCM CompressedInMemory", plAudioClipCodec::ImaAdpcm, plAudioClipLoadMode::CompressedInMemory},
    };

    plDynamicArray<plInt16> adpcmResults[2];

    for (const Case& c : cases)
    {
      plStringBuilder sBlock;
      sBlock.SetFormat("{} ch {}", uiChannels, c.m_szName);

      PL_TEST_BLOCK(plTestBlock::Enabled, sBlock.GetData())
      {
        plAudioClipBuildSettings settings;
        settings.m_eCodec = c.m_eCodec;
        settings.m_eLoadMode = c.m_eLoadMode;

        plAudioClipResourceDescriptor desc;
        if (!PL_TEST_RESULT(ParseClip(BuildClip(source, settings), desc)))
          continue;

        PL_TEST_INT(desc.m_Header.m_uiFrames, uiFrames);
        PL_TEST_INT(desc.m_Header.m_uiChannels, uiChannels);
        PL_TEST_BOOL(desc.m_Header.m_uiPeakBuckets >= plAudioClipConstants::PeakBuckets);
        PL_TEST_INT(desc.m_Header.m_Peaks.GetCount(), desc.m_Header.m_uiPeakBuckets * uiChannels * 2);
        PL_TEST_INT(desc.m_Header.m_SeekTable.GetCount(), c.m_eCodec == plAudioClipCodec::ImaAdpcm ? 2 : 0);

        plStringBuilder sId;
        sId.SetFormat("PlasmaAudioTest:Clip{}{}", uiChannels, c.m_szName);
        plAudioClipResourceHandle hClip = plResourceManager::CreateResource<plAudioClipResource>(sId, std::move(desc));

        plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
        if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
          continue;

        plAudioClipVoiceSource voice;
        if (!PL_TEST_RESULT(pClip->CreateVoiceSource(voice, nullptr)))
          continue;

        ma_format format = ma_format_unknown;
        ma_uint32 uiFormatChannels = 0;
        ma_uint32 uiRate = 0;
        ma_data_source_get_data_format(voice.m_pSource, &format, &uiFormatChannels, &uiRate, nullptr, 0);
        PL_TEST_BOOL(format == ma_format_s16);
        PL_TEST_INT(uiFormatChannels, uiChannels);
        PL_TEST_INT(uiRate, 48000);

        ma_uint64 uiLength = 0;
        ma_data_source_get_length_in_pcm_frames(voice.m_pSource, &uiLength);
        PL_TEST_INT(uiLength, uiFrames);

        const plDynamicArray<plInt16> played = ReadAll(voice.m_pSource, uiChannels);

        if (c.m_eCodec == plAudioClipCodec::Pcm16)
        {
          PL_TEST_INT(MaxAbsDiff(played, expected), 0);
        }
        else
        {
          const plInt32 iMaxError = MaxAbsDiff(played, expected);
          PL_CLIP_TEST_REPORT("{}: max abs error {}", sBlock, iMaxError);
          PL_TEST_BOOL(iMaxError < s_iAdpcmTolerance);
          adpcmResults[c.m_eLoadMode == plAudioClipLoadMode::DecompressOnLoad ? 0 : 1] = played;

          // seeking into the middle of a block continues with the same samples
          const ma_uint64 uiSeek = 3 * plImaAdpcmCodec::FramesPerBlock + 17;
          PL_TEST_BOOL(ma_data_source_seek_to_pcm_frame(voice.m_pSource, uiSeek) == MA_SUCCESS);
          const plDynamicArray<plInt16> tail = ReadAll(voice.m_pSource, uiChannels);
          PL_TEST_INT(tail.GetCount(), (uiFrames - uiSeek) * uiChannels);
          PL_TEST_INT(MaxAbsDiff(tail, played.GetArrayPtr().GetSubArray(static_cast<plUInt32>(uiSeek * uiChannels))), 0);
        }

        voice.Uninit();
      }
    }

    PL_TEST_INT(MaxAbsDiff(adpcmResults[0], adpcmResults[1]), 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Header versions")
  {
    // an ADPCM streamed looping clip with two languages: the seek tables are identical, one per language
    plAudioClipSourcePcm source = MakeSource(3 * 48000, 1);
    source.m_bHasLoop = true;
    source.m_uiLoopStart = 48000 + 30000;
    source.m_uiLoopEnd = 2 * 48000;
    const plAudioClipSourcePcm* sources[] = {&source, &source};

    plAudioClipBuildSettings settings;
    settings.m_eCodec = plAudioClipCodec::ImaAdpcm;
    settings.m_eLoadMode = plAudioClipLoadMode::Streamed;
    settings.m_Languages.PushBack("");
    settings.m_Languages.PushBack("de");

    plDynamicArray<plUInt8> bytes;
    {
      plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&bytes);
      plMemoryStreamWriter writer(&storage);
      PL_TEST_RESULT(plAudioClipBuilder::Build(plArrayPtr<const plAudioClipSourcePcm* const>(sources), settings, writer));
    }

    plAudioClipHeader header;
    plRawMemoryStreamReader reader(bytes);
    if (!PL_TEST_RESULT(header.Read(reader)))
      return;

    PL_TEST_INT(header.m_uiVersion, 3);
    PL_TEST_INT(header.m_Languages.GetCount(), 2);
    PL_TEST_INT(header.m_SeekTable.GetCount(), 3);
    PL_TEST_INT(header.m_Languages[1].m_SeekTable.GetCount(), 3);

    // version 3 ADPCM: the loop priming data starts at the block of the loop start
    PL_TEST_INT(header.GetLoopPrimingOffset(), header.GetPayloadOffsetForFrame(source.m_uiLoopStart));
    PL_TEST_INT(header.GetLoopPrimingOffset(), (source.m_uiLoopStart / plImaAdpcmCodec::FramesPerBlock) * plImaAdpcmCodec::GetBlockBytes(1));

    for (plUInt16 uiVersion = 1; uiVersion <= 2; ++uiVersion)
    {
      const plDynamicArray<plUInt8> legacyBytes = WriteLegacyHeader(header, uiVersion);

      plAudioClipHeader legacy;
      plRawMemoryStreamReader legacyReader(legacyBytes);
      if (!PL_TEST_RESULT(legacy.Read(legacyReader)))
        continue;

      PL_TEST_INT(legacy.m_uiVersion, uiVersion);
      PL_TEST_INT(legacyReader.GetReadPosition(), legacyBytes.GetCount());
      PL_TEST_INT(legacy.m_Languages.GetCount(), 2);
      PL_TEST_INT(legacy.m_SeekTable.GetCount(), 3);
      PL_TEST_INT(legacy.m_Languages[1].m_SeekTable.GetCount(), 3);
      PL_TEST_INT(legacy.m_Languages[1].m_uiPayloadBytes, header.m_Languages[1].m_uiPayloadBytes);

      // clips built before version 3 copied the loop priming data from the seek entry at or before the loop start
      PL_TEST_INT(legacy.GetLoopPrimingOffset(), header.m_SeekTable[1].m_uiByteOffset);
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Underrun keeps the cursor")
  {
    class GatedBytes : public plAudioByteSource
    {
    public:
      plMemoryByteSource m_Inner;
      bool m_bOpen = false;

      virtual plUInt32 Read(plUInt64 uiOffset, void* pOut, plUInt32 uiBytes) override { return m_Inner.Read(uiOffset, pOut, uiBytes); }
      virtual bool IsAvailable(plUInt64 uiOffset, plUInt32 uiBytes) override { return m_bOpen && m_Inner.IsAvailable(uiOffset, uiBytes); }
    };

    const plDynamicArray<plInt16> pcm = ToPcm16(MakeSine(4000, 1, 48000, 440.0, 0.5f));
    plDynamicArray<plUInt8> payload;
    EncodeAdpcm(pcm, 1, payload);

    GatedBytes bytes;
    bytes.m_Inner.SetData(payload);

    plAdpcmDataSource source;
    PL_TEST_BOOL(plAdpcmDataSource_Init(&bytes, plImaAdpcmCodec::FramesPerBlock, 1, 48000, 4000, &source) == MA_SUCCESS);

    plInt16 buffer[256];
    ma_uint64 uiRead = 0;
    PL_TEST_BOOL(ma_data_source_read_pcm_frames(&source, buffer, 256, &uiRead) == MA_SUCCESS);
    PL_TEST_INT(uiRead, 256);
    PL_TEST_INT(buffer[100], 0);
    PL_TEST_INT(source.m_uiCursor, 0);
    PL_TEST_INT(source.m_iUnderruns, 1);

    bytes.m_bOpen = true;
    const plDynamicArray<plInt16> played = ReadAll(&source, 1);
    PL_TEST_INT(MaxAbsDiff(played, DecodeAdpcm(payload, 1, 4000)), 0);

    plAdpcmDataSource_Uninit(&source);
  }
}

PL_CREATE_SIMPLE_TEST(AudioClip, Loader)
{
  if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(">pltest/", "PlasmaAudioClipTest", "cliptest", plDataDirUsage::AllowWrites)))
    return;

  PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioClipTest"));
  PL_SCOPE_EXIT(plAudioClipResourceLoader::SetLanguage(""));

  PL_TEST_BLOCK(plTestBlock::Enabled, "CompressedInMemory ADPCM")
  {
    const plAudioClipSourcePcm source = MakeSource(48000, 2);

    plAudioClipBuildSettings settings;
    settings.m_eCodec = plAudioClipCodec::ImaAdpcm;
    settings.m_eLoadMode = plAudioClipLoadMode::CompressedInMemory;

    const plDynamicArray<plUInt8> clip = BuildClip(source, settings);
    if (!PL_TEST_RESULT(WriteClipFile(":cliptest/Adpcm.plBinAudioClip", clip)))
      return;

    plAudioClipResourceHandle hClip = plResourceManager::LoadResource<plAudioClipResource>(":cliptest/Adpcm.plBinAudioClip");
    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    PL_TEST_BOOL(pClip->GetCodec() == plAudioClipCodec::ImaAdpcm);
    PL_TEST_BOOL(pClip->GetLoadMode() == plAudioClipLoadMode::CompressedInMemory);
    PL_TEST_INT(pClip->GetFrames(), 48000);
    PL_TEST_INT(pClip->GetPayload().GetCount(), plImaAdpcmCodec::GetBlockCount(48000) * plImaAdpcmCodec::GetBlockBytes(2));

    plAudioClipVoiceSource voice;
    if (PL_TEST_RESULT(pClip->CreateVoiceSource(voice, nullptr)))
    {
      const plDynamicArray<plInt16> played = ReadAll(voice.m_pSource, 2);
      PL_TEST_BOOL(MaxAbsDiff(played, ToPcm16(source.m_Samples)) < s_iAdpcmTolerance);
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Streamed PCM16 keeps the payload on disk")
  {
    // 10 s stereo: 1.92 MB of payload, looping from 4 s to 9 s
    plAudioClipSourcePcm source = MakeSource(10 * 48000, 2);
    source.m_bHasLoop = true;
    source.m_uiLoopStart = 4 * 48000;
    source.m_uiLoopEnd = 9 * 48000;

    plAudioClipBuildSettings settings;
    settings.m_eCodec = plAudioClipCodec::Pcm16;
    settings.m_eLoadMode = plAudioClipLoadMode::Streamed;

    const plDynamicArray<plUInt8> clip = BuildClip(source, settings);
    if (!PL_TEST_RESULT(WriteClipFile(":cliptest/Streamed.plBinAudioClip", clip)))
      return;

    plAudioClipResourceHandle hClip = plResourceManager::LoadResource<plAudioClipResource>(":cliptest/Streamed.plBinAudioClip");
    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    const plUInt64 uiPayloadBytes = 10ull * 48000 * 2 * sizeof(plInt16);
    PL_TEST_INT(pClip->GetPayloadBytes(), uiPayloadBytes);
    PL_TEST_INT(pClip->GetPayload().GetCount(), 0);
    PL_TEST_INT(pClip->GetPcm().GetCount(), 0);
    PL_TEST_INT(pClip->GetPriming().GetCount(), plAudioClipConstants::PrimingBytes);
    PL_TEST_INT(pClip->GetLoopPriming().GetCount(), plAudioClipConstants::PrimingBytes);
    PL_TEST_INT(pClip->GetLoopPrimingOffset(), 4ull * 48000 * 4);
    const plUInt64 uiResident = pClip->GetPriming().GetCount() + pClip->GetLoopPriming().GetCount() + pClip->GetPayload().GetCount() + pClip->GetPcm().GetCount() * sizeof(plInt16);
    PL_TEST_BOOL(uiResident < uiPayloadBytes);
    PL_CLIP_TEST_REPORT("Streamed clip: {} sample bytes resident for a {} byte payload", uiResident, uiPayloadBytes);

    plUInt64 uiLoopStart = 0;
    plUInt64 uiLoopEnd = 0;
    PL_TEST_BOOL(pClip->GetLoopPoints(uiLoopStart, uiLoopEnd));
    PL_TEST_INT(uiLoopStart, 4 * 48000);
    PL_TEST_INT(uiLoopEnd, 9 * 48000);

    // the priming data and the payload range on disk hold the expected samples
    const plDynamicArray<plInt16> expected = ToPcm16(source.m_Samples);
    PL_TEST_INT(plMemoryUtils::Compare(pClip->GetPriming().GetPtr(), reinterpret_cast<const plUInt8*>(expected.GetData()), plAudioClipConstants::PrimingBytes), 0);
    PL_TEST_INT(plMemoryUtils::Compare(pClip->GetLoopPriming().GetPtr(), reinterpret_cast<const plUInt8*>(expected.GetData()) + pClip->GetLoopPrimingOffset(), plAudioClipConstants::PrimingBytes), 0);

    plOSFile file;
    if (PL_TEST_RESULT(file.Open(pClip->GetOsFilePath(), plFileOpenMode::Read)))
    {
      plDynamicArray<plUInt8> payload;
      payload.SetCountUninitialized(static_cast<plUInt32>(uiPayloadBytes));
      file.SetFilePosition(static_cast<plInt64>(pClip->GetPayloadFileOffset()), plFileSeekMode::FromStart);
      PL_TEST_INT(file.Read(payload.GetData(), uiPayloadBytes), uiPayloadBytes);
      PL_TEST_INT(plMemoryUtils::Compare(payload.GetData(), reinterpret_cast<const plUInt8*>(expected.GetData()), static_cast<size_t>(uiPayloadBytes)), 0);
    }

    // without a stream service streamed clips cannot be played
    plAudioClipVoiceSource voice;
    PL_TEST_BOOL(pClip->CreateVoiceSource(voice, nullptr).Failed());
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Languages")
  {
    const plAudioClipSourcePcm english = MakeSource(24000, 1, 440.0, 0.5f);
    const plAudioClipSourcePcm german = MakeSource(30000, 1, 660.0, 0.25f);
    const plAudioClipSourcePcm* sources[] = {&english, &german};

    plAudioClipBuildSettings settings;
    settings.m_eCodec = plAudioClipCodec::Pcm16;
    settings.m_eLoadMode = plAudioClipLoadMode::DecompressOnLoad;
    settings.m_Languages.PushBack("");
    settings.m_Languages.PushBack("de");

    plDynamicArray<plUInt8> clip;
    {
      plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&clip);
      plMemoryStreamWriter writer(&storage);
      PL_TEST_RESULT(plAudioClipBuilder::Build(plArrayPtr<const plAudioClipSourcePcm* const>(sources), settings, writer));
    }

    if (!PL_TEST_RESULT(WriteClipFile(":cliptest/Languages.plBinAudioClip", clip)))
      return;

    plAudioClipResourceLoader::SetLanguage("de");

    plAudioClipResourceHandle hClip = plResourceManager::LoadResource<plAudioClipResource>(":cliptest/Languages.plBinAudioClip");
    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    PL_TEST_STRING(pClip->GetActiveLanguage(), "de");
    PL_TEST_INT(pClip->GetFrames(), 30000); // the longest language

    plDynamicArray<plInt16> expected = ToPcm16(german.m_Samples);
    PL_TEST_INT(MaxAbsDiff(pClip->GetPcm(), expected), 0);
  }
}

namespace
{
  plInt32 GetDecoderUnderruns(const plAudioClipVoiceSource& voice)
  {
    switch (voice.m_Kind)
    {
      case plAudioClipVoiceSource::Kind::Adpcm:
        return voice.m_Adpcm.m_iUnderruns;
      case plAudioClipVoiceSource::Kind::Opus:
        return voice.m_Opus.m_iUnderruns;
      default:
        return voice.m_PcmBytes.m_iUnderruns;
    }
  }

  /// Plays a voice source in blocks of 1000 frames up to uiFrames frames or its end.
  ///
  /// Offline reads run much faster than realtime, so with bWait the IO thread is given time to fill the ring
  /// before every block (plAudioStreamService::WaitForIdle). Blocks that underrun contribute only the frames the
  /// decoder advanced over, so the result is what a listener would hear with the gaps removed.
  plDynamicArray<plInt16> PlayVoice(plAudioStreamService* pStreams, const plAudioClipVoiceSource& voice, plUInt8 uiChannels, plUInt64 uiFrames = plMath::MaxValue<plUInt64>())
  {
    plDynamicArray<plInt16> out;
    plInt16 buffer[1000 * 8];
    plUInt32 uiStarvedInARow = 0;

    while (out.GetCount() / uiChannels < uiFrames && uiStarvedInARow < 100)
    {
      if (pStreams != nullptr)
      {
        pStreams->WaitForIdle();
      }

      const ma_uint64 uiChunk = plMath::Min<plUInt64>(1000, uiFrames - out.GetCount() / uiChannels);

      ma_uint64 uiCursorBefore = 0;
      ma_data_source_get_cursor_in_pcm_frames(voice.m_pSource, &uiCursorBefore);
      const plInt32 iUnderrunsBefore = GetDecoderUnderruns(voice);

      ma_uint64 uiRead = 0;
      const ma_result res = ma_data_source_read_pcm_frames(voice.m_pSource, buffer, uiChunk, &uiRead);

      if (GetDecoderUnderruns(voice) != iUnderrunsBefore)
      {
        ma_uint64 uiCursorAfter = 0;
        ma_data_source_get_cursor_in_pcm_frames(voice.m_pSource, &uiCursorAfter);
        uiRead = uiCursorAfter >= uiCursorBefore ? uiCursorAfter - uiCursorBefore : uiRead;
        ++uiStarvedInARow;
      }
      else
      {
        uiStarvedInARow = 0;
      }

      for (ma_uint64 i = 0; i < uiRead * uiChannels; ++i)
        out.PushBack(buffer[i]);

      if (res != MA_SUCCESS || (uiRead == 0 && uiStarvedInARow == 0))
        break;
    }

    return out;
  }

  plAudioClipResourceHandle LoadBuiltClip(plStringView sName, const plAudioClipSourcePcm& source, plAudioClipCodec::Enum codec, plAudioClipLoadMode::Enum loadMode)
  {
    plAudioClipBuildSettings settings;
    settings.m_eCodec = codec;
    settings.m_eLoadMode = loadMode;

    plStringBuilder sPath;
    sPath.SetFormat(":streamtest/{}.plBinAudioClip", sName);

    if (WriteClipFile(sPath, BuildClip(source, settings)).Failed())
      return {};

    return plResourceManager::LoadResource<plAudioClipResource>(sPath);
  }

  /// Mono cosine; with the loop used below, the loop holds a whole number of periods.
  plAudioClipSourcePcm MakeCosine(plUInt32 uiFrames, double fFrequency, float fAmplitude)
  {
    plAudioClipSourcePcm source;
    source.m_uiChannels = 1;
    source.m_uiSampleRate = 48000;
    source.m_Samples.SetCountUninitialized(uiFrames);

    for (plUInt32 f = 0; f < uiFrames; ++f)
    {
      source.m_Samples[f] = fAmplitude * static_cast<float>(std::cos(2.0 * plMath::Pi<double>() * fFrequency * f / 48000.0));
    }

    return source;
  }

  plDynamicArray<plInt16> PlayLoopedClip(plAudioStreamService* pStreams, const plAudioClipResource& clip, plUInt64 uiPlayFrames, plInt32* out_pStreamUnderruns);

  /// Plays a streamed looping clip through, after seeks backward and forward, and looped; everything must match the
  /// resident clip exactly.
  void CheckStreamedMatchesResident(plAudioStreamService& ref_streams, const plAudioClipResource& resident, const plAudioClipResource& streamed, bool bExpectFileSystem, plStringView sLabel)
  {
    const plUInt8 uiChannels = resident.GetChannels();

    plAudioClipVoiceSource residentVoice;
    plAudioClipVoiceSource voice;
    if (!PL_TEST_RESULT(resident.CreateVoiceSource(residentVoice, nullptr)) || !PL_TEST_RESULT(streamed.CreateVoiceSource(voice, &ref_streams)))
      return;

    PL_TEST_BOOL(voice.m_pStream->IsReadingThroughFileSystem() == bExpectFileSystem);

    const plDynamicArray<plInt16> expected = PlayVoice(nullptr, residentVoice, uiChannels);
    const plDynamicArray<plInt16> played = PlayVoice(&ref_streams, voice, uiChannels);
    PL_TEST_INT(played.GetCount(), expected.GetCount());
    PL_TEST_INT(MaxAbsDiff(played, expected), 0);

    // backward (reopens a plFileReader), forward (skips), and backward into the start priming data
    const plUInt64 seeks[] = {3 * 48000 + 17, 7 * 48000 + 321, 1000};
    for (plUInt64 uiSeek : seeks)
    {
      PL_TEST_BOOL(ma_data_source_seek_to_pcm_frame(voice.m_pSource, uiSeek) == MA_SUCCESS);
      const plDynamicArray<plInt16> tail = PlayVoice(&ref_streams, voice, uiChannels, 48000);
      PL_TEST_INT(tail.GetCount(), 48000 * uiChannels);
      PL_TEST_INT(MaxAbsDiff(tail, expected.GetArrayPtr().GetSubArray(static_cast<plUInt32>(uiSeek * uiChannels), 48000 * uiChannels)), 0);
    }

    const plInt32 iFileOpens = voice.m_pStream->GetFileOpenCount();
    if (bExpectFileSystem)
    {
      PL_TEST_BOOL(iFileOpens >= 3);
    }
    else
    {
      PL_TEST_INT(iFileOpens, 1);
    }

    voice.Uninit();

    plUInt64 uiLoopStart = 0;
    plUInt64 uiLoopEnd = 0;
    PL_TEST_BOOL(streamed.GetLoopPoints(uiLoopStart, uiLoopEnd));
    const plUInt64 uiPlayFrames = uiLoopStart + 3 * (uiLoopEnd - uiLoopStart) + 1000;

    plInt32 iStreamUnderruns = -1;
    const plDynamicArray<plInt16> loopExpected = PlayLoopedClip(nullptr, resident, uiPlayFrames, nullptr);
    const plDynamicArray<plInt16> loopPlayed = PlayLoopedClip(&ref_streams, streamed, uiPlayFrames, &iStreamUnderruns);
    PL_TEST_INT(loopExpected.GetCount(), uiPlayFrames * uiChannels);
    PL_TEST_INT(MaxAbsDiff(loopPlayed, loopExpected), 0);
    PL_TEST_INT(iStreamUnderruns, 0);

    PL_CLIP_TEST_REPORT("{}: {} file opens, {} stream underruns over 3 loops", sLabel, iFileOpens, iStreamUnderruns);
  }
} // namespace

PL_CREATE_SIMPLE_TEST(AudioClip, Streaming)
{
  if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(">pltest/", "PlasmaAudioStreamTest", "streamtest", plDataDirUsage::AllowWrites)))
    return;

  PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioStreamTest"));

  plAudioStreamService streams;
  streams.Startup();
  PL_SCOPE_EXIT(streams.Shutdown());

  // 10 s stereo: far more than the priming data and the ring of a stream
  const plAudioClipSourcePcm stereo = MakeSource(10 * 48000, 2);

  struct Case
  {
    const char* m_szName;
    plAudioClipCodec::Enum m_eCodec;
    plAudioClipLoadMode::Enum m_eResidentMode;
  };

  const Case cases[] = {
    {"Pcm16", plAudioClipCodec::Pcm16, plAudioClipLoadMode::DecompressOnLoad},
    {"Adpcm", plAudioClipCodec::ImaAdpcm, plAudioClipLoadMode::CompressedInMemory},
  };

  for (const Case& c : cases)
  {
    plStringBuilder sBlock;
    sBlock.SetFormat("Streamed {} renders like resident", c.m_szName);

    PL_TEST_BLOCK(plTestBlock::Enabled, sBlock.GetData())
    {
      plStringBuilder sName;
      sName.SetFormat("{}Resident", c.m_szName);
      plAudioClipResourceHandle hResident = LoadBuiltClip(sName, stereo, c.m_eCodec, c.m_eResidentMode);
      sName.SetFormat("{}Streamed", c.m_szName);
      plAudioClipResourceHandle hStreamed = LoadBuiltClip(sName, stereo, c.m_eCodec, plAudioClipLoadMode::Streamed);

      plResourceLock<plAudioClipResource> pResident(hResident, plResourceAcquireMode::BlockTillLoaded);
      plResourceLock<plAudioClipResource> pStreamed(hStreamed, plResourceAcquireMode::BlockTillLoaded);
      if (!PL_TEST_BOOL(pResident.GetAcquireResult() == plResourceAcquireResult::Final && pStreamed.GetAcquireResult() == plResourceAcquireResult::Final))
        continue;

      PL_TEST_BOOL(pStreamed->GetLoadMode() == plAudioClipLoadMode::Streamed);

      plAudioClipVoiceSource resident;
      if (!PL_TEST_RESULT(pResident->CreateVoiceSource(resident, nullptr)))
        continue;

      const plDynamicArray<plInt16> expected = PlayVoice(nullptr, resident, 2);
      PL_TEST_INT(expected.GetCount(), 10 * 48000 * 2);

      plAudioClipVoiceSource voice;
      if (!PL_TEST_RESULT(pStreamed->CreateVoiceSource(voice, &streams)))
        continue;

      PL_TEST_BOOL(voice.m_pStream != nullptr);
      PL_TEST_INT(streams.GetActiveStreamCount(), 1);

      const plDynamicArray<plInt16> played = PlayVoice(&streams, voice, 2);
      PL_TEST_INT(played.GetCount(), expected.GetCount());
      PL_TEST_INT(MaxAbsDiff(played, expected), 0);
      PL_TEST_INT(voice.m_pStream->GetUnderruns(), 0);
      PL_TEST_INT(GetDecoderUnderruns(voice), 0);

      // seeking forward and backward outside the priming data refills the ring from the new position
      const plUInt64 seeks[] = {7 * 48000 + 321, 3 * 48000 + 17};
      for (plUInt64 uiSeek : seeks)
      {
        PL_TEST_BOOL(ma_data_source_seek_to_pcm_frame(voice.m_pSource, uiSeek) == MA_SUCCESS);
        const plDynamicArray<plInt16> tail = PlayVoice(&streams, voice, 2, 48000);
        PL_TEST_INT(tail.GetCount(), 48000 * 2);
        PL_TEST_INT(MaxAbsDiff(tail, expected.GetArrayPtr().GetSubArray(static_cast<plUInt32>(uiSeek * 2), 48000 * 2)), 0);
      }

      // the initial fill and the two seeks; each seek underruns once, on the read that requests it
      PL_TEST_INT(voice.m_pStream->GetSeekCount(), 3);
      PL_TEST_INT(voice.m_pStream->GetUnderruns(), 2);
      PL_CLIP_TEST_REPORT("Streamed {}: {} seeks, {} underruns", c.m_szName, voice.m_pStream->GetSeekCount(), voice.m_pStream->GetUnderruns());

      voice.Uninit();
      PL_TEST_INT(streams.GetActiveStreamCount(), 0);
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Streamed loop")
  {
    // 4 s mono cosine at 1.6 Hz, looping [1.25 s, 3.125 s): the loop holds exactly 3 periods
    const plUInt64 uiLoopStart = 60000;
    const plUInt64 uiLoopEnd = 150000;
    const plUInt64 uiLoopFrames = uiLoopEnd - uiLoopStart;
    plAudioClipSourcePcm cosine = MakeCosine(4 * 48000, 1.6, 0.5f);
    cosine.m_bHasLoop = true;
    cosine.m_uiLoopStart = uiLoopStart;
    cosine.m_uiLoopEnd = uiLoopEnd;

    // 10 loops and a bit
    const plUInt64 uiFrames = uiLoopStart + 10 * uiLoopFrames + 1000;

    // plays 10 loops and a bit
    auto PlayLooped = [&](plAudioStreamService* pStreams, plAudioClipResource* pClip) -> plDynamicArray<plInt16>
    {
      plUInt64 uiStart = 0;
      plUInt64 uiEnd = 0;
      PL_TEST_BOOL(pClip->GetLoopPoints(uiStart, uiEnd));
      const plUInt64 uiPlayFrames = uiStart + 10 * (uiEnd - uiStart) + 1000;

      plAudioClipVoiceSource voice;
      if (pClip->CreateVoiceSource(voice, pStreams).Failed())
        return {};

      ma_data_source_set_loop_point_in_pcm_frames(voice.m_pSource, uiStart, uiEnd);
      ma_data_source_set_looping(voice.m_pSource, MA_TRUE);

      plDynamicArray<plInt16> played = PlayVoice(pStreams, voice, 1, uiPlayFrames);

      if (voice.m_pStream != nullptr)
      {
        // the initial fill plus one refill from the end of the loop priming data per loop jump
        PL_TEST_INT(voice.m_pStream->GetUnderruns(), 0);
        PL_TEST_INT(voice.m_pStream->GetSeekCount(), 11);
        PL_CLIP_TEST_REPORT("Streamed loop ({}): {} seeks", pClip->GetCodec() == plAudioClipCodec::Pcm16 ? "PCM16" : "ADPCM", voice.m_pStream->GetSeekCount());
      }

      return played;
    };

    // PCM16: exact samples and a smooth seam
    {
      plAudioClipResourceHandle hClip = LoadBuiltClip("LoopPcm16", cosine, plAudioClipCodec::Pcm16, plAudioClipLoadMode::Streamed);
      plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
      if (PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      {
        PL_TEST_INT(pClip->GetLoopPriming().GetCount(), plAudioClipConstants::PrimingBytes);

        const plDynamicArray<plInt16> played = PlayLooped(&streams, pClip.GetPointerNonConst());
        const plDynamicArray<plInt16> source = ToPcm16(cosine.m_Samples);

        if (PL_TEST_INT(played.GetCount(), uiFrames))
        {
          plInt32 iMismatches = 0;
          float fMaxStep = 0.0f;

          for (plUInt64 i = 0; i < uiFrames; ++i)
          {
            const plUInt64 uiSourceFrame = i < uiLoopEnd ? i : uiLoopStart + (i - uiLoopStart) % uiLoopFrames;
            if (played[static_cast<plUInt32>(i)] != source[static_cast<plUInt32>(uiSourceFrame)])
              ++iMismatches;

            if (i > 0)
              fMaxStep = plMath::Max(fMaxStep, plMath::Abs(played[static_cast<plUInt32>(i)] - played[static_cast<plUInt32>(i - 1)]) / 32767.0f);
          }

          PL_TEST_INT(iMismatches, 0);
          PL_TEST_BOOL(fMaxStep < 1e-3f);
          PL_CLIP_TEST_REPORT("Streamed loop PCM16: max step between frames {}", fMaxStep);
        }
      }
    }

    // ADPCM: the loop priming data starts at the block of the loop start. ADPCM is 4:1, so the clip is longer: 20 s
    // with an 8 s loop, far more than the priming data and the ring.
    {
      plAudioClipSourcePcm adpcmCosine = MakeCosine(20 * 48000, 1.5, 0.5f);
      adpcmCosine.m_bHasLoop = true;
      adpcmCosine.m_uiLoopStart = uiLoopStart;
      adpcmCosine.m_uiLoopEnd = uiLoopStart + 8 * 48000;

      plAudioClipResourceHandle hResident = LoadBuiltClip("LoopAdpcmResident", adpcmCosine, plAudioClipCodec::ImaAdpcm, plAudioClipLoadMode::CompressedInMemory);
      plAudioClipResourceHandle hStreamed = LoadBuiltClip("LoopAdpcmStreamed", adpcmCosine, plAudioClipCodec::ImaAdpcm, plAudioClipLoadMode::Streamed);
      plResourceLock<plAudioClipResource> pResident(hResident, plResourceAcquireMode::BlockTillLoaded);
      plResourceLock<plAudioClipResource> pStreamed(hStreamed, plResourceAcquireMode::BlockTillLoaded);

      if (PL_TEST_BOOL(pResident.GetAcquireResult() == plResourceAcquireResult::Final && pStreamed.GetAcquireResult() == plResourceAcquireResult::Final))
      {
        PL_TEST_INT(pStreamed->GetLoopPrimingOffset(), pStreamed->GetHeader().GetPayloadOffsetForFrame(uiLoopStart));

        const plDynamicArray<plInt16> expected = PlayLooped(nullptr, pResident.GetPointerNonConst());
        const plDynamicArray<plInt16> played = PlayLooped(&streams, pStreamed.GetPointerNonConst());
        PL_TEST_INT(expected.GetCount(), uiLoopStart + 10 * 8 * 48000 + 1000);
        PL_TEST_INT(MaxAbsDiff(played, expected), 0);
      }
    }

    // ADPCM with 6 channels: the 0.95 s of payload between the seek entry before the loop start and the loop start
    // are more than the loop priming data holds, so the loop jumps only play without underruns because the loop
    // priming data starts at the block of the loop start
    {
      plAudioClipSourcePcm hiRes;
      hiRes.m_uiChannels = 6;
      hiRes.m_uiSampleRate = 48000;
      hiRes.m_Samples = MakeSine(8 * 48000, 6, 48000, 300.0, 0.5f);
      hiRes.m_bHasLoop = true;
      hiRes.m_uiLoopStart = 48000 + 45600; // 1.95 s
      hiRes.m_uiLoopEnd = 5 * 48000;

      plAudioClipResourceHandle hResident = LoadBuiltClip("LoopAdpcm6chResident", hiRes, plAudioClipCodec::ImaAdpcm, plAudioClipLoadMode::CompressedInMemory);
      plAudioClipResourceHandle hStreamed = LoadBuiltClip("LoopAdpcm6chStreamed", hiRes, plAudioClipCodec::ImaAdpcm, plAudioClipLoadMode::Streamed);
      plResourceLock<plAudioClipResource> pResident(hResident, plResourceAcquireMode::BlockTillLoaded);
      plResourceLock<plAudioClipResource> pStreamed(hStreamed, plResourceAcquireMode::BlockTillLoaded);

      if (PL_TEST_BOOL(pResident.GetAcquireResult() == plResourceAcquireResult::Final && pStreamed.GetAcquireResult() == plResourceAcquireResult::Final))
      {
        const plAudioClipHeader& header = pStreamed->GetHeader();
        PL_TEST_INT(pStreamed->GetLoopPrimingOffset(), header.GetPayloadOffsetForFrame(hiRes.m_uiLoopStart));

        // the seek entry before the loop start (1 s) is further away than the loop priming data reaches
        PL_TEST_BOOL(header.GetPayloadOffsetForFrame(hiRes.m_uiLoopStart) - header.m_SeekTable[1].m_uiByteOffset > plAudioClipConstants::PrimingBytes);

        const plUInt64 uiPlayFrames = hiRes.m_uiLoopStart + 4 * (hiRes.m_uiLoopEnd - hiRes.m_uiLoopStart) + 1000;
        const plDynamicArray<plInt16> expected = PlayLoopedClip(nullptr, *pResident.GetPointer(), uiPlayFrames, nullptr);

        plInt32 iStreamUnderruns = -1;
        const plDynamicArray<plInt16> played = PlayLoopedClip(&streams, *pStreamed.GetPointer(), uiPlayFrames, &iStreamUnderruns);

        PL_TEST_INT(expected.GetCount(), uiPlayFrames * 6);
        PL_TEST_INT(played.GetCount(), expected.GetCount());
        PL_TEST_INT(MaxAbsDiff(played, expected), 0);
        PL_TEST_INT(iStreamUnderruns, 0);
        PL_CLIP_TEST_REPORT("Streamed loop ADPCM 6 ch: loop priming at {} (seek entry {}), {} stream underruns over 4 loops", pStreamed->GetLoopPrimingOffset(), header.m_SeekTable[1].m_uiByteOffset, iStreamUnderruns);
      }
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Starvation")
  {
    plAudioClipResourceHandle hClip = LoadBuiltClip("Starve", stereo, plAudioClipCodec::Pcm16, plAudioClipLoadMode::Streamed);
    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    const plDynamicArray<plInt16> expected = ToPcm16(stereo.m_Samples);

    plAudioClipVoiceSource voice;
    if (!PL_TEST_RESULT(pClip->CreateVoiceSource(voice, &streams)))
      return;

    // 1 s with the IO thread keeping up: the priming data is used up, the ring is in use
    plDynamicArray<plInt16> played = PlayVoice(&streams, voice, 2, 48000);

    // no IO: once the ring is drained, reads give silence and keep the cursor
    streams.SetIoPaused(true);

    const plInt32 iStreamUnderrunsBefore = voice.m_pStream->GetUnderruns();
    plUInt32 uiStarvedReads = 0;
    bool bSilent = true;
    bool bCursorKept = true;

    for (plUInt32 uiBlock = 0; uiBlock < 500 && uiStarvedReads < 20; ++uiBlock)
    {
      plInt16 buffer[1000 * 2];
      ma_uint64 uiCursorBefore = 0;
      ma_data_source_get_cursor_in_pcm_frames(voice.m_pSource, &uiCursorBefore);
      const plInt32 iUnderruns = GetDecoderUnderruns(voice);

      ma_uint64 uiRead = 0;
      ma_data_source_read_pcm_frames(voice.m_pSource, buffer, 1000, &uiRead);

      ma_uint64 uiCursorAfter = 0;
      ma_data_source_get_cursor_in_pcm_frames(voice.m_pSource, &uiCursorAfter);

      if (GetDecoderUnderruns(voice) != iUnderruns)
      {
        ++uiStarvedReads;
        bCursorKept = bCursorKept && uiCursorAfter == uiCursorBefore;

        for (plUInt32 i = 0; i < 1000 * 2; ++i)
          bSilent = bSilent && buffer[i] == 0;
      }
      else
      {
        for (ma_uint64 i = 0; i < uiRead * 2; ++i)
          played.PushBack(buffer[i]);
      }
    }

    PL_TEST_BOOL(uiStarvedReads > 0);
    PL_TEST_BOOL(bSilent);
    PL_TEST_BOOL(bCursorKept);
    PL_TEST_BOOL(voice.m_pStream->GetUnderruns() > iStreamUnderrunsBefore);
    PL_CLIP_TEST_REPORT("Starvation: silent after {} frames, {} stream underruns", played.GetCount() / 2, voice.m_pStream->GetUnderruns());

    // IO again: playback continues at the same frame
    streams.SetIoPaused(false);
    const plDynamicArray<plInt16> rest = PlayVoice(&streams, voice, 2);
    played.PushBackRange(rest);

    PL_TEST_INT(played.GetCount(), expected.GetCount());
    PL_TEST_INT(MaxAbsDiff(played, expected), 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Stream limit")
  {
    plCVarInt* pMaxStreams = static_cast<plCVarInt*>(plCVar::FindCVarByName("PlasmaAudio.MaxStreams"));
    if (!PL_TEST_BOOL(pMaxStreams != nullptr))
      return;

    const int iOldMax = pMaxStreams->GetValue();
    *pMaxStreams = 2;
    PL_SCOPE_EXIT(*pMaxStreams = iOldMax);

    plAudioClipResourceHandle hClip = LoadBuiltClip("Limit", MakeSource(4 * 48000, 1), plAudioClipCodec::ImaAdpcm, plAudioClipLoadMode::Streamed);
    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    plAudioClipVoiceSource voices[3];
    PL_TEST_RESULT(pClip->CreateVoiceSource(voices[0], &streams));
    PL_TEST_RESULT(pClip->CreateVoiceSource(voices[1], &streams));

    {
      plTestLogInterface log;
      plTestLogSystemScope logScope(&log);
      log.ExpectMessage("PlasmaAudio.MaxStreams", plLogMsgType::WarningMsg);

      PL_TEST_BOOL(pClip->CreateVoiceSource(voices[2], &streams).Failed());
      PL_TEST_BOOL(!voices[2].IsValid());
    }

    PL_TEST_INT(streams.GetActiveStreamCount(), 2);

    voices[0].Uninit();
    PL_TEST_INT(streams.GetActiveStreamCount(), 1);
    PL_TEST_RESULT(pClip->CreateVoiceSource(voices[2], &streams));
    PL_TEST_INT(streams.GetActiveStreamCount(), 2);

    for (plAudioClipVoiceSource& voice : voices)
      voice.Uninit();

    PL_TEST_INT(streams.GetActiveStreamCount(), 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Streamed through the file system")
  {
    // 10 s stereo PCM16 looping [2 s, 8 s): the payload is far larger than the priming data and the ring
    plAudioClipSourcePcm source = MakeSource(10 * 48000, 2);
    source.m_bHasLoop = true;
    source.m_uiLoopStart = 2 * 48000 + 11;
    source.m_uiLoopEnd = 8 * 48000;

    plAudioClipResourceHandle hResident = LoadBuiltClip("FsResident", source, plAudioClipCodec::Pcm16, plAudioClipLoadMode::DecompressOnLoad);
    plAudioClipResourceHandle hStreamed = LoadBuiltClip("FsStreamed", source, plAudioClipCodec::Pcm16, plAudioClipLoadMode::Streamed);
    plResourceLock<plAudioClipResource> pResident(hResident, plResourceAcquireMode::BlockTillLoaded);
    plResourceLock<plAudioClipResource> pStreamed(hStreamed, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pResident.GetAcquireResult() == plResourceAcquireResult::Final && pStreamed.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    // a plain file on disk: plOSFile
    PL_TEST_BOOL(!pStreamed->GetOsFilePath().IsEmpty());
    CheckStreamedMatchesResident(streams, *pResident.GetPointer(), *pStreamed.GetPointer(), false, "Streamed with plOSFile");

    // the same file through plFileReader
    streams.SetForceFileSystemReads(true);
    CheckStreamedMatchesResident(streams, *pResident.GetPointer(), *pStreamed.GetPointer(), true, "Streamed with plFileReader (forced)");
    streams.SetForceFileSystemReads(false);

    // inside a package archive, stored uncompressed and zstd compressed: the loader finds no file on disk
    plStringBuilder sSourceFile;
    if (!PL_TEST_RESULT(plFileSystem::ResolvePath(":streamtest/FsStreamed.plBinAudioClip", &sSourceFile, nullptr)))
      return;

    struct ArchiveCase
    {
      const char* m_szName;
      plArchiveCompressionMode m_eMode;
    };

    const ArchiveCase archiveCases[] = {{"Uncompressed", plArchiveCompressionMode::Uncompressed}, {"Zstd", plArchiveCompressionMode::Compressed_zstd}};

    for (const ArchiveCase& c : archiveCases)
    {
      plStringBuilder sArchiveFile;
      sArchiveFile.SetFormat(":streamtest/PlasmaAudioStream{}.plArchive", c.m_szName);

      plStringBuilder sEntry;
      sEntry.SetFormat("Archived{}.plBinAudioClip", c.m_szName);

      {
        plArchiveBuilder builder;
        plArchiveBuilder::SourceEntry& entry = builder.m_Entries.ExpandAndGetRef();
        entry.m_sAbsSourcePath = sSourceFile;
        entry.m_sRelTargetPath = sEntry;
        entry.m_CompressionMode = c.m_eMode;
        if (!PL_TEST_RESULT(builder.WriteArchive(sArchiveFile)))
          continue;
      }

      plStringBuilder sArchive;
      if (!PL_TEST_RESULT(plFileSystem::ResolvePath(sArchiveFile, &sArchive, nullptr)))
        continue;

      if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(sArchive, "PlasmaAudioArchiveTest", "archtest", plDataDirUsage::ReadOnly)))
        continue;
      PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioArchiveTest"));

      plStringBuilder sId;
      sId.SetFormat(":archtest/{}", sEntry);

      plAudioClipResourceHandle hArchived = plResourceManager::LoadResource<plAudioClipResource>(sId);
      plResourceLock<plAudioClipResource> pArchived(hArchived, plResourceAcquireMode::BlockTillLoaded);
      if (!PL_TEST_BOOL(pArchived.GetAcquireResult() == plResourceAcquireResult::Final))
        continue;

      PL_TEST_BOOL(pArchived->GetOsFilePath().IsEmpty());

      plStringBuilder sLabel;
      sLabel.SetFormat("Streamed from a {} package archive", c.m_szName);
      CheckStreamedMatchesResident(streams, *pResident.GetPointer(), *pArchived.GetPointer(), true, sLabel);
    }
  }
}

namespace
{
  /// Deterministic signal that costs Opus many bits: two sines and white noise per channel.
  plAudioClipSourcePcm MakeRichSource(plUInt32 uiFrames, plUInt8 uiChannels)
  {
    plAudioClipSourcePcm source;
    source.m_uiChannels = uiChannels;
    source.m_uiSampleRate = 48000;
    source.m_Samples.SetCountUninitialized(uiFrames * uiChannels);

    plUInt32 uiSeed = 12345;
    for (plUInt32 f = 0; f < uiFrames; ++f)
    {
      const double fTime = static_cast<double>(f) / 48000.0;

      for (plUInt32 c = 0; c < uiChannels; ++c)
      {
        uiSeed = uiSeed * 1664525u + 1013904223u;
        const float fNoise = static_cast<float>(uiSeed >> 8) / 16777216.0f * 2.0f - 1.0f;
        const double fTone = 0.3 * std::sin(2.0 * plMath::Pi<double>() * (220.0 + 110.0 * c) * fTime) + 0.15 * std::sin(2.0 * plMath::Pi<double>() * 3150.0 * fTime + c);
        source.m_Samples[f * uiChannels + c] = static_cast<float>(fTone) + 0.15f * fNoise;
      }
    }

    return source;
  }

  /// Signal to noise ratio in dB of decoded against reference over the samples [uiBegin, uiEnd).
  double GetSnrDb(plArrayPtr<const plInt16> reference, plArrayPtr<const plInt16> decoded, plUInt32 uiBegin, plUInt32 uiEnd)
  {
    double fSignal = 0.0;
    double fNoise = 0.0;

    for (plUInt32 i = uiBegin; i < uiEnd; ++i)
    {
      const double fReference = reference[i];
      const double fDiff = static_cast<double>(decoded[i]) - fReference;
      fSignal += fReference * fReference;
      fNoise += fDiff * fDiff;
    }

    if (fNoise <= 0.0)
      return 200.0;

    return 10.0 * std::log10(fSignal / fNoise);
  }

  /// Largest difference between consecutive frames of channel 0 in the frames [uiBegin, uiEnd), in full scale.
  float GetMaxStep(plArrayPtr<const plInt16> pcm, plUInt8 uiChannels, plUInt64 uiBegin, plUInt64 uiEnd)
  {
    float fMax = 0.0f;
    for (plUInt64 f = plMath::Max<plUInt64>(uiBegin, 1); f < uiEnd; ++f)
    {
      const plInt32 iStep = static_cast<plInt32>(pcm[static_cast<plUInt32>(f * uiChannels)]) - static_cast<plInt32>(pcm[static_cast<plUInt32>((f - 1) * uiChannels)]);
      fMax = plMath::Max(fMax, plMath::Abs(iStep) / 32767.0f);
    }
    return fMax;
  }

  /// Reads up to uiFrames frames from the current position in one call.
  plDynamicArray<plInt16> ReadFrames(ma_data_source* pSource, plUInt8 uiChannels, plUInt64 uiFrames)
  {
    plDynamicArray<plInt16> out;
    out.SetCount(static_cast<plUInt32>(uiFrames * uiChannels));

    ma_uint64 uiRead = 0;
    ma_data_source_read_pcm_frames(pSource, out.GetData(), uiFrames, &uiRead);
    out.SetCount(static_cast<plUInt32>(uiRead * uiChannels));
    return out;
  }

  plAudioClipResourceHandle LoadOpusClip(plStringView sName, const plAudioClipSourcePcm& source, plAudioClipLoadMode::Enum loadMode)
  {
    plAudioClipBuildSettings settings;
    settings.m_eCodec = plAudioClipCodec::Opus;
    settings.m_eLoadMode = loadMode;
    settings.m_uiOpusBitrateKbps = 96;

    plStringBuilder sPath;
    sPath.SetFormat(":opustest/{}.plBinAudioClip", sName);

    if (WriteClipFile(sPath, BuildClip(source, settings)).Failed())
      return {};

    return plResourceManager::LoadResource<plAudioClipResource>(sPath);
  }

  /// Plays uiPlayFrames frames of a clip with its loop points active.
  plDynamicArray<plInt16> PlayLoopedClip(plAudioStreamService* pStreams, const plAudioClipResource& clip, plUInt64 uiPlayFrames, plInt32* out_pStreamUnderruns = nullptr)
  {
    plUInt64 uiStart = 0;
    plUInt64 uiEnd = 0;
    PL_TEST_BOOL(clip.GetLoopPoints(uiStart, uiEnd));

    plAudioClipVoiceSource voice;
    if (clip.CreateVoiceSource(voice, pStreams).Failed())
      return {};

    ma_data_source_set_loop_point_in_pcm_frames(voice.m_pSource, uiStart, uiEnd);
    ma_data_source_set_looping(voice.m_pSource, MA_TRUE);

    plDynamicArray<plInt16> played = PlayVoice(pStreams, voice, clip.GetChannels(), uiPlayFrames);

    if (out_pStreamUnderruns != nullptr && voice.m_pStream != nullptr)
    {
      *out_pStreamUnderruns = voice.m_pStream->GetUnderruns();
    }

    return played;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(AudioClip, Opus)
{
  if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(">pltest/", "PlasmaAudioOpusTest", "opustest", plDataDirUsage::AllowWrites)))
    return;

  PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioOpusTest"));

  plAudioStreamService streams;
  streams.Startup();
  PL_SCOPE_EXIT(streams.Shutdown());

  for (plUInt8 uiChannels = 1; uiChannels <= 2; ++uiChannels)
  {
    plStringBuilder sBlock;
    sBlock.SetFormat("{} ch round trip, seeking, DecompressOnLoad", uiChannels);

    PL_TEST_BLOCK(plTestBlock::Enabled, sBlock.GetData())
    {
      const plUInt32 uiFrames = 2 * 48000 + 123;
      const plAudioClipSourcePcm source = MakeSource(uiFrames, uiChannels, 1000.0, 0.5f);
      const plDynamicArray<plInt16> expected = ToPcm16(source.m_Samples);

      plAudioClipBuildSettings settings;
      settings.m_eCodec = plAudioClipCodec::Opus;
      settings.m_eLoadMode = plAudioClipLoadMode::CompressedInMemory;
      settings.m_uiOpusBitrateKbps = 96;

      plAudioClipResourceDescriptor desc;
      if (!PL_TEST_RESULT(ParseClip(BuildClip(source, settings), desc)))
        continue;

      const plAudioClipHeader& header = desc.m_Header;
      PL_TEST_BOOL(header.m_eCodec == plAudioClipCodec::Opus);
      PL_TEST_INT(header.m_uiSampleRate, 48000);
      PL_TEST_INT(header.m_uiFrames, uiFrames);
      PL_TEST_INT(header.m_uiBlockFrames, plOpusCodec::FramesPerPacket);
      PL_TEST_INT(header.m_uiBlockBytes, 0);
      PL_TEST_BOOL(header.m_uiPreSkip > 0 && header.m_uiPreSkip < plOpusCodec::FramesPerPacket);

      // the payload is a chain of length prefixed packets with a seek entry every SeekIntervalPackets packets
      const plUInt64 uiPackets = plOpusCodec::GetPacketCount(uiFrames, header.m_uiPreSkip);
      plUInt64 uiOffset = 0;
      plUInt64 uiPacketCount = 0;
      bool bSeekTableMatches = true;

      while (uiOffset + 2 <= desc.m_Payload.GetCount())
      {
        if (uiPacketCount % plOpusCodec::SeekIntervalPackets == 0)
        {
          const plUInt32 uiEntry = static_cast<plUInt32>(uiPacketCount / plOpusCodec::SeekIntervalPackets);
          bSeekTableMatches = bSeekTableMatches && uiEntry < header.m_SeekTable.GetCount() && header.m_SeekTable[uiEntry].m_uiByteOffset == uiOffset &&
                              header.m_SeekTable[uiEntry].m_uiFrame == uiPacketCount * plOpusCodec::FramesPerPacket;
        }

        uiOffset += 2 + (static_cast<plUInt32>(desc.m_Payload[static_cast<plUInt32>(uiOffset)]) | (static_cast<plUInt32>(desc.m_Payload[static_cast<plUInt32>(uiOffset + 1)]) << 8));
        ++uiPacketCount;
      }

      PL_TEST_INT(uiOffset, desc.m_Payload.GetCount());
      PL_TEST_INT(uiPacketCount, uiPackets);
      PL_TEST_INT(header.m_SeekTable.GetCount(), (uiPackets + plOpusCodec::SeekIntervalPackets - 1) / plOpusCodec::SeekIntervalPackets);
      PL_TEST_BOOL(bSeekTableMatches);

      // a version 1 header (without the pre-skip field) still reads
      {
        const plDynamicArray<plUInt8> headerBytes = WriteLegacyHeader(header, 1);

        plAudioClipHeader oldHeader;
        plRawMemoryStreamReader reader(headerBytes);
        if (PL_TEST_RESULT(oldHeader.Read(reader)))
        {
          PL_TEST_INT(oldHeader.m_uiPreSkip, 0);
          PL_TEST_INT(oldHeader.m_uiFrames, uiFrames);
          PL_TEST_INT(oldHeader.m_SeekTable.GetCount(), header.m_SeekTable.GetCount());
          PL_TEST_INT(oldHeader.m_Languages[0].m_uiPayloadBytes, header.m_Languages[0].m_uiPayloadBytes);
          PL_TEST_INT(reader.GetReadPosition(), headerBytes.GetCount());
        }
      }

      plAudioClipResourceDescriptor decompressDesc;
      decompressDesc.m_Header = desc.m_Header;
      decompressDesc.m_Header.m_eLoadMode = plAudioClipLoadMode::DecompressOnLoad;
      decompressDesc.m_Payload = desc.m_Payload;

      const double fKbps = desc.m_Payload.GetCount() * 8.0 / (uiFrames / 48000.0) / 1000.0;

      plStringBuilder sId;
      sId.SetFormat("PlasmaAudioTest:Opus{}", uiChannels);
      plAudioClipResourceHandle hClip = plResourceManager::CreateResource<plAudioClipResource>(sId, std::move(desc));

      plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
      if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
        continue;

      plAudioClipVoiceSource voice;
      if (!PL_TEST_RESULT(pClip->CreateVoiceSource(voice, nullptr)))
        continue;

      PL_TEST_BOOL(voice.m_Kind == plAudioClipVoiceSource::Kind::Opus);

      ma_format format = ma_format_unknown;
      ma_uint32 uiFormatChannels = 0;
      ma_uint32 uiRate = 0;
      ma_data_source_get_data_format(voice.m_pSource, &format, &uiFormatChannels, &uiRate, nullptr, 0);
      PL_TEST_BOOL(format == ma_format_s16);
      PL_TEST_INT(uiFormatChannels, uiChannels);
      PL_TEST_INT(uiRate, 48000);

      ma_uint64 uiLength = 0;
      ma_data_source_get_length_in_pcm_frames(voice.m_pSource, &uiLength);
      PL_TEST_INT(uiLength, uiFrames);

      const plDynamicArray<plInt16> played = ReadAll(voice.m_pSource, uiChannels);
      if (!PL_TEST_INT(played.GetCount(), expected.GetCount()))
        continue;

      // the decoder drops the pre-skip: the output is aligned with the source
      plInt32 iBestLag = 0;
      double fBestLagSnr = -1000.0;
      for (plInt32 iLag = -40; iLag <= 40; ++iLag)
      {
        double fSignal = 0.0;
        double fNoise = 0.0;
        for (plUInt32 f = 2000; f < uiFrames - 2000; ++f)
        {
          for (plUInt32 c = 0; c < uiChannels; ++c)
          {
            const double fReference = expected[f * uiChannels + c];
            const double fDiff = played[(f + iLag) * uiChannels + c] - fReference;
            fSignal += fReference * fReference;
            fNoise += fDiff * fDiff;
          }
        }

        const double fSnr = 10.0 * std::log10(fSignal / plMath::Max(fNoise, 1e-9));
        if (fSnr > fBestLagSnr)
        {
          fBestLagSnr = fSnr;
          iBestLag = iLag;
        }
      }

      const double fSnr = GetSnrDb(expected, played, 0, expected.GetCount());
      PL_CLIP_TEST_REPORT("Opus {} ch, 1 kHz sine at 96 kbps ({} kbps actual): SNR {} dB over the whole clip, best alignment lag {} (pre-skip {})", uiChannels, fKbps, fSnr, iBestLag, header.m_uiPreSkip);
      PL_TEST_INT(iBestLag, 0);
      PL_TEST_BOOL(fSnr > 20.0);

      // seeking decodes the pre-roll and continues like the decode-through; within the first packets it is exact
      const plUInt64 seeks[] = {17, 3 * plOpusCodec::FramesPerPacket + 5, 48000 + 321, 70000, 12345, uiFrames - 500};
      double fWorstSeekSnr = 200.0;
      plInt32 iWorstSeekDiff = 0;

      for (plUInt64 uiSeek : seeks)
      {
        PL_TEST_BOOL(ma_data_source_seek_to_pcm_frame(voice.m_pSource, uiSeek) == MA_SUCCESS);

        const plUInt64 uiCount = plMath::Min<plUInt64>(4800, uiFrames - uiSeek);
        const plDynamicArray<plInt16> tail = ReadFrames(voice.m_pSource, uiChannels, uiCount);
        if (!PL_TEST_INT(tail.GetCount(), uiCount * uiChannels))
          continue;

        const plArrayPtr<const plInt16> reference = played.GetArrayPtr().GetSubArray(static_cast<plUInt32>(uiSeek * uiChannels), static_cast<plUInt32>(uiCount * uiChannels));
        const plInt32 iDiff = MaxAbsDiff(tail, reference);

        if (plOpusCodec::GetDecodeStartPacket(uiSeek, header.m_uiPreSkip) == 0)
        {
          PL_TEST_INT(iDiff, 0);
        }

        fWorstSeekSnr = plMath::Min(fWorstSeekSnr, GetSnrDb(reference, tail, 0, tail.GetCount()));        iWorstSeekDiff = plMath::Max(iWorstSeekDiff, iDiff);
      }

      PL_CLIP_TEST_REPORT("Opus {} ch seek vs decode-through: worst SNR {} dB, worst max abs diff {}", uiChannels, fWorstSeekSnr, iWorstSeekDiff);
      PL_TEST_BOOL(fWorstSeekSnr > 40.0);

      voice.Uninit();

      // DecompressOnLoad decodes the same samples at load time
      sId.SetFormat("PlasmaAudioTest:OpusDecompress{}", uiChannels);
      plAudioClipResourceHandle hDecoded = plResourceManager::CreateResource<plAudioClipResource>(sId, std::move(decompressDesc));

      plResourceLock<plAudioClipResource> pDecoded(hDecoded, plResourceAcquireMode::BlockTillLoaded);
      if (PL_TEST_BOOL(pDecoded.GetAcquireResult() == plResourceAcquireResult::Final))
      {
        PL_TEST_INT(MaxAbsDiff(pDecoded->GetPcm(), played), 0);

        plAudioClipVoiceSource decodedVoice;
        if (PL_TEST_RESULT(pDecoded->CreateVoiceSource(decodedVoice, nullptr)))
        {
          PL_TEST_BOOL(decodedVoice.m_Kind == plAudioClipVoiceSource::Kind::PcmBuffer);
          PL_TEST_INT(MaxAbsDiff(ReadAll(decodedVoice.m_pSource, uiChannels), played), 0);
        }
      }
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Seek from silence converges within the pre-roll")
  {
    // 1 s silence, then 1 s of a 1 kHz sine: a seek from the silent part into the tone starts the pre-roll with the
    // band energies of silence, the worst case for the energy prediction of the decoder
    plAudioClipSourcePcm source = MakeSource(2 * 48000, 1, 1000.0, 0.5f);
    plMemoryUtils::ZeroFill(source.m_Samples.GetData(), 48000);

    plAudioClipBuildSettings settings;
    settings.m_eCodec = plAudioClipCodec::Opus;
    settings.m_eLoadMode = plAudioClipLoadMode::CompressedInMemory;

    plAudioClipResourceDescriptor desc;
    if (!PL_TEST_RESULT(ParseClip(BuildClip(source, settings), desc)))
      return;

    plAudioClipResourceHandle hClip = plResourceManager::CreateResource<plAudioClipResource>("PlasmaAudioTest:OpusSilenceSeek", std::move(desc));
    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);

    plAudioClipVoiceSource voice;
    if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final) || !PL_TEST_RESULT(pClip->CreateVoiceSource(voice, nullptr)))
      return;

    const plDynamicArray<plInt16> through = ReadAll(voice.m_pSource, 1);

    PL_TEST_BOOL(ma_data_source_seek_to_pcm_frame(voice.m_pSource, 12000) == MA_SUCCESS);
    ReadFrames(voice.m_pSource, 1, 960);

    const plUInt64 uiSeek = 72000;
    PL_TEST_BOOL(ma_data_source_seek_to_pcm_frame(voice.m_pSource, uiSeek) == MA_SUCCESS);
    const plDynamicArray<plInt16> tail = ReadFrames(voice.m_pSource, 1, 4800);
    if (!PL_TEST_INT(tail.GetCount(), 4800))
      return;

    const plArrayPtr<const plInt16> reference = through.GetArrayPtr().GetSubArray(static_cast<plUInt32>(uiSeek), 4800);
    const double fFirstPacketSnr = GetSnrDb(reference, tail, 0, 960);
    const double fLastPacketSnr = GetSnrDb(reference, tail, 3840, 4800);
    PL_CLIP_TEST_REPORT("Opus seek from silence into a tone: {} dB SNR in the first 20 ms, {} dB 80 ms later", fFirstPacketSnr, fLastPacketSnr);
    // the energies converge by about 6 dB per packet
    PL_TEST_BOOL(fFirstPacketSnr > 10.0);
    PL_TEST_BOOL(fLastPacketSnr > 30.0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Streamed renders like CompressedInMemory")
  {
    // 60 s stereo: far more than the priming data and the ring of a stream
    const plAudioClipSourcePcm rich = MakeRichSource(60 * 48000, 2);

    plAudioClipResourceHandle hResident = LoadOpusClip("RichResident", rich, plAudioClipLoadMode::CompressedInMemory);
    plAudioClipResourceHandle hStreamed = LoadOpusClip("RichStreamed", rich, plAudioClipLoadMode::Streamed);

    plResourceLock<plAudioClipResource> pResident(hResident, plResourceAcquireMode::BlockTillLoaded);
    plResourceLock<plAudioClipResource> pStreamed(hStreamed, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pResident.GetAcquireResult() == plResourceAcquireResult::Final && pStreamed.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    PL_TEST_BOOL(pStreamed->GetLoadMode() == plAudioClipLoadMode::Streamed);
    PL_TEST_BOOL(pStreamed->GetPayloadBytes() > plAudioClipConstants::PrimingBytes + 4 * plAudioClipConstants::StreamChunkBytes);

    plAudioClipVoiceSource resident;
    plAudioClipVoiceSource voice;
    if (!PL_TEST_RESULT(pResident->CreateVoiceSource(resident, nullptr)) || !PL_TEST_RESULT(pStreamed->CreateVoiceSource(voice, &streams)))
      return;

    const plDynamicArray<plInt16> expected = PlayVoice(nullptr, resident, 2);
    PL_TEST_INT(expected.GetCount(), 60 * 48000 * 2);

    const plDynamicArray<plInt16> played = PlayVoice(&streams, voice, 2);
    PL_TEST_INT(played.GetCount(), expected.GetCount());
    PL_TEST_INT(MaxAbsDiff(played, expected), 0);
    PL_TEST_INT(voice.m_pStream->GetUnderruns(), 0);
    PL_TEST_INT(GetDecoderUnderruns(voice), 0);

    // seeks outside the priming data: the stream refills from the seek entry the decoder walks from
    const plUInt64 seeks[] = {40 * 48000 + 321, 10 * 48000 + 17};
    for (plUInt64 uiSeek : seeks)
    {
      PL_TEST_BOOL(ma_data_source_seek_to_pcm_frame(resident.m_pSource, uiSeek) == MA_SUCCESS);
      PL_TEST_BOOL(ma_data_source_seek_to_pcm_frame(voice.m_pSource, uiSeek) == MA_SUCCESS);

      const plDynamicArray<plInt16> reference = PlayVoice(nullptr, resident, 2, 48000);
      const plDynamicArray<plInt16> tail = PlayVoice(&streams, voice, 2, 48000);
      PL_TEST_INT(tail.GetCount(), 48000 * 2);
      PL_TEST_INT(MaxAbsDiff(tail, reference), 0);
    }

    PL_CLIP_TEST_REPORT("Streamed Opus: {} payload bytes, {} seeks, {} stream underruns, {} decoder underruns", pStreamed->GetPayloadBytes(), voice.m_pStream->GetSeekCount(), voice.m_pStream->GetUnderruns(), GetDecoderUnderruns(voice));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Seamless loop")
  {
    // 4 s of a 500 Hz cosine, looping [1.25 s, 3.25 s): the loop holds exactly 1000 periods
    const plUInt64 uiLoopStart = 60000;
    const plUInt64 uiLoopEnd = 156000;
    const plUInt64 uiLoopFrames = uiLoopEnd - uiLoopStart;
    const plUInt32 uiLoops = 5;

    plAudioClipSourcePcm tone = MakeCosine(4 * 48000, 500.0, 0.5f);
    tone.m_bHasLoop = true;
    tone.m_uiLoopStart = uiLoopStart;
    tone.m_uiLoopEnd = uiLoopEnd;

    plAudioClipResourceHandle hClip = LoadOpusClip("LoopTone", tone, plAudioClipLoadMode::CompressedInMemory);
    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    plDynamicArray<plInt16> through;
    {
      plAudioClipVoiceSource voice;
      if (!PL_TEST_RESULT(pClip->CreateVoiceSource(voice, nullptr)))
        return;
      through = ReadAll(voice.m_pSource, 1);
    }

    const plUInt64 uiPlayFrames = uiLoopStart + uiLoops * uiLoopFrames + 4800;
    const plDynamicArray<plInt16> played = PlayLoopedClip(nullptr, *pClip.GetPointer(), uiPlayFrames);
    if (!PL_TEST_INT(played.GetCount(), uiPlayFrames))
      return;

    // steps between frames: over the first pass (no seam) and around every seam
    const float fRegularStep = GetMaxStep(played, 1, 1000, uiLoopEnd - 1000);
    float fSeamStep = 0.0f;
    double fWorstSeamSnr = 200.0;

    for (plUInt32 uiLoop = 0; uiLoop < uiLoops; ++uiLoop)
    {
      const plUInt64 uiSeam = uiLoopEnd + uiLoop * uiLoopFrames;
      fSeamStep = plMath::Max(fSeamStep, GetMaxStep(played, 1, uiSeam - plOpusCodec::FramesPerPacket, uiSeam + plOpusCodec::FramesPerPacket));

      // after the seam the loop plays what the decode-through plays at the loop start
      const plArrayPtr<const plInt16> reference = through.GetArrayPtr().GetSubArray(static_cast<plUInt32>(uiLoopStart), 4800);
      const plArrayPtr<const plInt16> afterSeam = played.GetArrayPtr().GetSubArray(static_cast<plUInt32>(uiSeam), 4800);
      fWorstSeamSnr = plMath::Min(fWorstSeamSnr, GetSnrDb(reference, afterSeam, 0, 4800));    }

    PL_CLIP_TEST_REPORT("Opus loop: max step {} at the seams, {} inside the first pass; after the seam {} dB SNR against the decode-through", fSeamStep, fRegularStep, fWorstSeamSnr);
    PL_TEST_BOOL(fSeamStep <= 1.25f * fRegularStep);
    PL_TEST_BOOL(fWorstSeamSnr > 40.0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Streamed loop")
  {
    // 45 s mono looping [10 s, 40 s): the loop start is far outside the start priming data
    const plUInt64 uiLoopStart = 10 * 48000 + 17;
    const plUInt64 uiLoopEnd = 40 * 48000 + 5;

    plAudioClipSourcePcm rich = MakeRichSource(45 * 48000, 1);
    rich.m_bHasLoop = true;
    rich.m_uiLoopStart = uiLoopStart;
    rich.m_uiLoopEnd = uiLoopEnd;

    plAudioClipResourceHandle hResident = LoadOpusClip("LoopRichResident", rich, plAudioClipLoadMode::CompressedInMemory);
    plAudioClipResourceHandle hStreamed = LoadOpusClip("LoopRichStreamed", rich, plAudioClipLoadMode::Streamed);

    plResourceLock<plAudioClipResource> pResident(hResident, plResourceAcquireMode::BlockTillLoaded);
    plResourceLock<plAudioClipResource> pStreamed(hStreamed, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pResident.GetAcquireResult() == plResourceAcquireResult::Final && pStreamed.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    const plAudioClipHeader& header = pStreamed->GetHeader();
    PL_TEST_BOOL(pStreamed->GetPayloadBytes() > plAudioClipConstants::PrimingBytes + 4 * plAudioClipConstants::StreamChunkBytes);

    // the loop priming data starts at the seek entry the decoder walks from for the pre-roll of the loop start
    const plUInt64 uiPreRollPacket = plOpusCodec::GetDecodeStartPacket(uiLoopStart, header.m_uiPreSkip);
    PL_TEST_BOOL(uiPreRollPacket + plOpusCodec::PreRollPackets == plOpusCodec::GetPacketOfFrame(uiLoopStart, header.m_uiPreSkip));
    PL_TEST_INT(pStreamed->GetLoopPrimingOffset(), header.GetPayloadOffsetForFrame(uiLoopStart));
    PL_TEST_BOOL(header.GetDecodeStartFrame(uiLoopStart) <= uiPreRollPacket * plOpusCodec::FramesPerPacket);
    PL_TEST_INT(pStreamed->GetLoopPriming().GetCount(), plAudioClipConstants::PrimingBytes);

    const plUInt64 uiPlayFrames = uiLoopStart + 2 * (uiLoopEnd - uiLoopStart) + 1000;
    const plDynamicArray<plInt16> expected = PlayLoopedClip(nullptr, *pResident.GetPointer(), uiPlayFrames);

    plInt32 iStreamUnderruns = -1;
    const plDynamicArray<plInt16> played = PlayLoopedClip(&streams, *pStreamed.GetPointer(), uiPlayFrames, &iStreamUnderruns);

    PL_TEST_INT(expected.GetCount(), uiPlayFrames);
    PL_TEST_INT(played.GetCount(), uiPlayFrames);
    PL_TEST_INT(MaxAbsDiff(played, expected), 0);
    PL_TEST_INT(iStreamUnderruns, 0);
    PL_CLIP_TEST_REPORT("Streamed Opus loop: {} payload bytes, loop priming at {}, {} stream underruns", pStreamed->GetPayloadBytes(), pStreamed->GetLoopPrimingOffset(), iStreamUnderruns);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Two languages")
  {
    PL_SCOPE_EXIT(plAudioClipResourceLoader::SetLanguage(""));

    // 20 s mono looping [12 s, 18 s); the default language is silent for its first 10 s and needs far fewer bits
    // there, so the packet offsets, seek tables and loop priming offsets of the languages differ
    plAudioClipSourcePcm english = MakeSource(20 * 48000, 1, 440.0, 0.5f);
    plMemoryUtils::ZeroFill(english.m_Samples.GetData(), 10 * 48000);
    plAudioClipSourcePcm german = MakeRichSource(20 * 48000, 1);

    for (plAudioClipSourcePcm* pSource : {&english, &german})
    {
      pSource->m_bHasLoop = true;
      pSource->m_uiLoopStart = 12 * 48000 + 17;
      pSource->m_uiLoopEnd = 18 * 48000;
    }

    const plAudioClipSourcePcm* sources[] = {&english, &german};

    // every language decodes like a single-language clip of its source
    plAudioClipResourceHandle hEnglish = LoadOpusClip("TwoLanguagesRefEn", english, plAudioClipLoadMode::CompressedInMemory);
    plAudioClipResourceHandle hGerman = LoadOpusClip("TwoLanguagesRefDe", german, plAudioClipLoadMode::CompressedInMemory);

    plAudioClipResourceHandle hClips[2];
    const plAudioClipLoadMode::Enum loadModes[] = {plAudioClipLoadMode::CompressedInMemory, plAudioClipLoadMode::Streamed};

    for (plUInt32 uiMode = 0; uiMode < 2; ++uiMode)
    {
      plAudioClipBuildSettings settings;
      settings.m_eCodec = plAudioClipCodec::Opus;
      settings.m_eLoadMode = loadModes[uiMode];
      settings.m_uiOpusBitrateKbps = 96;
      settings.m_Languages.PushBack("");
      settings.m_Languages.PushBack("de");

      plDynamicArray<plUInt8> bytes;
      {
        plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&bytes);
        plMemoryStreamWriter writer(&storage);
        if (!PL_TEST_RESULT(plAudioClipBuilder::Build(plArrayPtr<const plAudioClipSourcePcm* const>(sources), settings, writer)))
          return;
      }

      plStringBuilder sPath;
      sPath.SetFormat(":opustest/TwoLanguages{}.plBinAudioClip", uiMode);
      if (!PL_TEST_RESULT(WriteClipFile(sPath, bytes)))
        return;

      hClips[uiMode] = plResourceManager::LoadResource<plAudioClipResource>(sPath);
    }

    const char* languages[] = {"", "de"};
    const plAudioClipResourceHandle* references[] = {&hEnglish, &hGerman};
    plUInt64 loopPrimingOffsets[2] = {};

    for (plUInt32 uiLanguage = 0; uiLanguage < 2; ++uiLanguage)
    {
      plAudioClipResourceLoader::SetLanguage(languages[uiLanguage]);
      plResourceManager::ReloadResource(hClips[0], true);
      plResourceManager::ReloadResource(hClips[1], true);

      plResourceLock<plAudioClipResource> pReference(*references[uiLanguage], plResourceAcquireMode::BlockTillLoaded);
      plResourceLock<plAudioClipResource> pClip(hClips[0], plResourceAcquireMode::BlockTillLoaded);
      plResourceLock<plAudioClipResource> pStreamed(hClips[1], plResourceAcquireMode::BlockTillLoaded);
      if (!PL_TEST_BOOL(pReference.GetAcquireResult() == plResourceAcquireResult::Final && pClip.GetAcquireResult() == plResourceAcquireResult::Final &&
                        pStreamed.GetAcquireResult() == plResourceAcquireResult::Final))
        continue;

      PL_TEST_STRING(pClip->GetActiveLanguage(), languages[uiLanguage]);
      PL_TEST_STRING(pStreamed->GetActiveLanguage(), languages[uiLanguage]);
      PL_TEST_INT(pClip->GetHeader().m_uiPreSkip, pReference->GetHeader().m_uiPreSkip);
      PL_TEST_INT(pClip->GetHeader().m_SeekTable.GetCount(), pReference->GetHeader().m_SeekTable.GetCount());
      PL_TEST_INT(pClip->GetPayload().GetCount(), pReference->GetPayload().GetCount());
      PL_TEST_INT(pStreamed->GetLoopPrimingOffset(), pReference->GetLoopPrimingOffset());
      loopPrimingOffsets[uiLanguage] = pStreamed->GetLoopPrimingOffset();

      plAudioClipVoiceSource referenceVoice;
      plAudioClipVoiceSource voice;
      if (!PL_TEST_RESULT(pReference->CreateVoiceSource(referenceVoice, nullptr)) || !PL_TEST_RESULT(pClip->CreateVoiceSource(voice, nullptr)))
        continue;

      const plDynamicArray<plInt16> expected = ReadAll(referenceVoice.m_pSource, 1);
      const plDynamicArray<plInt16> played = ReadAll(voice.m_pSource, 1);
      PL_TEST_INT(played.GetCount(), 20 * 48000);
      PL_TEST_INT(MaxAbsDiff(played, expected), 0);

      // seeking walks the seek table of the language
      const plUInt64 uiSeek = 9 * 48000 + 321;
      PL_TEST_BOOL(ma_data_source_seek_to_pcm_frame(referenceVoice.m_pSource, uiSeek) == MA_SUCCESS);
      PL_TEST_BOOL(ma_data_source_seek_to_pcm_frame(voice.m_pSource, uiSeek) == MA_SUCCESS);
      const plDynamicArray<plInt16> referenceTail = ReadFrames(referenceVoice.m_pSource, 1, 4800);
      const plDynamicArray<plInt16> tail = ReadFrames(voice.m_pSource, 1, 4800);
      PL_TEST_INT(tail.GetCount(), 4800);
      PL_TEST_INT(MaxAbsDiff(tail, referenceTail), 0);

      // streamed and looping: the loop priming data of the language
      const plUInt64 uiPlayFrames = english.m_uiLoopStart + 2 * (english.m_uiLoopEnd - english.m_uiLoopStart) + 1000;
      plInt32 iStreamUnderruns = -1;
      const plDynamicArray<plInt16> loopExpected = PlayLoopedClip(nullptr, *pReference.GetPointer(), uiPlayFrames);
      const plDynamicArray<plInt16> loopPlayed = PlayLoopedClip(&streams, *pStreamed.GetPointer(), uiPlayFrames, &iStreamUnderruns);
      PL_TEST_INT(loopPlayed.GetCount(), uiPlayFrames);
      PL_TEST_INT(MaxAbsDiff(loopPlayed, loopExpected), 0);
      PL_TEST_INT(iStreamUnderruns, 0);

      PL_CLIP_TEST_REPORT("Opus language '{}': {} payload bytes, {} seek entries, loop priming at {}, {} stream underruns", languages[uiLanguage], pClip->GetPayload().GetCount(), pClip->GetHeader().m_SeekTable.GetCount(), pStreamed->GetLoopPrimingOffset(), iStreamUnderruns);
    }

    // the sections really have different packet offsets
    PL_TEST_BOOL(loopPrimingOffsets[0] + 10000 < loopPrimingOffsets[1]);
  }
}
