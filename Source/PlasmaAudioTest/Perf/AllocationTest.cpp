#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/FileSystem/FileWriter.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <Foundation/Types/ScopeExit.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <PlasmaAudioPlugin/Clips/AudioClipBuilder.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResource.h>
#include <PlasmaAudioPlugin/Clips/AudioClipVoiceSource.h>
#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioPlugin/MiniAudioSingleton.h>
#include <PlasmaAudioPlugin/Streaming/AudioStreamService.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

#include <Foundation/IO/OSFile.h>

#include <cmath>

PL_CREATE_SIMPLE_TEST_GROUP(Perf);

namespace
{
  void Report(plStringView sText)
  {
    plStringBuilder sTmp = sText;
    plTestFramework::Output(plTestOutput::Message, "%s", sTmp.GetData());
  }

#define PL_PERF_TEST_REPORT(...)                   \
  do                                               \
  {                                                \
    plStringBuilder sPerfReport;                   \
    sPerfReport.SetFormat(__VA_ARGS__);            \
    Report(sPerfReport);                           \
  } while (false)

  plUInt64 GetDefaultAllocations()
  {
    return plFoundation::GetDefaultAllocator()->GetStats().m_uiNumAllocations;
  }

  plUInt64 GetAllocations(plAllocator* pAllocator)
  {
    return pAllocator != nullptr ? pAllocator->GetStats().m_uiNumAllocations : 0;
  }

  struct ClipCase
  {
    const char* m_szName;
    plAudioClipCodec::Enum m_eCodec;
    plAudioClipLoadMode::Enum m_eLoadMode;
  };

  constexpr ClipCase s_Cases[] = {
    {"Pcm", plAudioClipCodec::Pcm16, plAudioClipLoadMode::DecompressOnLoad},
    {"Adpcm", plAudioClipCodec::ImaAdpcm, plAudioClipLoadMode::CompressedInMemory},
    {"Opus", plAudioClipCodec::Opus, plAudioClipLoadMode::CompressedInMemory},
    {"AdpcmStreamed", plAudioClipCodec::ImaAdpcm, plAudioClipLoadMode::Streamed},
    {"OpusStreamed", plAudioClipCodec::Opus, plAudioClipLoadMode::Streamed},
  };

  /// 2 s stereo sine, written as a clip file under :perftest/ and loaded.
  plAudioClipResourceHandle LoadClip(const ClipCase& c)
  {
    plAudioClipSourcePcm source;
    source.m_uiChannels = 2;
    source.m_uiSampleRate = 48000;
    source.m_Samples.SetCountUninitialized(2 * 48000 * 2);
    for (plUInt32 f = 0; f < 2 * 48000; ++f)
    {
      const float fValue = 0.5f * static_cast<float>(std::sin(2.0 * plMath::Pi<double>() * 440.0 * f / 48000.0));
      source.m_Samples[2 * f] = fValue;
      source.m_Samples[2 * f + 1] = fValue;
    }

    plAudioClipBuildSettings settings;
    settings.m_eCodec = c.m_eCodec;
    settings.m_eLoadMode = c.m_eLoadMode;

    plDynamicArray<plUInt8> bytes;
    {
      plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&bytes);
      plMemoryStreamWriter writer(&storage);
      if (plAudioClipBuilder::Build(source, settings, writer).Failed())
        return {};
    }

    plStringBuilder sPath;
    sPath.SetFormat(":perftest/{}.plBinAudioClip", c.m_szName);

    {
      plFileWriter file;
      if (file.Open(sPath).Failed())
        return {};

      plAssetFileHeader assetHeader;
      assetHeader.SetFileHashAndVersion(1, 1);
      if (assetHeader.Write(file).Failed() || file.WriteBytes(bytes.GetData(), bytes.GetCount()).Failed())
        return {};
    }

    return plResourceManager::LoadResource<plAudioClipResource>(sPath);
  }

  /// Starts the voice source, reads 4800 frames (waiting for the stream when there is one) and stops it again.
  bool CycleVoiceSource(const plAudioClipResource& clip, plAudioClipVoiceSource& ref_voice, plAudioStreamService& ref_streams)
  {
    if (clip.CreateVoiceSource(ref_voice, &ref_streams).Failed())
      return false;

    plInt16 buffer[480 * 2];
    for (plUInt32 i = 0; i < 10; ++i)
    {
      if (ref_voice.m_pStream != nullptr)
      {
        ref_streams.WaitForIdle();
      }

      ma_uint64 uiRead = 0;
      ma_data_source_read_pcm_frames(ref_voice.m_pSource, buffer, 480, &uiRead);
    }

    ref_voice.Uninit();
    return true;
  }

  plAudioEventResourceDescriptor MakeClipEvent(plStringView sClipId)
  {
    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plDynamicArray<float>());
    res.m_SampleData.Clear();
    res.m_Desc.m_SampleNames.Clear();
    res.m_Desc.m_Instruments[0].m_Playlist[0].m_uiSample = plAudioEventSilenceSample;
    res.m_Desc.m_Instruments[0].m_Playlist[0].m_sClip = sClipId;
    return res;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Perf, Allocation)
{
  if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(">pltest/", "PlasmaAudioPerfTest", "perftest", plDataDirUsage::AllowWrites)))
    return;

  PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioPerfTest"));

  plAudioStreamService streams;
  streams.Startup();
  PL_SCOPE_EXIT(streams.Shutdown());

  // one stream plays at a time; a second pooled one covers a release that the IO thread pools one pass later
  streams.ReserveStreams(2);

  constexpr plUInt32 uiWarmUp = 3;
  constexpr plUInt32 uiCycles = 16;

  PL_TEST_BLOCK(plTestBlock::Enabled, "Voice sources reuse their codec memory and streams")
  {
    for (const ClipCase& c : s_Cases)
    {
      plAudioClipResourceHandle hClip = LoadClip(c);
      plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
      if (!PL_TEST_BOOL_MSG(pClip.GetAcquireResult() == plResourceAcquireResult::Final, "clip %s did not load", c.m_szName))
        continue;

      PL_TEST_BOOL(pClip->GetLoadMode() == c.m_eLoadMode);

      plAudioClipVoiceSource voice;
      bool bOk = true;

      for (plUInt32 i = 0; i < uiWarmUp; ++i)
        bOk &= CycleVoiceSource(*pClip.GetPointer(), voice, streams);

      const bool bStreamed = c.m_eLoadMode == plAudioClipLoadMode::Streamed;
      const plUInt32 uiPooledBefore = streams.GetPooledStreamCount();

      const plUInt64 uiBefore = GetDefaultAllocations();
      for (plUInt32 i = 0; i < uiCycles; ++i)
        bOk &= CycleVoiceSource(*pClip.GetPointer(), voice, streams);
      const plUInt64 uiAllocations = GetDefaultAllocations() - uiBefore;

      PL_TEST_BOOL(bOk);
      PL_TEST_INT(streams.GetActiveStreamCount(), 0);

      // the voice source's codec arrays, the Opus decoder state and the stream objects are reused
      plUInt64 uiFileOpenAllocations = 0;

      if (bStreamed)
      {
        // one voice at a time: the reserved stream objects are reused, none is added
        PL_TEST_INT(uiPooledBefore, 2);
        PL_TEST_INT(streams.GetPooledStreamCount(), 2);

        // the stream opens the clip file for every start; what plOSFile allocates for that is not the stream's
        const plString sPath = pClip->GetOsFilePath();
        if (PL_TEST_BOOL(!sPath.IsEmpty()))
        {
          plOSFile file;
          const plUInt64 uiOpenBefore = GetDefaultAllocations();
          for (plUInt32 i = 0; i < uiCycles; ++i)
          {
            if (file.Open(sPath, plFileOpenMode::Read, plFileShareMode::SharedReadsAndDelete).Succeeded())
              file.Close();
          }
          uiFileOpenAllocations = GetDefaultAllocations() - uiOpenBefore;
        }
      }

      PL_TEST_INT(uiAllocations, uiFileOpenAllocations);

      PL_PERF_TEST_REPORT("Voice source {}: {} default-allocator allocations over {} start/stop cycles ({} of them from opening the clip file)", c.m_szName, uiAllocations, uiCycles, uiFileOpenAllocations);
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Event voices allocate only their miniaudio node heaps")
  {
    plMiniAudioSingleton* pBackend = plMiniAudioSingleton::GetSingleton();
    if (!PL_TEST_BOOL(pBackend != nullptr))
      return;

    // the backend initializes COM on this thread itself
    pBackend->Startup();
    PL_SCOPE_EXIT(pBackend->Shutdown());

    ma_engine* pEngine = pBackend->GetEngine();
    if (!PL_TEST_BOOL(pEngine != nullptr))
      return;

    plAllocator* pMaAllocator = pBackend->GetMiniAudioAllocator();
    PL_TEST_BOOL(GetAllocations(pMaAllocator) > 0);

    plMiniAudioBusGraph busGraph;
    {
      plAudioMixerDesc mixer;
      mixer.MakeDefault();
      PL_TEST_RESULT(busGraph.Build(mixer, pEngine));
    }
    PL_SCOPE_EXIT(busGraph.Teardown());

    const plTime tStep = plTime::MakeFromMilliseconds(10);

    for (const ClipCase& c : s_Cases)
    {
      plStringBuilder sClipId;
      sClipId.SetFormat(":perftest/{}.plBinAudioClip", c.m_szName);

      plStringBuilder sEventId;
      sEventId.SetFormat("PlasmaAudioPerfTest:{}Event", c.m_szName);
      plAudioTestUtils::CreateEventResource(sEventId, MakeClipEvent(sClipId));

      plPlasmaAudioEventManager manager;
      manager.SetStreamService(&streams);
      plPlasmaAudioEventData eventData;

      plUInt32 uiPlayed = 0;
      auto Cycle = [&]()
      {
        if (manager.ActivateEvent(sEventId, nullptr, &eventData, &busGraph, pEngine).Failed())
          return;

        for (plUInt32 i = 0; i < 3; ++i)
        {
          pBackend->UpdateSound(tStep);
          manager.Update(tStep, plVec3::MakeZero());
        }

        if (manager.GetActiveVoiceCount() > 0)
          ++uiPlayed;

        manager.StopEvent(&eventData, true).IgnoreResult();

        for (plUInt32 i = 0; i < 2; ++i)
        {
          pBackend->UpdateSound(tStep);
          manager.Update(tStep, plVec3::MakeZero());
        }
      };

      for (plUInt32 i = 0; i < uiWarmUp; ++i)
        Cycle();

      uiPlayed = 0;
      const plUInt64 uiDefaultBefore = GetDefaultAllocations();
      const plUInt64 uiMaBefore = GetAllocations(pMaAllocator);

      for (plUInt32 i = 0; i < uiCycles; ++i)
        Cycle();

      const plUInt64 uiMa = GetAllocations(pMaAllocator) - uiMaBefore;
      // the miniaudio allocator forwards to the default allocator, which counts those allocations as well
      const plUInt64 uiOther = GetDefaultAllocations() - uiDefaultBefore - uiMa;

      PL_TEST_INT(uiPlayed, uiCycles);
      PL_TEST_INT(manager.GetInstanceCount(), 0);

      // Instances, their arrays and clip sources come from the manager's pool, codec state and streams are reused.
      // What remains are the heaps of the voice's miniaudio nodes, which cannot be preallocated: the ma_sound (1) and
      // the occlusion filter node (1; its filter state lives inline in the voice). The default mixer has no environment
      // bus, so there is no splitter node.
      constexpr plUInt64 uiNodeHeapsPerVoice = 2;
      PL_TEST_INT(uiOther, 0);
      PL_TEST_INT(uiMa, uiNodeHeapsPerVoice * uiCycles);

      PL_PERF_TEST_REPORT("Event voice {}: per start/stop {} miniaudio-allocator allocations (voice node heaps), {} other default-allocator allocations", c.m_szName,
        plArgF(static_cast<double>(uiMa) / uiCycles, 1), plArgF(static_cast<double>(uiOther) / uiCycles, 1));
    }
  }

  plResourceManager::FreeAllUnusedResources();
}

PL_CREATE_SIMPLE_TEST(Perf, DspLoad)
{
  plMiniAudioSingleton* pBackend = plMiniAudioSingleton::GetSingleton();
  if (!PL_TEST_BOOL(pBackend != nullptr))
    return;

  pBackend->Startup();
  PL_SCOPE_EXIT(pBackend->Shutdown());

  if (!PL_TEST_BOOL(pBackend->IsEngineValid()))
    return;

  // with a device the device thread measures its callbacks, without one UpdateSound measures the mix
  auto Run = [&]()
  {
    for (plUInt32 i = 0; i < 20; ++i)
    {
      pBackend->UpdateSound(plTime::MakeFromMilliseconds(10));
      plThreadUtils::Sleep(plTime::MakeFromMilliseconds(10));
    }
  };

  PL_TEST_BLOCK(plTestBlock::Enabled, "Mixing is measured")
  {
    Run();

    const float fLoad = pBackend->GetDspLoad();
    const float fMicros = pBackend->GetLastMixMicroseconds();
    PL_TEST_BOOL(fMicros > 0.0f);
    PL_TEST_BOOL(fLoad > 0.0f && fLoad < 1.0f);

    PL_PERF_TEST_REPORT("DSP load {}, last mix {} us", plArgF(fLoad, 4), plArgF(fMicros, 1));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Mixing is measured after a device re-init")
  {
    if (!PL_TEST_RESULT(pBackend->ReinitDevice()))
      return;

    Run();

    const float fLoad = pBackend->GetDspLoad();
    const float fMicros = pBackend->GetLastMixMicroseconds();
    PL_TEST_BOOL(fMicros > 0.0f);
    PL_TEST_BOOL(fLoad > 0.0f && fLoad < 1.0f);

    PL_PERF_TEST_REPORT("After re-init: DSP load {}, last mix {} us", plArgF(fLoad, 4), plArgF(fMicros, 1));
  }
}
