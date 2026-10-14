#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/FileSystem/FileWriter.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <PlasmaAudioPlugin/Clips/AudioClipBuilder.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResource.h>
#include <PlasmaAudioPlugin/Events/AudioVoiceManager.h>
#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioPlugin/Streaming/AudioStreamService.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

PL_CREATE_SIMPLE_TEST_GROUP(Voices);

namespace
{
  constexpr float SilenceThreshold = 1e-6f;

  void Report(plStringView sText)
  {
    plStringBuilder sTmp = sText;
    plTestFramework::Output(plTestOutput::Message, "%s", sTmp.GetData());
  }

  /// \brief Sets the voice budgets for the lifetime of the scope and restores the cvar values afterwards.
  struct BudgetScope
  {
    BudgetScope(plInt32 iReal, plInt32 iVirtual) { plAudioVoiceManager::SetBudgets(iReal, iVirtual); }
    ~BudgetScope() { plAudioVoiceManager::SetBudgets(-1, -1); }
  };

  struct Emitter
  {
    plPlasmaAudioEntityData m_Entity;
    plPlasmaAudioEventData m_Event;

    void SetPosition(const plVec3& vPos) { m_Entity.m_LastTransform.m_vPosition = vPos; }
  };

  /// \brief A single-instrument event without instance limit, spatialized with min distance 1 and max distance 100.
  plAudioEventResourceDescriptor MakeVoiceEvent(const plDynamicArray<float>& samples, bool bLoop, bool bSpatialize, plUInt8 uiPriority)
  {
    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(samples);
    res.m_Desc.m_uiMaxInstances = 0;
    res.m_Desc.m_eStealing = plAudioEventStealingMode::None;
    res.m_Desc.m_uiPriority = uiPriority;
    res.m_Desc.m_bSpatialize = bSpatialize;
    res.m_Desc.m_fMinDistance = 1.0f;
    res.m_Desc.m_fMaxDistance = 100.0f;
    res.m_Desc.m_fRolloff = 1.0f;
    res.m_Desc.m_Instruments[0].m_bLoop = bLoop;
    return res;
  }

  plUInt32 GetRealCount(const plPlasmaAudioEventManager& manager, const Emitter& emitter)
  {
    plUInt32 uiReal = 0;
    plUInt32 uiVirtual = 0;
    manager.GetEventVoiceCounts(&emitter.m_Event, uiReal, uiVirtual);
    return uiReal;
  }

  plUInt32 GetVirtualCount(const plPlasmaAudioEventManager& manager, const Emitter& emitter)
  {
    plUInt32 uiReal = 0;
    plUInt32 uiVirtual = 0;
    manager.GetEventVoiceCounts(&emitter.m_Event, uiReal, uiVirtual);
    return uiVirtual;
  }

  plResult WriteAssetFile(plStringView sPath, plArrayPtr<const plUInt8> content)
  {
    plFileWriter file;
    PL_SUCCEED_OR_RETURN(file.Open(sPath));

    plAssetFileHeader assetHeader;
    assetHeader.SetFileHashAndVersion(1, 1);
    PL_SUCCEED_OR_RETURN(assetHeader.Write(file));

    return file.WriteBytes(content.GetPtr(), content.GetCount());
  }

  /// \brief Builds a mono 48 kHz clip file.
  plResult WriteBuiltClip(plStringView sPath, const plDynamicArray<float>& samples, plAudioClipCodec::Enum eCodec, plAudioClipLoadMode::Enum eLoadMode)
  {
    plAudioClipSourcePcm source;
    source.m_Samples = samples;
    source.m_uiSampleRate = plOfflineAudioEngine::SampleRate;
    source.m_uiChannels = 1;
    const plAudioClipSourcePcm* pSource = &source;

    plAudioClipBuildSettings settings;
    settings.m_eCodec = eCodec;
    settings.m_eLoadMode = eLoadMode;

    plDynamicArray<plUInt8> bytes;
    {
      plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&bytes);
      plMemoryStreamWriter writer(&storage);
      PL_SUCCEED_OR_RETURN(plAudioClipBuilder::Build(plArrayPtr<const plAudioClipSourcePcm* const>(&pSource, 1), settings, writer));
    }

    return WriteAssetFile(sPath, bytes);
  }

  struct StressResult
  {
    plDynamicArray<plUInt32> m_RealEmitters;
    plUInt32 m_uiMaxRealCount = 0;
    plUInt32 m_uiVirtualCount = 0;
    double m_fMeanUpdateMs = 0.0;
    double m_fMaxUpdateMs = 0.0;
    bool m_bClosestAreReal = true;
  };

  /// \brief Plays a looping event on uiEmitters entities at distinct distances with budgets 64 / 512 and returns
  /// which emitters are real after uiBlocks blocks.
  StressResult RunStress(plStringView sEventId, plUInt32 uiEmitters, plUInt32 uiBlocks)
  {
    StressResult result;

    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return result;

    plPlasmaAudioEventManager manager;

    plDynamicArray<plUniquePtr<Emitter>> emitters;
    plDynamicArray<float> distances;

    for (plUInt32 i = 0; i < uiEmitters; ++i)
    {
      // distinct distances in [1, 91) in a scrambled order, on a circle around the listener
      const float fDistance = 1.0f + static_cast<float>((i * 7919u) % uiEmitters) * (90.0f / static_cast<float>(uiEmitters));
      const plAngle angle = plAngle::MakeFromDegree(static_cast<float>(i % 360));

      plUniquePtr<Emitter> pEmitter = PL_DEFAULT_NEW(Emitter);
      pEmitter->m_Event.m_bLooping = true;
      pEmitter->SetPosition(plVec3(plMath::Cos(angle) * fDistance, plMath::Sin(angle) * fDistance, 0.0f));
      distances.PushBack(fDistance);
      emitters.PushBack(std::move(pEmitter));
    }

    for (auto& pEmitter : emitters)
    {
      PL_TEST_RESULT(manager.ActivateEvent(sEventId, &pEmitter->m_Entity, &pEmitter->m_Event, engine.GetBusGraph(), engine.GetEngine()));
    }

    double fTotalMs = 0.0;
    for (plUInt32 uiBlock = 0; uiBlock < uiBlocks; ++uiBlock)
    {
      engine.Render(plOfflineAudioEngine::BlockFrames, nullptr);

      const plTime tStart = plTime::Now();
      manager.Update(plOfflineAudioEngine::GetBlockDuration(), plVec3::MakeZero());
      const double fMs = (plTime::Now() - tStart).GetMilliseconds();

      fTotalMs += fMs;
      result.m_fMaxUpdateMs = plMath::Max(result.m_fMaxUpdateMs, fMs);
      result.m_uiMaxRealCount = plMath::Max(result.m_uiMaxRealCount, manager.GetRealVoiceCount());
    }

    result.m_fMeanUpdateMs = fTotalMs / plMath::Max(uiBlocks, 1u);
    result.m_uiVirtualCount = manager.GetVirtualVoiceCount();

    // with equal priorities the closest emitters are the loudest
    plDynamicArray<float> sorted = distances;
    sorted.Sort();
    const float fCut = sorted[plMath::Min<plUInt32>(plAudioVoiceManager::GetMaxRealVoices(), sorted.GetCount()) - 1];

    for (plUInt32 i = 0; i < emitters.GetCount(); ++i)
    {
      const bool bReal = GetRealCount(manager, *emitters[i]) > 0;
      if (bReal)
        result.m_RealEmitters.PushBack(i);

      if (bReal != (distances[i] <= fCut))
        result.m_bClosestAreReal = false;
    }

    manager.StopAll(true);
    return result;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Voices, Stress)
{
  BudgetScope budgets(64, 512);

  constexpr plUInt32 uiEmitters = 2000;
  constexpr plUInt32 uiBlocks = 20;

  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoiceStress", MakeVoiceEvent(plAudioTestUtils::MakeDc(4800, 0.001f), true, true, 128));

  const StressResult run0 = RunStress("PlasmaAudioTest:VoiceStress", uiEmitters, uiBlocks);
  const StressResult run1 = RunStress("PlasmaAudioTest:VoiceStress", uiEmitters, uiBlocks);

  plStringBuilder sMsg;
  sMsg.SetFormat("Voices/Stress: {} emitters, {} real (max {}), {} virtual; Update mean {} ms, max {} ms (run 2: mean {} ms, max {} ms)", uiEmitters, run0.m_RealEmitters.GetCount(), run0.m_uiMaxRealCount, run0.m_uiVirtualCount,
    plArgF(run0.m_fMeanUpdateMs, 3), plArgF(run0.m_fMaxUpdateMs, 3), plArgF(run1.m_fMeanUpdateMs, 3), plArgF(run1.m_fMaxUpdateMs, 3));
  Report(sMsg);

  PL_TEST_BOOL(run0.m_uiMaxRealCount <= 64);
  PL_TEST_BOOL(run1.m_uiMaxRealCount <= 64);
  PL_TEST_INT(run0.m_RealEmitters.GetCount(), 64);
  PL_TEST_INT(run0.m_uiVirtualCount, uiEmitters - 64);
  PL_TEST_BOOL(run0.m_bClosestAreReal);

  // the ranking is deterministic
  PL_TEST_BOOL(run0.m_RealEmitters == run1.m_RealEmitters);
}

PL_CREATE_SIMPLE_TEST(Voices, ResumePosition)
{
  constexpr plUInt32 uiRampFrames = plOfflineAudioEngine::SampleRate;

  // a ramp, so every source frame has its own value
  plDynamicArray<float> ramp;
  ramp.SetCountUninitialized(uiRampFrames);
  for (plUInt32 i = 0; i < uiRampFrames; ++i)
    ramp[i] = 0.1f + 0.8f * static_cast<float>(i) / static_cast<float>(uiRampFrames);

  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoiceResume", MakeVoiceEvent(ramp, false, false, 128));

  constexpr plUInt32 uiRealBlocks = 5;
  constexpr plUInt32 uiVirtualBlocks = 20;
  constexpr plUInt32 uiTailBlocks = 35;

  // reference: the voice stays real
  plDynamicArray<float> reference;
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    plPlasmaAudioEventManager manager;
    Emitter emitter;
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceResume", &emitter.m_Entity, &emitter.m_Event, engine.GetBusGraph(), engine.GetEngine()));
    engine.RenderBlocks(uiRealBlocks + 1 + uiVirtualBlocks + uiTailBlocks, &manager, &reference);
  }

  const plUInt32 uiLatency = plAudioTestUtils::FindFirstAbove(reference, SilenceThreshold);
  if (!PL_TEST_BOOL(uiLatency != plInvalidIndex))
    return;

  plDynamicArray<float> output;
  ma_uint64 uiResumeFrame = 0;
  plUInt32 uiVirtualizedAt = 0;
  plUInt32 uiRealizedAt = 0;
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    plPlasmaAudioEventManager manager;
    Emitter emitter;

    BudgetScope budgets(64, 512);
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceResume", &emitter.m_Entity, &emitter.m_Event, engine.GetBusGraph(), engine.GetEngine()));
    engine.RenderBlocks(uiRealBlocks, &manager, &output);
    PL_TEST_INT(GetRealCount(manager, emitter), 1);

    // no real voice: the voice goes virtual in the next update
    plAudioVoiceManager::SetBudgets(0, 512);
    engine.RenderBlocks(1, &manager, &output);
    uiVirtualizedAt = output.GetCount();
    PL_TEST_INT(GetRealCount(manager, emitter), 0);
    PL_TEST_INT(GetVirtualCount(manager, emitter), 1);

    engine.RenderBlocks(uiVirtualBlocks - 1, &manager, &output);
    PL_TEST_INT(GetVirtualCount(manager, emitter), 1);

    // one real voice again: the update after this block realizes it one look-ahead span ahead
    plAudioVoiceManager::SetBudgets(1, 512);
    engine.RenderBlocks(1, &manager, &output);
    uiRealizedAt = output.GetCount();
    uiResumeFrame = engine.GetTime() + plOfflineAudioEngine::LookAheadFrames;
    PL_TEST_INT(GetRealCount(manager, emitter), 1);

    engine.RenderBlocks(uiTailBlocks, &manager, &output);
  }

  if (!PL_TEST_INT(output.GetCount(), reference.GetCount()))
    return;

  const plUInt32 uiFadeFrames = static_cast<plUInt32>(plPlasmaAudioEventInstance::VirtualFadeDuration.GetSeconds() * plOfflineAudioEngine::SampleRate + 0.5);

  // The virtualization fades the voice out over uiFadeFrames instead of cutting it: no step in the output is larger
  // than the signal's own step plus the step of a linear fade from the signal's level to silence.
  auto AllowedStep = [&](plUInt32 uiBegin, plUInt32 uiEnd)
  {
    float fPeak = 0.0f;
    for (plUInt32 i = uiBegin; i < uiEnd; ++i)
      fPeak = plMath::Max(fPeak, plMath::Abs(reference[i]));

    return plAudioTestUtils::MaxStep(reference, uiBegin, uiEnd) + 1.5f * fPeak / static_cast<float>(uiFadeFrames);
  };

  const plUInt32 uiFadeOutBegin = uiVirtualizedAt - plOfflineAudioEngine::BlockFrames;
  const plUInt32 uiFadeOutEnd = uiVirtualizedAt + 3 * plOfflineAudioEngine::BlockFrames;
  const float fAllowedFadeOutStep = AllowedStep(uiFadeOutBegin, uiFadeOutEnd);
  const float fFadeOutStep = plAudioTestUtils::MaxStep(output, uiFadeOutBegin, uiFadeOutEnd);

  // nothing plays while the voice is virtual (once the fade-out and the filter tail are over), and the realized voice
  // stays silent up to its resume frame; the frame the output bus holds back from the faded voice is silent
  const plUInt32 uiSilentFrom = uiVirtualizedAt + 3 * plOfflineAudioEngine::BlockFrames;
  const plUInt32 uiOnset = plAudioTestUtils::FindFirstAbove(output, SilenceThreshold, uiSilentFrom);
  const plUInt32 uiExpectedOnset = static_cast<plUInt32>(uiResumeFrame) + uiLatency;

  // The realized voice fades in over uiFadeFrames from its resume frame, so its steps stay as small as the fade-out's.
  // After the fade the voice's filters settle; from then on the output is the uninterrupted timeline, sample for sample.
  constexpr plUInt32 uiSettleFrames = 200;
  float fFadeInStep = 0.0f;
  float fAllowedFadeInStep = 0.0f;
  float fSettledDiff = 0.0f;

  if (uiOnset != plInvalidIndex && uiOnset + uiFadeFrames + uiSettleFrames < output.GetCount())
  {
    fFadeInStep = plAudioTestUtils::MaxStep(output, uiRealizedAt, uiOnset + uiFadeFrames + uiSettleFrames);
    fAllowedFadeInStep = AllowedStep(uiOnset, uiOnset + uiFadeFrames + uiSettleFrames);
    for (plUInt32 i = uiOnset + uiFadeFrames + uiSettleFrames; i < output.GetCount(); ++i)
      fSettledDiff = plMath::Max(fSettledDiff, plMath::Abs(output[i] - reference[i]));
  }

  plStringBuilder sMsg;
  sMsg.SetFormat("Voices/ResumePosition: latency {}, resume frame {}, onset {} (expected {}), fade {} frames, fade-out step {} (allowed {}), fade-in step {} (allowed {}), max diff after fade + {} frames {}", uiLatency, uiResumeFrame, uiOnset, uiExpectedOnset, uiFadeFrames,
    fFadeOutStep, fAllowedFadeOutStep, fFadeInStep, fAllowedFadeInStep, uiSettleFrames, fSettledDiff);
  Report(sMsg);

  PL_TEST_BOOL_MSG(fFadeOutStep <= fAllowedFadeOutStep, "%s", sMsg.GetData());
  PL_TEST_BOOL_MSG(fFadeInStep <= fAllowedFadeInStep, "%s", sMsg.GetData());
  PL_TEST_INT(uiOnset, uiExpectedOnset);
  PL_TEST_BOOL_MSG(fSettledDiff < 1e-5f, "%s", sMsg.GetData());
}

PL_CREATE_SIMPLE_TEST(Voices, StartTimeCulling)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Embedded sample")
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoiceCull", MakeVoiceEvent(plAudioTestUtils::MakeDc(4800, 0.5f), true, true, 128));

    plPlasmaAudioEventManager manager;
    Emitter emitter;
    emitter.m_Event.m_bLooping = true;
    emitter.SetPosition(plVec3(200.0f, 0.0f, 0.0f));

    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceCull", &emitter.m_Entity, &emitter.m_Event, engine.GetBusGraph(), engine.GetEngine()));
    PL_TEST_INT(manager.GetInstanceCount(), 1);
    PL_TEST_INT(GetRealCount(manager, emitter), 0);
    PL_TEST_INT(GetVirtualCount(manager, emitter), 1);

    engine.RenderBlocks(5, &manager, nullptr);
    PL_TEST_BOOL(manager.IsEventPlaying(&emitter.m_Event));
    PL_TEST_INT(GetRealCount(manager, emitter), 0);
    PL_TEST_INT(GetVirtualCount(manager, emitter), 1);

    emitter.SetPosition(plVec3(10.0f, 0.0f, 0.0f));
    plDynamicArray<float> output;
    engine.RenderBlocks(5, &manager, &output);
    PL_TEST_INT(GetRealCount(manager, emitter), 1);
    PL_TEST_INT(GetVirtualCount(manager, emitter), 0);
    PL_TEST_BOOL(plAudioTestUtils::FindFirstAbove(output, SilenceThreshold) != plInvalidIndex);

    manager.StopAll(true);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Streamed clip")
  {
    if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(">pltest/", "PlasmaAudioVoiceTest", "voicetest", plDataDirUsage::AllowWrites)))
      return;

    PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioVoiceTest"));

    plAudioStreamService streams;
    streams.Startup();
    PL_SCOPE_EXIT(streams.Shutdown());

    const plStringView sClipPath = ":voicetest/StreamedCull.plBinAudioClip";
    if (!PL_TEST_RESULT(WriteBuiltClip(sClipPath, plAudioTestUtils::MakeDc(2 * plOfflineAudioEngine::SampleRate, 0.5f), plAudioClipCodec::Pcm16, plAudioClipLoadMode::Streamed)))
      return;

    plAudioClipResourceHandle hClip = plResourceManager::LoadResource<plAudioClipResource>(sClipPath);
    {
      plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
      if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
        return;
    }

    plAudioEventResourceDescriptor res = MakeVoiceEvent(plDynamicArray<float>(), true, true, 128);
    res.m_SampleData.Clear();
    res.m_Desc.m_SampleNames.Clear();
    res.m_Desc.m_Instruments[0].m_Playlist[0].m_uiSample = plAudioEventSilenceSample;
    res.m_Desc.m_Instruments[0].m_Playlist[0].m_sClip = sClipPath;
    plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoiceCullStreamed", std::move(res));

    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    plPlasmaAudioEventManager manager;
    manager.SetStreamService(&streams);

    Emitter emitter;
    emitter.m_Event.m_bLooping = true;
    emitter.SetPosition(plVec3(200.0f, 0.0f, 0.0f));

    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceCullStreamed", &emitter.m_Entity, &emitter.m_Event, engine.GetBusGraph(), engine.GetEngine()));

    for (plUInt32 i = 0; i < 10; ++i)
    {
      streams.WaitForIdle();
      engine.RenderBlocks(1, &manager, nullptr);
    }

    PL_TEST_INT(GetRealCount(manager, emitter), 0);
    PL_TEST_INT(GetVirtualCount(manager, emitter), 1);
    PL_TEST_INT(streams.GetActiveStreamCount(), 0);

    emitter.SetPosition(plVec3(10.0f, 0.0f, 0.0f));
    plDynamicArray<float> output;
    for (plUInt32 i = 0; i < 20; ++i)
    {
      streams.WaitForIdle();
      engine.RenderBlocks(1, &manager, &output);
    }

    PL_TEST_INT(GetRealCount(manager, emitter), 1);
    PL_TEST_INT(streams.GetActiveStreamCount(), 1);
    PL_TEST_BOOL(plAudioTestUtils::FindFirstAbove(output, SilenceThreshold) != plInvalidIndex);

    // beyond the max distance again: the voice goes virtual after its dwell time, fades out and then releases its stream
    emitter.SetPosition(plVec3(200.0f, 0.0f, 0.0f));
    for (plUInt32 i = 0; i < 30; ++i)
    {
      streams.WaitForIdle();
      engine.RenderBlocks(1, &manager, nullptr);
    }

    PL_TEST_INT(GetRealCount(manager, emitter), 0);
    PL_TEST_INT(GetVirtualCount(manager, emitter), 1);
    PL_TEST_INT(streams.GetActiveStreamCount(), 0);

    manager.StopAll(true);
    PL_TEST_INT(streams.GetActiveStreamCount(), 0);
  }
}

PL_CREATE_SIMPLE_TEST(Voices, Hysteresis)
{
  BudgetScope budgets(1, 512);

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoiceHysteresis", MakeVoiceEvent(plAudioTestUtils::MakeDc(4800, 0.25f), true, false, 128));

  plPlasmaAudioEventManager manager;
  Emitter emitters[2];
  for (Emitter& emitter : emitters)
  {
    emitter.m_Event.m_bLooping = true;
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceHysteresis", &emitter.m_Entity, &emitter.m_Event, engine.GetBusGraph(), engine.GetEngine()));
  }

  const plUInt64 uiFlipsBefore = manager.GetVoiceFlipCount();

  // the second emitter's loudness swings around the first one's by +/- 10 % every block
  for (plUInt32 uiBlock = 0; uiBlock < 60; ++uiBlock)
  {
    emitters[1].m_Entity.m_fVolumeMultiplier = (uiBlock % 2 == 0) ? 1.1f : 0.9f;
    engine.RenderBlocks(1, &manager, nullptr);
    PL_TEST_INT(manager.GetRealVoiceCount(), 1);
  }

  const plUInt64 uiFlips = manager.GetVoiceFlipCount() - uiFlipsBefore;

  plStringBuilder sMsg;
  sMsg.SetFormat("Voices/Hysteresis: {} flips over 60 blocks", uiFlips);
  Report(sMsg);

  PL_TEST_BOOL(uiFlips <= 2);
}

PL_CREATE_SIMPLE_TEST(Voices, PriorityBeatsAudibility)
{
  BudgetScope budgets(1, 512);

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoicePrio10", MakeVoiceEvent(plAudioTestUtils::MakeDc(4800, 0.25f), true, false, 10));
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoicePrio200", MakeVoiceEvent(plAudioTestUtils::MakeDc(4800, 0.25f), true, false, 200));

  plPlasmaAudioEventManager manager;
  Emitter loud;
  Emitter quiet;
  loud.m_Event.m_bLooping = true;
  quiet.m_Event.m_bLooping = true;
  quiet.m_Entity.m_fVolumeMultiplier = 0.05f;

  PL_TEST_BLOCK(plTestBlock::Enabled, "Admission")
  {
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoicePrio10", &loud.m_Entity, &loud.m_Event, engine.GetBusGraph(), engine.GetEngine()));
    engine.RenderBlocks(1, &manager, nullptr);
    PL_TEST_INT(GetRealCount(manager, loud), 1);

    // the new voice outranks the only real voice and takes its place right away
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoicePrio200", &quiet.m_Entity, &quiet.m_Event, engine.GetBusGraph(), engine.GetEngine()));
    PL_TEST_INT(GetRealCount(manager, quiet), 1);
    PL_TEST_INT(GetRealCount(manager, loud), 0);
    PL_TEST_INT(GetVirtualCount(manager, loud), 1);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Ranking")
  {
    engine.RenderBlocks(30, &manager, nullptr);
    PL_TEST_INT(GetRealCount(manager, quiet), 1);
    PL_TEST_INT(GetRealCount(manager, loud), 0);
    PL_TEST_INT(GetVirtualCount(manager, loud), 1);
  }

  manager.StopAll(true);
}

PL_CREATE_SIMPLE_TEST(Voices, StealingWithBudget)
{
  BudgetScope budgets(1, 512);

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(4800, 0.25f));
  res.m_Desc.m_uiMaxInstances = 2;
  res.m_Desc.m_eStealing = plAudioEventStealingMode::Oldest;
  res.m_Desc.m_StopFadeOut = plTime::MakeZero();
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoiceSteal", std::move(res));

  Emitter emitters[3];
  plPlasmaAudioEventManager manager;

  for (plUInt32 i = 0; i < 3; ++i)
  {
    emitters[i].m_Event.m_bLooping = true; // held open until stopped, so only stealing ends an instance
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceSteal", &emitters[i].m_Entity, &emitters[i].m_Event, engine.GetBusGraph(), engine.GetEngine()));
    engine.RenderBlocks(1, &manager, nullptr);
    PL_TEST_BOOL(manager.GetRealVoiceCount() <= 1);
  }

  PL_TEST_INT(manager.GetInstanceCount(), 2);
  PL_TEST_BOOL(!manager.IsEventPlaying(&emitters[0].m_Event));
  PL_TEST_BOOL(manager.IsEventPlaying(&emitters[1].m_Event));
  PL_TEST_BOOL(manager.IsEventPlaying(&emitters[2].m_Event));
}

PL_CREATE_SIMPLE_TEST(Voices, VirtualKillMode)
{
  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plAudioEventResourceDescriptor res = MakeVoiceEvent(plAudioTestUtils::MakeDc(4800, 0.5f), false, true, 128);
  res.m_Desc.m_eVirtualMode = plAudioEventVirtualMode::Kill;
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoiceKill", std::move(res));

  plPlasmaAudioEventManager manager;
  Emitter emitter;
  emitter.SetPosition(plVec3(200.0f, 0.0f, 0.0f));

  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceKill", &emitter.m_Entity, &emitter.m_Event, engine.GetBusGraph(), engine.GetEngine()));
  PL_TEST_INT(manager.GetRealVoiceCount(), 0);
  PL_TEST_INT(manager.GetVirtualVoiceCount(), 0);

  engine.RenderBlocks(1, &manager, nullptr);
  PL_TEST_INT(manager.GetInstanceCount(), 0);
  PL_TEST_BOOL(!manager.IsEventPlaying(&emitter.m_Event));
}

PL_CREATE_SIMPLE_TEST(Voices, FadesAfterMixedChunk)
{
  // A device thread may mix the chunk that begins at the engine time the audio thread reads before that thread sets a
  // fade. The offline engine simulates this by moving its time back one block while the manager works and forward
  // again before it renders, so the manager sees the time of a chunk that was mixed already.
  constexpr float fLevel = 0.5f;
  const plUInt32 uiFadeFrames = static_cast<plUInt32>(plPlasmaAudioEventInstance::VirtualFadeDuration.GetSeconds() * plOfflineAudioEngine::SampleRate + 0.5);

  // a linear fade of fLevel over uiFadeFrames, with some room for the voice's filters
  const float fAllowedStep = 1.5f * fLevel / static_cast<float>(uiFadeFrames);

  PL_TEST_BLOCK(plTestBlock::Enabled, "Virtualization fade-out")
  {
    BudgetScope budgets(64, 512);

    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoiceFadeOutLate", MakeVoiceEvent(plAudioTestUtils::MakeDc(4800, fLevel), true, false, 128));

    plPlasmaAudioEventManager manager;
    Emitter emitter;
    emitter.m_Event.m_bLooping = true;

    plDynamicArray<float> output;
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceFadeOutLate", &emitter.m_Entity, &emitter.m_Event, engine.GetBusGraph(), engine.GetEngine()));
    engine.RenderBlocks(5, &manager, &output);
    PL_TEST_INT(GetRealCount(manager, emitter), 1);

    const plUInt32 uiVirtualizedAt = output.GetCount();

    plAudioVoiceManager::SetBudgets(0, 512);
    const ma_uint64 uiTime = engine.GetTime();
    ma_engine_set_time_in_pcm_frames(engine.GetEngine(), uiTime - plOfflineAudioEngine::BlockFrames);
    manager.Update(plOfflineAudioEngine::GetBlockDuration(), plVec3::MakeZero());
    ma_engine_set_time_in_pcm_frames(engine.GetEngine(), uiTime);

    PL_TEST_INT(GetRealCount(manager, emitter), 0);
    PL_TEST_INT(GetVirtualCount(manager, emitter), 1);

    engine.RenderBlocks(10, &manager, &output);

    const float fStep = plAudioTestUtils::MaxStep(output, uiVirtualizedAt - plOfflineAudioEngine::BlockFrames, output.GetCount());
    const plUInt32 uiSoundAfter = plAudioTestUtils::FindFirstAbove(output, SilenceThreshold, uiVirtualizedAt + uiFadeFrames + plOfflineAudioEngine::BlockFrames);

    plStringBuilder sMsg;
    sMsg.SetFormat("Voices/FadesAfterMixedChunk: fade-out step {} (allowed {}), level before {}", fStep, fAllowedStep, output[uiVirtualizedAt - 1]);
    Report(sMsg);

    PL_TEST_BOOL(plMath::Abs(output[uiVirtualizedAt - 1]) > 0.9f * fLevel);
    PL_TEST_BOOL_MSG(fStep <= fAllowedStep, "%s", sMsg.GetData());
    PL_TEST_BOOL(uiSoundAfter == plInvalidIndex);

    manager.StopAll(true);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Start fade-in")
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    plAudioEventResourceDescriptor res = MakeVoiceEvent(plAudioTestUtils::MakeDc(4800, fLevel), false, false, 128);
    res.m_Desc.m_FadeIn = plPlasmaAudioEventInstance::VirtualFadeDuration;
    plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoiceFadeInLate", std::move(res));

    plPlasmaAudioEventManager manager;
    Emitter emitter;

    plDynamicArray<float> output;
    engine.RenderBlocks(2, &manager, &output);

    const plUInt32 uiStartedAt = output.GetCount();

    const ma_uint64 uiTime = engine.GetTime();
    ma_engine_set_time_in_pcm_frames(engine.GetEngine(), uiTime - plOfflineAudioEngine::BlockFrames);
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceFadeInLate", &emitter.m_Entity, &emitter.m_Event, engine.GetBusGraph(), engine.GetEngine()));
    ma_engine_set_time_in_pcm_frames(engine.GetEngine(), uiTime);

    engine.RenderBlocks(5, &manager, &output);

    const float fStep = plAudioTestUtils::MaxStep(output, uiStartedAt, output.GetCount());
    const float fPeak = plMath::Abs(output[output.GetCount() - 1]);

    plStringBuilder sMsg;
    sMsg.SetFormat("Voices/FadesAfterMixedChunk: fade-in step {} (allowed {}), level after {}", fStep, fAllowedStep, fPeak);
    Report(sMsg);

    PL_TEST_BOOL(fPeak > 0.9f * fLevel);
    PL_TEST_BOOL_MSG(fStep <= fAllowedStep, "%s", sMsg.GetData());

    manager.StopAll(true);
  }
}

PL_CREATE_SIMPLE_TEST(Voices, AdmissionDuringUpdate)
{
  BudgetScope budgets(1, 512);

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoiceAdmitLow", MakeVoiceEvent(plAudioTestUtils::MakeDc(4800, 0.25f), true, false, 10));

  // the instrument starts well behind the look-ahead, so it triggers inside a later Update
  plAudioEventResourceDescriptor res = MakeVoiceEvent(plAudioTestUtils::MakeDc(4800, 0.25f), false, false, 200);
  res.m_Desc.m_Instruments[0].m_StartDelay = plTime::MakeFromMilliseconds(100);
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoiceAdmitDelayed", std::move(res));

  plPlasmaAudioEventManager manager;
  Emitter low;
  Emitter high;
  low.m_Event.m_bLooping = true;

  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceAdmitLow", &low.m_Entity, &low.m_Event, engine.GetBusGraph(), engine.GetEngine()));
  engine.RenderBlocks(2, &manager, nullptr);
  PL_TEST_INT(GetRealCount(manager, low), 1);

  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceAdmitDelayed", &high.m_Entity, &high.m_Event, engine.GetBusGraph(), engine.GetEngine()));
  PL_TEST_INT(GetRealCount(manager, high) + GetVirtualCount(manager, high), 0);

  const plUInt64 uiFlipsBefore = manager.GetVoiceFlipCount();

  bool bTriggered = false;
  for (plUInt32 uiBlock = 0; uiBlock < 20 && !bTriggered; ++uiBlock)
  {
    engine.RenderBlocks(1, &manager, nullptr);
    bTriggered = GetRealCount(manager, high) + GetVirtualCount(manager, high) > 0;
  }

  if (!PL_TEST_BOOL(bTriggered))
    return;

  // admitted real when it started inside the instance updates, by virtualizing the low-priority voice; the voice
  // ranking of the same Update then has nothing left to change
  PL_TEST_INT(GetRealCount(manager, high), 1);
  PL_TEST_INT(GetRealCount(manager, low), 0);
  PL_TEST_INT(GetVirtualCount(manager, low), 1);
  PL_TEST_INT(manager.GetVoiceFlipCount() - uiFlipsBefore, 1);
  PL_TEST_INT(manager.GetRealVoiceCount(), 1);

  manager.StopAll(true);
}

PL_CREATE_SIMPLE_TEST(Voices, KillOnAdmission)
{
  BudgetScope budgets(1, 512);

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoiceKillHold", MakeVoiceEvent(plAudioTestUtils::MakeDc(4800, 0.25f), true, false, 200));

  // two instruments: when the first one is not admitted, the second one (still pending) must not play either
  plAudioEventResourceDescriptor res = MakeVoiceEvent(plAudioTestUtils::MakeDc(4800, 0.5f), false, false, 10);
  res.m_Desc.m_eVirtualMode = plAudioEventVirtualMode::Kill;
  res.m_Desc.m_Instruments.PushBack(res.m_Desc.m_Instruments[0]);
  res.m_Desc.m_Instruments[1].m_StartDelay = plTime::MakeFromSeconds(1.0);
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoiceKillAdmission", std::move(res));

  plPlasmaAudioEventManager manager;
  Emitter hold;
  Emitter killed;
  hold.m_Event.m_bLooping = true;

  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceKillHold", &hold.m_Entity, &hold.m_Event, engine.GetBusGraph(), engine.GetEngine()));
  engine.RenderBlocks(1, &manager, nullptr);
  PL_TEST_INT(GetRealCount(manager, hold), 1);

  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceKillAdmission", &killed.m_Entity, &killed.m_Event, engine.GetBusGraph(), engine.GetEngine()));
  PL_TEST_INT(GetRealCount(manager, killed) + GetVirtualCount(manager, killed), 0);

  engine.RenderBlocks(1, &manager, nullptr);
  PL_TEST_BOOL(!manager.IsEventPlaying(&killed.m_Event));
  PL_TEST_INT(manager.GetInstanceCount(), 1);
  PL_TEST_INT(GetRealCount(manager, hold), 1);

  manager.StopAll(true);
}

PL_CREATE_SIMPLE_TEST(Voices, StreamLimitSwap)
{
  /// counts the stream limit warnings
  struct StreamLimitLog : public plLogInterface
  {
    plUInt32 m_uiCount = 0;
    void HandleLogMessage(const plLoggingEventData& le) override
    {
      if (le.m_EventType == plLogMsgType::WarningMsg && le.m_sText.FindSubString("PlasmaAudio.MaxStreams") != nullptr)
        ++m_uiCount;
    }
  };

  BudgetScope budgets(1, 512);

  plCVarInt* pMaxStreams = static_cast<plCVarInt*>(plCVar::FindCVarByName("PlasmaAudio.MaxStreams"));
  if (!PL_TEST_BOOL(pMaxStreams != nullptr))
    return;

  const int iOldMax = pMaxStreams->GetValue();
  *pMaxStreams = 1;
  PL_SCOPE_EXIT(*pMaxStreams = iOldMax);

  if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(">pltest/", "PlasmaAudioVoiceTest", "voicetest", plDataDirUsage::AllowWrites)))
    return;

  PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioVoiceTest"));

  plAudioStreamService streams;
  streams.Startup();
  PL_SCOPE_EXIT(streams.Shutdown());

  const plStringView sClipPath = ":voicetest/StreamedSwap.plBinAudioClip";
  if (!PL_TEST_RESULT(WriteBuiltClip(sClipPath, plAudioTestUtils::MakeDc(2 * plOfflineAudioEngine::SampleRate, 0.5f), plAudioClipCodec::Pcm16, plAudioClipLoadMode::Streamed)))
    return;

  plAudioClipResourceHandle hClip = plResourceManager::LoadResource<plAudioClipResource>(sClipPath);
  {
    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      return;
  }

  plAudioEventResourceDescriptor res = MakeVoiceEvent(plDynamicArray<float>(), true, true, 128);
  res.m_SampleData.Clear();
  res.m_Desc.m_SampleNames.Clear();
  res.m_Desc.m_Instruments[0].m_Playlist[0].m_uiSample = plAudioEventSilenceSample;
  res.m_Desc.m_Instruments[0].m_Playlist[0].m_sClip = sClipPath;
  plAudioTestUtils::CreateEventResource("PlasmaAudioTest:VoiceStreamSwap", std::move(res));

  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  plPlasmaAudioEventManager manager;
  manager.SetStreamService(&streams);

  Emitter a;
  Emitter b;
  a.m_Event.m_bLooping = true;
  b.m_Event.m_bLooping = true;
  a.SetPosition(plVec3(5.0f, 0.0f, 0.0f));
  b.SetPosition(plVec3(20.0f, 0.0f, 0.0f));

  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceStreamSwap", &a.m_Entity, &a.m_Event, engine.GetBusGraph(), engine.GetEngine()));
  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:VoiceStreamSwap", &b.m_Entity, &b.m_Event, engine.GetBusGraph(), engine.GetEngine()));

  for (plUInt32 i = 0; i < 5; ++i)
  {
    streams.WaitForIdle();
    engine.RenderBlocks(1, &manager, nullptr);
  }

  PL_TEST_INT(GetRealCount(manager, a), 1);
  PL_TEST_INT(GetVirtualCount(manager, b), 1);
  PL_TEST_INT(streams.GetActiveStreamCount(), 1);

  // b becomes the louder voice: a goes virtual and holds the only stream through its fade-out, b waits for it
  a.SetPosition(plVec3(20.0f, 0.0f, 0.0f));
  b.SetPosition(plVec3(5.0f, 0.0f, 0.0f));

  StreamLimitLog log;
  plUInt32 uiRealizedAfter = plInvalidIndex;
  {
    plLogSystemScope logScope(&log);

    for (plUInt32 i = 0; i < 20; ++i)
    {
      streams.WaitForIdle();
      engine.RenderBlocks(1, &manager, nullptr);

      if (uiRealizedAfter == plInvalidIndex && GetRealCount(manager, b) == 1)
        uiRealizedAfter = i + 1;
    }
  }

  plStringBuilder sMsg;
  sMsg.SetFormat("Voices/StreamLimitSwap: b realized after {} blocks, {} stream limit warnings", uiRealizedAfter, log.m_uiCount);
  Report(sMsg);

  // the swap waits for a's fade-out and teardown (one look-ahead span plus the fade) and then completes
  PL_TEST_BOOL(uiRealizedAfter != plInvalidIndex && uiRealizedAfter <= 6);
  PL_TEST_INT(GetRealCount(manager, b), 1);
  PL_TEST_INT(GetRealCount(manager, a), 0);
  PL_TEST_INT(GetVirtualCount(manager, a), 1);
  PL_TEST_INT(streams.GetActiveStreamCount(), 1);
  PL_TEST_INT(log.m_uiCount, 0);

  manager.StopAll(true);
  PL_TEST_INT(streams.GetActiveStreamCount(), 0);
}
