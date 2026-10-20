#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/FileSystem/FileWriter.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <PlasmaAudioPlugin/Clips/AudioClipBuilder.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResource.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResourceLoader.h>
#include <PlasmaAudioPlugin/Codecs/ImaAdpcm.h>
#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioPlugin/Streaming/AudioStreamService.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

PL_CREATE_SIMPLE_TEST_GROUP(ClipEvent);

namespace
{
  constexpr float SilenceThreshold = 1e-6f;

  void Report(plStringView sText)
  {
    plStringBuilder sTmp = sText;
    plTestFramework::Output(plTestOutput::Message, "%s", sTmp.GetData());
  }

  plDynamicArray<plInt16> ToPcm16(const plDynamicArray<float>& samples)
  {
    plDynamicArray<plInt16> pcm;
    pcm.SetCountUninitialized(samples.GetCount());

    for (plUInt32 i = 0; i < samples.GetCount(); ++i)
    {
      const float fScaled = plMath::Clamp(samples[i], -1.0f, 1.0f) * 32767.0f;
      pcm[i] = static_cast<plInt16>(plMath::Clamp<plInt32>(static_cast<plInt32>(plMath::Round(fScaled)), -32768, 32767));
    }

    return pcm;
  }

  void AppendU32(plDataBuffer& ref_data, plUInt32 uiValue)
  {
    for (plUInt32 i = 0; i < 4; ++i)
      ref_data.PushBack(static_cast<plUInt8>((uiValue >> (8 * i)) & 0xFF));
  }

  void AppendU16(plDataBuffer& ref_data, plUInt16 uiValue)
  {
    ref_data.PushBack(static_cast<plUInt8>(uiValue & 0xFF));
    ref_data.PushBack(static_cast<plUInt8>((uiValue >> 8) & 0xFF));
  }

  void AppendTag(plDataBuffer& ref_data, const char* szTag)
  {
    for (plUInt32 i = 0; i < 4; ++i)
      ref_data.PushBack(static_cast<plUInt8>(szTag[i]));
  }

  /// \brief Mono s16 PCM as an in-memory RIFF/WAVE file.
  plDataBuffer MakeWavS16(const plDynamicArray<plInt16>& pcm)
  {
    const plUInt32 uiDataBytes = pcm.GetCount() * 2;

    plDataBuffer data;
    AppendTag(data, "RIFF");
    AppendU32(data, 36 + uiDataBytes);
    AppendTag(data, "WAVE");
    AppendTag(data, "fmt ");
    AppendU32(data, 16);
    AppendU16(data, 1); // WAVE_FORMAT_PCM
    AppendU16(data, 1);
    AppendU32(data, plOfflineAudioEngine::SampleRate);
    AppendU32(data, plOfflineAudioEngine::SampleRate * 2);
    AppendU16(data, 2);
    AppendU16(data, 16);
    AppendTag(data, "data");
    AppendU32(data, uiDataBytes);

    for (plInt16 s : pcm)
      AppendU16(data, static_cast<plUInt16>(s));

    return data;
  }

  /// \brief Creates a mono 48 kHz clip resource from pcm. Returns the PCM the clip plays (the ADPCM round trip for ADPCM).
  plDynamicArray<plInt16> CreateClip(plStringView sId, const plDynamicArray<plInt16>& pcm, plAudioClipCodec::Enum eCodec, plAudioClipLoadMode::Enum eLoadMode)
  {
    const plUInt64 uiFrames = pcm.GetCount();

    plAudioClipResourceDescriptor desc;
    desc.m_Header.m_eCodec = eCodec;
    desc.m_Header.m_eLoadMode = eLoadMode;
    desc.m_Header.m_uiChannels = 1;
    desc.m_Header.m_uiSampleRate = plOfflineAudioEngine::SampleRate;
    desc.m_Header.m_uiFrames = uiFrames;

    plDynamicArray<plInt16> played = pcm;

    if (eCodec == plAudioClipCodec::ImaAdpcm)
    {
      const plUInt32 uiBlockBytes = plImaAdpcmCodec::GetBlockBytes(1);
      const plUInt32 uiBlocks = static_cast<plUInt32>(plImaAdpcmCodec::GetBlockCount(uiFrames));

      desc.m_Header.m_uiBlockFrames = plImaAdpcmCodec::FramesPerBlock;
      desc.m_Header.m_uiBlockBytes = uiBlockBytes;
      desc.m_Payload.SetCountUninitialized(uiBlocks * uiBlockBytes);

      plInt16 predictors[1] = {};
      plUInt8 stepIndices[1] = {};
      plInt16 block[plImaAdpcmCodec::FramesPerBlock];

      for (plUInt32 b = 0; b < uiBlocks; ++b)
      {
        const plUInt64 uiFirst = b * plImaAdpcmCodec::FramesPerBlock;
        const plUInt32 uiCount = static_cast<plUInt32>(plMath::Min<plUInt64>(plImaAdpcmCodec::FramesPerBlock, uiFrames - uiFirst));
        plUInt8* pBlock = desc.m_Payload.GetData() + b * uiBlockBytes;

        plImaAdpcmCodec::EncodeBlock(pcm.GetData() + uiFirst, uiCount, 1, predictors, stepIndices, pBlock);
        plImaAdpcmCodec::DecodeBlock(pBlock, 1, block);

        for (plUInt32 i = 0; i < uiCount; ++i)
          played[static_cast<plUInt32>(uiFirst) + i] = block[i];
      }
    }
    else if (eLoadMode == plAudioClipLoadMode::DecompressOnLoad)
    {
      desc.m_Pcm = pcm;
    }
    else
    {
      desc.m_Payload.SetCountUninitialized(pcm.GetCount() * 2);
      plMemoryUtils::Copy(desc.m_Payload.GetData(), reinterpret_cast<const plUInt8*>(pcm.GetData()), pcm.GetCount() * 2);
    }

    plResourceManager::CreateResource<plAudioClipResource>(sId, std::move(desc));
    return played;
  }

  /// \brief A single-instrument event that plays the embedded s16 wav of pcm.
  plAudioEventResourceDescriptor MakeEmbeddedEvent(const plDynamicArray<plInt16>& pcm)
  {
    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plDynamicArray<float>());
    res.m_SampleData[0] = MakeWavS16(pcm);
    return res;
  }

  /// \brief A single-instrument event whose only playlist entry plays the clip sClipId (no embedded samples).
  plAudioEventResourceDescriptor MakeClipEvent(plStringView sClipId)
  {
    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plDynamicArray<float>());
    res.m_SampleData.Clear();
    res.m_Desc.m_SampleNames.Clear();
    res.m_Desc.m_Instruments[0].m_Playlist[0].m_uiSample = plAudioEventSilenceSample;
    res.m_Desc.m_Instruments[0].m_Playlist[0].m_sClip = sClipId;
    return res;
  }

  /// \brief Plays the event (created under sEventId) from engine frame uiPreRoll on and renders uiBlocks blocks.
  plDynamicArray<float> RenderEvent(plStringView sEventId, plAudioEventResourceDescriptor&& res, bool bLooping, plUInt32 uiBlocks, plUInt32 uiPreRoll = 0, plUInt32 uiLeadFrames = 0)
  {
    plDynamicArray<float> output;

    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return output;

    if (uiPreRoll > 0)
      engine.Render(uiPreRoll, nullptr);

    plAudioTestUtils::CreateEventResource(sEventId, std::move(res));

    plPlasmaAudioEventData eventData;
    eventData.m_bLooping = bLooping;
    plPlasmaAudioEventManager manager;

    PL_TEST_RESULT(manager.ActivateEvent(sEventId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));
    PL_TEST_BOOL(manager.IsEventPlaying(&eventData));

    if (uiLeadFrames > 0)
    {
      engine.Render(uiLeadFrames, &output);
      manager.Update(plTime::MakeFromSeconds(static_cast<double>(uiLeadFrames) / plOfflineAudioEngine::SampleRate), plVec3::MakeZero());
    }

    engine.RenderBlocks(uiBlocks, &manager, &output);
    return output;
  }

  /// \brief Checks the seams of a looping cycle rendered from uiOnset on (see Scheduler.LoopSeam).
  void CheckLoopSeams(const plDynamicArray<float>& output, plUInt32 uiOnset, plUInt32 uiCycleFrames, plUInt32 uiLoops, plStringView sLabel)
  {
    constexpr plUInt32 uiSettleFrames = 200;
    constexpr plUInt32 uiSeamWindow = 16;

    if (!PL_TEST_BOOL_MSG(uiOnset != plInvalidIndex, "no output"))
      return;

    const plUInt32 uiEnd = uiOnset + uiLoops * uiCycleFrames;
    if (!PL_TEST_BOOL_MSG(uiEnd <= output.GetCount(), "rendered too little"))
      return;

    const float fInnerStep = plAudioTestUtils::MaxStep(output, uiOnset + uiSettleFrames, uiOnset + uiCycleFrames - uiSeamWindow);
    const float fAllowed = fInnerStep + 1e-4f;

    float fWorstSeam = 0.0f;
    for (plUInt32 k = 1; k < uiLoops; ++k)
    {
      const plUInt32 uiSeam = uiOnset + k * uiCycleFrames;
      fWorstSeam = plMath::Max(fWorstSeam, plAudioTestUtils::MaxStep(output, uiSeam - uiSeamWindow, uiSeam + uiSeamWindow));
    }

    const float fOverall = plAudioTestUtils::MaxStep(output, uiOnset + uiSettleFrames, uiEnd);
    plStringBuilder sMsg;
    sMsg.SetFormat("{}: onset {}, max inner step {}, worst seam step {}, max step over {} loops {}", sLabel, uiOnset, fInnerStep, fWorstSeam, uiLoops, fOverall);
    Report(sMsg);

    PL_TEST_BOOL_MSG(fWorstSeam <= fAllowed, "%s: seam step %f exceeds %f", sMsg.GetData(), fWorstSeam, fAllowed);
    PL_TEST_BOOL_MSG(fOverall <= fAllowed, "%s: step %f exceeds %f", sMsg.GetData(), fOverall, fAllowed);
  }

  struct ClipCase
  {
    const char* m_szName;
    plAudioClipCodec::Enum m_eCodec;
    plAudioClipLoadMode::Enum m_eLoadMode;
  };

  const ClipCase s_ClipCases[] = {
    {"PCM16 DecompressOnLoad", plAudioClipCodec::Pcm16, plAudioClipLoadMode::DecompressOnLoad},
    {"ADPCM CompressedInMemory", plAudioClipCodec::ImaAdpcm, plAudioClipLoadMode::CompressedInMemory},
  };
} // namespace

PL_CREATE_SIMPLE_TEST(ClipEvent, MatchesEmbedded)
{
  // 4100 frames end inside a render block; the cosine starts at full level, so the onset is sharp
  const plDynamicArray<plInt16> pcm = ToPcm16(plAudioTestUtils::MakeCosineCycle(4100, 41, 0.5f));

  for (const ClipCase& c : s_ClipCases)
  {
    PL_TEST_BLOCK(plTestBlock::Enabled, c.m_szName)
    {
      plStringBuilder sClipId, sClipEventId, sEmbeddedEventId;
      sClipId.SetFormat("PlasmaAudioTest:MatchClip {}", c.m_szName);
      sClipEventId.SetFormat("PlasmaAudioTest:MatchClipEvent {}", c.m_szName);
      sEmbeddedEventId.SetFormat("PlasmaAudioTest:MatchEmbeddedEvent {}", c.m_szName);

      const plDynamicArray<plInt16> played = CreateClip(sClipId, pcm, c.m_eCodec, c.m_eLoadMode);

      const plDynamicArray<float> clipOut = RenderEvent(sClipEventId, MakeClipEvent(sClipId), false, 20, 1000);
      const plDynamicArray<float> embeddedOut = RenderEvent(sEmbeddedEventId, MakeEmbeddedEvent(played), false, 20, 1000);

      if (!PL_TEST_INT(clipOut.GetCount(), embeddedOut.GetCount()))
        continue;

      float fMaxDiff = 0.0f;
      for (plUInt32 i = 0; i < clipOut.GetCount(); ++i)
        fMaxDiff = plMath::Max(fMaxDiff, plMath::Abs(clipOut[i] - embeddedOut[i]));

      const plUInt32 uiOnset = plAudioTestUtils::FindFirstAbove(clipOut, SilenceThreshold);

      plStringBuilder sMsg;
      sMsg.SetFormat("MatchesEmbedded {}: onset {}, max difference {}", c.m_szName, uiOnset, fMaxDiff);
      Report(sMsg);

      PL_TEST_BOOL(uiOnset != plInvalidIndex);
      PL_TEST_BOOL(uiOnset == plAudioTestUtils::FindFirstAbove(embeddedOut, SilenceThreshold));
      PL_TEST_FLOAT(fMaxDiff, 0.0f, 0.0f);
    }
  }
}

PL_CREATE_SIMPLE_TEST(ClipEvent, StartDelayExact)
{
  const plDynamicArray<plInt16> pcm = ToPcm16(plAudioTestUtils::MakeDc(9600, 0.5f));

  for (const ClipCase& c : s_ClipCases)
  {
    PL_TEST_BLOCK(plTestBlock::Enabled, c.m_szName)
    {
      plStringBuilder sClipId;
      sClipId.SetFormat("PlasmaAudioTest:DelayClip {}", c.m_szName);
      CreateClip(sClipId, pcm, c.m_eCodec, c.m_eLoadMode);

      plStringBuilder sId;
      sId.SetFormat("PlasmaAudioTest:DelayClip0 {}", c.m_szName);
      const plUInt32 uiLatency = plAudioTestUtils::FindFirstAbove(RenderEvent(sId, MakeClipEvent(sClipId), false, 30, 1000), SilenceThreshold);

      const plUInt32 leadFrames[] = {0, 7};
      for (plUInt32 uiLead : leadFrames)
      {
        plAudioEventResourceDescriptor res = MakeClipEvent(sClipId);
        res.m_Desc.m_Instruments[0].m_StartDelay = plTime::MakeFromMilliseconds(100);

        sId.SetFormat("PlasmaAudioTest:DelayClip100_{} {}", uiLead, c.m_szName);
        const plUInt32 uiDelayed = plAudioTestUtils::FindFirstAbove(RenderEvent(sId, std::move(res), false, 30, 1000, uiLead), SilenceThreshold);

        plStringBuilder sMsg;
        sMsg.SetFormat("ClipEvent StartDelayExact {} (block offset {}): zero-delay +{}, 100 ms +{}, difference {} (expected 4800)", c.m_szName, uiLead, uiLatency, uiDelayed, static_cast<plInt64>(uiDelayed) - static_cast<plInt64>(uiLatency));
        Report(sMsg);

        if (!PL_TEST_BOOL(uiLatency != plInvalidIndex && uiDelayed != plInvalidIndex))
          continue;

        PL_TEST_INT(static_cast<plInt64>(uiDelayed) - static_cast<plInt64>(uiLatency), 4800);
      }
    }
  }
}

PL_CREATE_SIMPLE_TEST(ClipEvent, LoopSeam)
{
  constexpr plUInt32 uiLoops = 5;

  for (const ClipCase& c : s_ClipCases)
  {
    // gapless: the clip source loops natively
    {
      constexpr plUInt32 uiCycleFrames = 4800;

      plStringBuilder sLabel, sClipId, sId;
      sLabel.SetFormat("ClipEvent LoopSeam/Gapless {}", c.m_szName);
      sClipId.SetFormat("PlasmaAudioTest:SeamClipGapless {}", c.m_szName);
      sId.SetFormat("PlasmaAudioTest:SeamEventGapless {}", c.m_szName);

      PL_TEST_BLOCK(plTestBlock::Enabled, sLabel.GetData())
      {
        CreateClip(sClipId, ToPcm16(plAudioTestUtils::MakeCosineCycle(uiCycleFrames, 48, 0.5f)), c.m_eCodec, c.m_eLoadMode);

        plAudioEventResourceDescriptor res = MakeClipEvent(sClipId);
        res.m_Desc.m_Instruments[0].m_bLoop = true;

        const plDynamicArray<float> output = RenderEvent(sId, std::move(res), true, (uiLoops + 1) * uiCycleFrames / plOfflineAudioEngine::BlockFrames + 2);
        CheckLoopSeams(output, plAudioTestUtils::FindFirstAbove(output, SilenceThreshold), uiCycleFrames, uiLoops, sLabel);
      }
    }

    // two playlist entries: every pass is a new voice scheduled at the previous voice's end frame
    const plUInt32 scheduledCycles[][2] = {{4800, 48}, {4100, 41}};
    for (const auto& cfg : scheduledCycles)
    {
      const plUInt32 uiFrames = cfg[0];

      plStringBuilder sLabel, sClipId, sId;
      sLabel.SetFormat("ClipEvent LoopSeam/Scheduled {} frames {}", uiFrames, c.m_szName);
      sClipId.SetFormat("PlasmaAudioTest:SeamClip{} {}", uiFrames, c.m_szName);
      sId.SetFormat("PlasmaAudioTest:SeamEvent{} {}", uiFrames, c.m_szName);

      PL_TEST_BLOCK(plTestBlock::Enabled, sLabel.GetData())
      {
        CreateClip(sClipId, ToPcm16(plAudioTestUtils::MakeCosineCycle(uiFrames, cfg[1], 0.5f)), c.m_eCodec, c.m_eLoadMode);

        plAudioEventResourceDescriptor res = MakeClipEvent(sClipId);
        res.m_Desc.m_Instruments[0].m_bLoop = true;
        const plAudioEventPlaylistEntryDesc entry = res.m_Desc.m_Instruments[0].m_Playlist[0];
        res.m_Desc.m_Instruments[0].m_Playlist.PushBack(entry);

        const plDynamicArray<float> output = RenderEvent(sId, std::move(res), true, (uiLoops + 1) * uiFrames / plOfflineAudioEngine::BlockFrames + 2);
        CheckLoopSeams(output, plAudioTestUtils::FindFirstAbove(output, SilenceThreshold), uiFrames, uiLoops, sLabel);
      }
    }
  }
}

PL_CREATE_SIMPLE_TEST(ClipEvent, ScattererGap)
{
  constexpr plUInt32 uiExpectedGap = 2400; // 50 ms at 48 kHz
  const plUInt32 sampleFrames[] = {4800, 4100};

  for (const ClipCase& c : s_ClipCases)
  {
    for (plUInt32 uiSampleFrames : sampleFrames)
    {
      plStringBuilder sLabel, sClipId, sId;
      sLabel.SetFormat("ClipEvent ScattererGap {} frames {}", uiSampleFrames, c.m_szName);
      sClipId.SetFormat("PlasmaAudioTest:ScatterClip{} {}", uiSampleFrames, c.m_szName);
      sId.SetFormat("PlasmaAudioTest:ScatterEvent{} {}", uiSampleFrames, c.m_szName);

      PL_TEST_BLOCK(plTestBlock::Enabled, sLabel.GetData())
      {
        CreateClip(sClipId, ToPcm16(plAudioTestUtils::MakeDc(uiSampleFrames, 0.5f)), c.m_eCodec, c.m_eLoadMode);

        plAudioEventResourceDescriptor res = MakeClipEvent(sClipId);
        res.m_Desc.m_Instruments[0].m_bLoop = true;
        res.m_Desc.m_Instruments[0].m_MinRetriggerDelay = plTime::MakeFromMilliseconds(50);
        res.m_Desc.m_Instruments[0].m_MaxRetriggerDelay = plTime::MakeFromMilliseconds(50);

        const plDynamicArray<float> output = RenderEvent(sId, std::move(res), true, 5 * (uiSampleFrames + uiExpectedGap) / plOfflineAudioEngine::BlockFrames);
        const plDynamicArray<plUInt32> onsets = plAudioTestUtils::FindOnsets(output, 1e-3f, 256);

        if (!PL_TEST_BOOL_MSG(onsets.GetCount() >= 4, "only %u playbacks", onsets.GetCount()))
          continue;

        for (plUInt32 i = 1; i < onsets.GetCount(); ++i)
        {
          const plInt64 iGap = static_cast<plInt64>(onsets[i]) - static_cast<plInt64>(onsets[i - 1]) - uiSampleFrames;

          plStringBuilder sMsg;
          sMsg.SetFormat("{}: playback {} at frame {}, onset gap {} frames", sLabel, i, onsets[i], iGap);
          Report(sMsg);

          PL_TEST_INT(iGap, uiExpectedGap);
        }
      }
    }
  }
}

namespace
{
  plResult WriteAssetFile(plStringView sPath, plArrayPtr<const plUInt8> content)
  {
    plFileWriter file;
    PL_SUCCEED_OR_RETURN(file.Open(sPath));

    plAssetFileHeader assetHeader;
    assetHeader.SetFileHashAndVersion(1, 1);
    PL_SUCCEED_OR_RETURN(assetHeader.Write(file));

    return file.WriteBytes(content.GetPtr(), content.GetCount());
  }

  /// \brief Builds a mono 48 kHz clip file with one section per entry of languages: the default language, then "de".
  plResult WriteBuiltClip(plStringView sPath, plArrayPtr<const plDynamicArray<float>> languages, plAudioClipCodec::Enum eCodec, plAudioClipLoadMode::Enum eLoadMode)
  {
    plHybridArray<plAudioClipSourcePcm, 2> sources;
    plHybridArray<const plAudioClipSourcePcm*, 2> sourcePtrs;
    sources.SetCount(languages.GetCount());

    plAudioClipBuildSettings settings;
    settings.m_eCodec = eCodec;
    settings.m_eLoadMode = eLoadMode;

    for (plUInt32 i = 0; i < languages.GetCount(); ++i)
    {
      sources[i].m_Samples = languages[i];
      sources[i].m_uiSampleRate = plOfflineAudioEngine::SampleRate;
      sources[i].m_uiChannels = 1;
      sourcePtrs.PushBack(&sources[i]);

      if (languages.GetCount() > 1)
        settings.m_Languages.PushBack(i == 0 ? "" : "de");
    }

    plDynamicArray<plUInt8> bytes;
    {
      plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&bytes);
      plMemoryStreamWriter writer(&storage);
      PL_SUCCEED_OR_RETURN(plAudioClipBuilder::Build(plArrayPtr<const plAudioClipSourcePcm* const>(sourcePtrs.GetData(), sourcePtrs.GetCount()), settings, writer));
    }

    return WriteAssetFile(sPath, bytes);
  }

  /// \brief Writes an event resource file as the event asset transform does.
  plResult WriteEventFile(plStringView sPath, const plAudioEventResourceDescriptor& res)
  {
    plDynamicArray<plUInt8> bytes;
    {
      plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&bytes);
      plMemoryStreamWriter writer(&storage);
      PL_SUCCEED_OR_RETURN(res.m_Desc.Save(writer));

      writer << res.m_SampleData.GetCount();
      for (const plDataBuffer& sample : res.m_SampleData)
      {
        writer << sample.GetCount();
        PL_SUCCEED_OR_RETURN(writer.WriteBytes(sample.GetData(), sample.GetCount()));
      }
    }

    return WriteAssetFile(sPath, bytes);
  }

  /// \brief Renders uiBlocks blocks, giving the IO thread of pStreams time to fill before every block.
  void RenderStreamedBlocks(plOfflineAudioEngine& ref_engine, plPlasmaAudioEventManager& ref_manager, plAudioStreamService* pStreams, plUInt32 uiBlocks, plDynamicArray<float>& ref_output)
  {
    for (plUInt32 i = 0; i < uiBlocks; ++i)
    {
      if (pStreams != nullptr)
        pStreams->WaitForIdle();

      ref_engine.RenderBlocks(1, &ref_manager, &ref_output);
    }
  }

  float MaxAbs(const plDynamicArray<float>& samples, plUInt32 uiBegin)
  {
    float fMax = 0.0f;
    for (plUInt32 i = uiBegin; i < samples.GetCount(); ++i)
      fMax = plMath::Max(fMax, plMath::Abs(samples[i]));
    return fMax;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(ClipEvent, ClipUnloadStopsVoices)
{
  if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(">pltest/", "PlasmaAudioUnloadTest", "unloadtest", plDataDirUsage::AllowWrites)))
    return;

  PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioUnloadTest"));

  plAudioStreamService streams;
  streams.Startup();
  PL_SCOPE_EXIT(streams.Shutdown());

  const ClipCase cases[] = {
    {"PCM16 DecompressOnLoad", plAudioClipCodec::Pcm16, plAudioClipLoadMode::DecompressOnLoad},
    {"ADPCM CompressedInMemory", plAudioClipCodec::ImaAdpcm, plAudioClipLoadMode::CompressedInMemory},
    {"PCM16 Streamed", plAudioClipCodec::Pcm16, plAudioClipLoadMode::Streamed},
    {"ADPCM Streamed", plAudioClipCodec::ImaAdpcm, plAudioClipLoadMode::Streamed},
  };

  // 5 s of DC: a streamed clip plays from the ring by the time it is reloaded
  const plDynamicArray<float> dc = plAudioTestUtils::MakeDc(5 * plOfflineAudioEngine::SampleRate, 0.5f);

  for (plUInt32 uiCase = 0; uiCase < PL_ARRAY_SIZE(cases); ++uiCase)
  {
    const ClipCase& c = cases[uiCase];
    const bool bStreamed = c.m_eLoadMode == plAudioClipLoadMode::Streamed;

    for (bool bLooping : {false, true})
    {
      plStringBuilder sBlock;
      sBlock.SetFormat("{}{}", c.m_szName, bLooping ? " looping" : "");

      PL_TEST_BLOCK(plTestBlock::Enabled, sBlock.GetData())
      {
        plStringBuilder sClipPath, sEventId;
        sClipPath.SetFormat(":unloadtest/Clip{}{}.plBinAudioClip", uiCase, bLooping ? "Loop" : "");
        sEventId.SetFormat("PlasmaAudioTest:UnloadEvent{}{}", uiCase, bLooping ? "Loop" : "");

        if (!PL_TEST_RESULT(WriteBuiltClip(sClipPath, plArrayPtr<const plDynamicArray<float>>(&dc, 1), c.m_eCodec, c.m_eLoadMode)))
          continue;

        plAudioClipResourceHandle hClip = plResourceManager::LoadResource<plAudioClipResource>(sClipPath);
        {
          plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
          if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
            continue;
        }

        plOfflineAudioEngine engine;
        if (!PL_TEST_BOOL(engine.IsValid()))
          continue;

        plPlasmaAudioEventManager manager;
        manager.SetStreamService(&streams);

        plAudioEventResourceDescriptor res = MakeClipEvent(sClipPath);
        res.m_Desc.m_Instruments[0].m_bLoop = bLooping;
        plAudioTestUtils::CreateEventResource(sEventId, std::move(res));

        plPlasmaAudioEventData eventData;
        eventData.m_bLooping = bLooping;
        PL_TEST_RESULT(manager.ActivateEvent(sEventId, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

        plDynamicArray<float> output;
        RenderStreamedBlocks(engine, manager, bStreamed ? &streams : nullptr, 20, output);

        PL_TEST_INT(manager.GetActiveVoiceCount(), 1);
        PL_TEST_BOOL(MaxAbs(output, output.GetCount() - plOfflineAudioEngine::BlockFrames) > 0.4f);
        if (bStreamed)
        {
          PL_TEST_INT(streams.GetActiveStreamCount(), 1);
        }

        // hot reload: the clip data is freed right away, the voice reading it is stopped first
        PL_TEST_BOOL(plResourceManager::ReloadResource(hClip, true));
        PL_TEST_INT(manager.GetActiveVoiceCount(), 0);
        PL_TEST_INT(streams.GetActiveStreamCount(), 0);

        const plUInt32 uiReloadFrame = output.GetCount();
        RenderStreamedBlocks(engine, manager, nullptr, 10, output);

        const float fAfterReload = MaxAbs(output, uiReloadFrame);
        PL_TEST_BOOL_MSG(fAfterReload < 1e-4f, "%s: level %f after the reload", sBlock.GetData(), fAfterReload);
        PL_TEST_BOOL(manager.IsEventPlaying(&eventData) == bLooping);

        if (bLooping)
        {
          // a looping instrument plays the clip again once it is reloaded
          {
            plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
            PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final);
          }

          const plUInt32 uiReloadedFrame = output.GetCount();
          RenderStreamedBlocks(engine, manager, bStreamed ? &streams : nullptr, 20, output);
          PL_TEST_INT(manager.GetActiveVoiceCount(), 1);
          PL_TEST_BOOL(MaxAbs(output, uiReloadedFrame) > 0.4f);

          manager.StopAll(true);
        }

        plStringBuilder sMsg;
        sMsg.SetFormat("ClipUnloadStopsVoices {}: level {} after the reload", sBlock, fAfterReload);
        Report(sMsg);
      }
    }
  }
}

PL_CREATE_SIMPLE_TEST(ClipEvent, EventUnloadStopsVoices)
{
  if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(">pltest/", "PlasmaAudioUnloadTest", "unloadtest", plDataDirUsage::AllowWrites)))
    return;

  PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioUnloadTest"));

  // an event resource file with an embedded 2 s sample, read by the voice's decoder straight from the resource
  const plStringView sEventPath = ":unloadtest/EmbeddedEvent.plAudioEvent";
  if (!PL_TEST_RESULT(WriteEventFile(sEventPath, MakeEmbeddedEvent(ToPcm16(plAudioTestUtils::MakeDc(2 * plOfflineAudioEngine::SampleRate, 0.5f))))))
    return;

  plAudioEventResourceHandle hEvent = plResourceManager::LoadResource<plAudioEventResource>(sEventPath);

  // activating an event that is not loaded queues the start (see PendingStart); loaded here, so it starts right away
  auto LoadEvent = [&]()
  {
    plResourceLock<plAudioEventResource> pEvent(hEvent, plResourceAcquireMode::BlockTillLoaded);
    return pEvent.GetAcquireResult() == plResourceAcquireResult::Final;
  };

  if (!PL_TEST_BOOL(LoadEvent()))
    return;

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plPlasmaAudioEventManager manager;
  plPlasmaAudioEventData eventData;
  PL_TEST_RESULT(manager.ActivateEvent(sEventPath, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

  plDynamicArray<float> output;
  engine.RenderBlocks(20, &manager, &output);
  PL_TEST_INT(manager.GetActiveVoiceCount(), 1);
  PL_TEST_BOOL(MaxAbs(output, output.GetCount() - plOfflineAudioEngine::BlockFrames) > 0.4f);

  // hot reload of the event: its sample data is freed right away, the instance stops first and ends
  PL_TEST_BOOL(plResourceManager::ReloadResource(hEvent, true));
  PL_TEST_INT(manager.GetActiveVoiceCount(), 0);

  const plUInt32 uiReloadFrame = output.GetCount();
  engine.RenderBlocks(10, &manager, &output);
  PL_TEST_BOOL(MaxAbs(output, uiReloadFrame) < 1e-4f);
  PL_TEST_BOOL(!manager.IsEventPlaying(&eventData));
  PL_TEST_INT(manager.GetInstanceCount(), 0);

  // the event loads again and plays in new instances
  PL_TEST_BOOL(LoadEvent());
  PL_TEST_RESULT(manager.ActivateEvent(sEventPath, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));
  const plUInt32 uiRestartFrame = output.GetCount();
  engine.RenderBlocks(10, &manager, &output);
  PL_TEST_INT(manager.GetActiveVoiceCount(), 1);
  PL_TEST_BOOL(MaxAbs(output, uiRestartFrame) > 0.4f);
}

PL_CREATE_SIMPLE_TEST(ClipEvent, PendingStart)
{
  if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(">pltest/", "PlasmaAudioPendingTest", "pendingtest", plDataDirUsage::AllowWrites)))
    return;

  PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioPendingTest"));

  const plStringView sEventPath = ":pendingtest/PendingEvent.plAudioEvent";
  const plStringView sStoppedPath = ":pendingtest/StoppedEvent.plAudioEvent";
  if (!PL_TEST_RESULT(WriteEventFile(sEventPath, MakeEmbeddedEvent(ToPcm16(plAudioTestUtils::MakeDc(plOfflineAudioEngine::SampleRate, 0.5f))))))
    return;
  if (!PL_TEST_RESULT(WriteEventFile(sStoppedPath, MakeEmbeddedEvent(ToPcm16(plAudioTestUtils::MakeDc(plOfflineAudioEngine::SampleRate, 0.5f))))))
    return;

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plPlasmaAudioEventManager manager;
  plDynamicArray<float> output;

  // renders until no start waits any more; the loader runs on its own threads, so this waits in wall-clock time
  auto RenderUntilStarted = [&]()
  {
    const plTime tEnd = plTime::Now() + plTime::MakeFromSeconds(5);
    while (manager.GetPendingActivationCount() > 0 && plTime::Now() < tEnd)
    {
      engine.RenderBlocks(1, &manager, &output);
      plThreadUtils::Sleep(plTime::MakeFromMilliseconds(1));
    }
  };

  // an event that is not loaded: the start is queued and the audio thread does not wait for the file
  {
    plPlasmaAudioEventData eventData;
    PL_TEST_RESULT(manager.ActivateEvent(sEventPath, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));
    PL_TEST_INT(manager.GetActiveVoiceCount(), 0);
    PL_TEST_BOOL(manager.IsEventPlaying(&eventData));

    RenderUntilStarted();
    PL_TEST_INT(manager.GetPendingActivationCount(), 0);
    PL_TEST_INT(manager.GetActiveVoiceCount(), 1);

    const plUInt32 uiStart = output.GetCount();
    engine.RenderBlocks(5, &manager, &output);
    PL_TEST_BOOL(MaxAbs(output, uiStart) > 0.4f);

    manager.StopAll(true);
  }

  // a stop before the event finished loading drops the start
  {
    plPlasmaAudioEventData eventData;
    PL_TEST_RESULT(manager.ActivateEvent(sStoppedPath, nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));
    PL_TEST_INT(manager.GetPendingActivationCount(), 1);

    PL_TEST_RESULT(manager.StopEvent(&eventData, false));
    PL_TEST_INT(manager.GetPendingActivationCount(), 0);
    PL_TEST_BOOL(!manager.IsEventPlaying(&eventData));

    // the load that was requested keeps running; it finishes before the data directory goes away
    {
      plResourceLock<plAudioEventResource> pEvent(plResourceManager::LoadResource<plAudioEventResource>(sStoppedPath), plResourceAcquireMode::BlockTillLoaded);
    }

    engine.RenderBlocks(5, &manager, &output);
    PL_TEST_INT(manager.GetInstanceCount(), 0);
  }
}

PL_CREATE_SIMPLE_TEST(ClipEvent, LanguageSwitchReloadsStaleClip)
{
  if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(">pltest/", "PlasmaAudioLanguageTest", "languagetest", plDataDirUsage::AllowWrites)))
    return;

  PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioLanguageTest"));
  PL_SCOPE_EXIT(plAudioClipResourceLoader::SetLanguage(""));

  plAudioClipResourceLoader::SetLanguage("");

  // the default language plays DC at 0.25, "de" at 0.5
  const plDynamicArray<float> languages[] = {plAudioTestUtils::MakeDc(plOfflineAudioEngine::SampleRate, 0.25f), plAudioTestUtils::MakeDc(plOfflineAudioEngine::SampleRate, 0.5f)};
  const plStringView sClipPath = ":languagetest/TwoLanguages.plBinAudioClip";
  if (!PL_TEST_RESULT(WriteBuiltClip(sClipPath, languages, plAudioClipCodec::Pcm16, plAudioClipLoadMode::DecompressOnLoad)))
    return;

  plAudioClipResourceHandle hClip = plResourceManager::LoadResource<plAudioClipResource>(sClipPath);
  {
    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    PL_TEST_STRING(pClip->GetActiveLanguage(), "");
    PL_TEST_INT(pClip->GetLanguageGeneration(), plAudioClipResourceLoader::GetLanguageGeneration());
  }

  // the language changes without the clip being reloaded, as for a clip whose load was in flight during the switch
  plAudioClipResourceLoader::SetLanguage("de");

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plPlasmaAudioEventManager manager;
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:LanguageEvent", MakeClipEvent(sClipPath));

  plPlasmaAudioEventData eventData;
  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:LanguageEvent", nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

  // the voice does not start with the stale language: the clip is reloaded first
  PL_TEST_INT(manager.GetActiveVoiceCount(), 0);

  {
    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    PL_TEST_STRING(pClip->GetActiveLanguage(), "de");
    PL_TEST_INT(pClip->GetLanguageGeneration(), plAudioClipResourceLoader::GetLanguageGeneration());
  }

  plDynamicArray<float> output;
  engine.RenderBlocks(20, &manager, &output);
  PL_TEST_INT(manager.GetActiveVoiceCount(), 1);

  // the level after the onset transient of the occlusion filter has settled
  const float fLevel = MaxAbs(output, output.GetCount() - plOfflineAudioEngine::BlockFrames);
  plStringBuilder sMsg;
  sMsg.SetFormat("LanguageSwitchReloadsStaleClip: level {} (default language 0.25, de 0.5)", fLevel);
  Report(sMsg);
  PL_TEST_FLOAT(fLevel, 0.5f, 0.01f);
}
