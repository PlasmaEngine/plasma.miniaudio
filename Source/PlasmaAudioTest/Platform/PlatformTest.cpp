#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <Foundation/IO/ChunkStream.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <PlasmaAudioPlugin/Codecs/AudioCodecBenchmark.h>
#include <PlasmaAudioPlugin/Platform/AudioBackends.h>
#include <PlasmaAudioPlugin/Platform/AudioPlatformConfig.h>
#include <PlasmaAudioPlugin/Platform/AudioSuspend.h>

#include <atomic>

PL_CREATE_SIMPLE_TEST_GROUP(Platform);

namespace
{
  void Report(plStringView sText)
  {
    plStringBuilder sTmp = sText;
    plTestFramework::Output(plTestOutput::Message, "%s", sTmp.GetData());
  }

  /// Updates the controller in 10 ms steps for duration and returns the last result.
  plAudioSuspendController::Result Run(plAudioSuspendController& ref_controller, bool bSuspend, plTime duration, plAudioSuspendPolicy::Enum policy, plTime fade, bool* out_pStopAll = nullptr)
  {
    plAudioSuspendController::Result result;
    for (plTime t; t < duration; t += plTime::MakeFromMilliseconds(10))
    {
      result = ref_controller.Update(bSuspend, plTime::MakeFromMilliseconds(10), policy, fade);
      if (out_pStopAll != nullptr && result.m_bStopAllEvents)
        *out_pStopAll = true;
    }
    return result;
  }

  /// A custom backend whose device "plays" by counting the frames miniaudio writes to it.
  class TestBackend : public plAudioCustomBackend
  {
  public:
    plStringView GetName() const override { return "Test Backend"; }

    void GetCallbacks(ma_backend_callbacks& out_callbacks) const override
    {
      out_callbacks = {};
      out_callbacks.onContextInit = &OnContextInit;
    }

    static inline std::atomic<plUInt64> s_uiFramesWritten = 0;
    static inline std::atomic<plUInt32> s_uiStarts = 0;

  private:
    static ma_result OnContextInit(ma_context* pContext, const ma_context_config* pConfig, ma_backend_callbacks* pCallbacks)
    {
      PL_IGNORE_UNUSED(pContext);
      PL_IGNORE_UNUSED(pConfig);

      pCallbacks->onContextUninit = [](ma_context*) -> ma_result
      { return MA_SUCCESS; };
      pCallbacks->onContextEnumerateDevices = &OnEnumerate;
      pCallbacks->onContextGetDeviceInfo = &OnGetDeviceInfo;
      pCallbacks->onDeviceInit = &OnDeviceInit;
      pCallbacks->onDeviceUninit = [](ma_device*) -> ma_result
      { return MA_SUCCESS; };
      pCallbacks->onDeviceStart = [](ma_device*) -> ma_result
      {
        ++s_uiStarts;
        return MA_SUCCESS;
      };
      pCallbacks->onDeviceStop = [](ma_device*) -> ma_result
      { return MA_SUCCESS; };
      pCallbacks->onDeviceWrite = &OnDeviceWrite;
      return MA_SUCCESS;
    }

    static ma_result OnEnumerate(ma_context* pContext, ma_enum_devices_callback_proc callback, void* pUserData)
    {
      ma_device_info info = {};
      plStringUtils::Copy(info.name, sizeof(info.name), "Test Output");
      info.isDefault = MA_TRUE;
      callback(pContext, ma_device_type_playback, &info, pUserData);
      return MA_SUCCESS;
    }

    static ma_result OnGetDeviceInfo(ma_context* pContext, ma_device_type deviceType, const ma_device_id* pDeviceID, ma_device_info* pDeviceInfo)
    {
      PL_IGNORE_UNUSED(pContext);
      PL_IGNORE_UNUSED(deviceType);
      PL_IGNORE_UNUSED(pDeviceID);
      plStringUtils::Copy(pDeviceInfo->name, sizeof(pDeviceInfo->name), "Test Output");
      pDeviceInfo->isDefault = MA_TRUE;
      pDeviceInfo->nativeDataFormatCount = 1;
      pDeviceInfo->nativeDataFormats[0].format = ma_format_f32;
      pDeviceInfo->nativeDataFormats[0].channels = 2;
      pDeviceInfo->nativeDataFormats[0].sampleRate = 48000;
      return MA_SUCCESS;
    }

    static ma_result OnDeviceInit(ma_device* pDevice, const ma_device_config* pConfig, ma_device_descriptor* pDescriptorPlayback, ma_device_descriptor* pDescriptorCapture)
    {
      PL_IGNORE_UNUSED(pDevice);
      PL_IGNORE_UNUSED(pConfig);
      PL_IGNORE_UNUSED(pDescriptorCapture);
      pDescriptorPlayback->format = ma_format_f32;
      pDescriptorPlayback->channels = 2;
      pDescriptorPlayback->sampleRate = 48000;
      ma_channel_map_init_standard(ma_standard_channel_map_default, pDescriptorPlayback->channelMap, MA_MAX_CHANNELS, 2);
      pDescriptorPlayback->periodSizeInFrames = 480;
      pDescriptorPlayback->periodCount = 2;
      return MA_SUCCESS;
    }

    static ma_result OnDeviceWrite(ma_device* pDevice, const void* pFrames, ma_uint32 uiFrameCount, ma_uint32* out_pFramesWritten)
    {
      PL_IGNORE_UNUSED(pDevice);
      PL_IGNORE_UNUSED(pFrames);

      // paced roughly like a device, so the worker thread does not spin
      s_uiFramesWritten += uiFrameCount;
      plThreadUtils::Sleep(plTime::MakeFromMilliseconds(5));
      *out_pFramesWritten = uiFrameCount;
      return MA_SUCCESS;
    }
  };
} // namespace

PL_CREATE_SIMPLE_TEST(Platform, SuspendController)
{
  const plTime fade = plTime::MakeFromMilliseconds(100);

  PL_TEST_BLOCK(plTestBlock::Enabled, "Pause: fade out, stop, resume with a fade in")
  {
    plAudioSuspendController controller;

    // half way through the fade
    plAudioSuspendController::Result result = Run(controller, true, plTime::MakeFromMilliseconds(50), plAudioSuspendPolicy::Pause, fade);
    PL_TEST_INT(controller.GetState(), plAudioOutputState::FadingOut);
    PL_TEST_FLOAT(result.m_fGain, 0.5f, 0.01f);
    PL_TEST_BOOL(result.m_bDeviceRunning);

    result = Run(controller, true, plTime::MakeFromMilliseconds(60), plAudioSuspendPolicy::Pause, fade);
    PL_TEST_INT(controller.GetState(), plAudioOutputState::Suspended);
    PL_TEST_FLOAT(result.m_fGain, 0.0f, 0.0f);
    PL_TEST_BOOL(!result.m_bDeviceRunning);

    // stays suspended while asked to
    result = Run(controller, true, plTime::MakeFromSeconds(1.0), plAudioSuspendPolicy::Pause, fade);
    PL_TEST_INT(controller.GetState(), plAudioOutputState::Suspended);

    // the resume starts the device at once and fades in
    result = controller.Update(false, plTime::MakeFromMilliseconds(10), plAudioSuspendPolicy::Pause, fade);
    PL_TEST_INT(controller.GetState(), plAudioOutputState::Resuming);
    PL_TEST_BOOL(result.m_bDeviceRunning);
    PL_TEST_BOOL(result.m_bStateChanged);
    PL_TEST_FLOAT(result.m_fGain, 0.1f, 0.01f);

    result = Run(controller, false, plTime::MakeFromMilliseconds(100), plAudioSuspendPolicy::Pause, fade);
    PL_TEST_INT(controller.GetState(), plAudioOutputState::Running);
    PL_TEST_FLOAT(result.m_fGain, 1.0f, 0.0f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "A resume during the fade out fades back from there")
  {
    plAudioSuspendController controller;
    Run(controller, true, plTime::MakeFromMilliseconds(30), plAudioSuspendPolicy::Pause, fade);
    PL_TEST_FLOAT(controller.GetGain(), 0.7f, 0.01f);

    const plAudioSuspendController::Result result = controller.Update(false, plTime::MakeFromMilliseconds(10), plAudioSuspendPolicy::Pause, fade);
    PL_TEST_INT(controller.GetState(), plAudioOutputState::Resuming);
    PL_TEST_FLOAT(result.m_fGain, 0.8f, 0.01f);
    PL_TEST_BOOL(result.m_bDeviceRunning);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Keep playing")
  {
    plAudioSuspendController controller;
    plAudioSuspendController::Result result = Run(controller, true, plTime::MakeFromMilliseconds(300), plAudioSuspendPolicy::KeepPlaying, fade);
    PL_TEST_INT(controller.GetState(), plAudioOutputState::Background);
    PL_TEST_FLOAT(result.m_fGain, 1.0f, 0.0f);
    PL_TEST_BOOL(result.m_bDeviceRunning);

    result = controller.Update(false, plTime::MakeFromMilliseconds(10), plAudioSuspendPolicy::KeepPlaying, fade);
    PL_TEST_INT(controller.GetState(), plAudioOutputState::Running);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Stop all: once, when the fade ended")
  {
    plAudioSuspendController controller;
    bool bStopAll = false;
    Run(controller, true, plTime::MakeFromMilliseconds(90), plAudioSuspendPolicy::StopAll, fade, &bStopAll);
    PL_TEST_BOOL(!bStopAll);

    Run(controller, true, plTime::MakeFromMilliseconds(20), plAudioSuspendPolicy::StopAll, fade, &bStopAll);
    PL_TEST_BOOL(bStopAll);
    PL_TEST_INT(controller.GetState(), plAudioOutputState::Suspended);

    bStopAll = false;
    Run(controller, true, plTime::MakeFromMilliseconds(200), plAudioSuspendPolicy::StopAll, fade, &bStopAll);
    PL_TEST_BOOL(!bStopAll);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "No fade: in one update")
  {
    plAudioSuspendController controller;
    plAudioSuspendController::Result result = controller.Update(true, plTime::MakeFromMilliseconds(10), plAudioSuspendPolicy::Pause, plTime::MakeZero());
    PL_TEST_INT(controller.GetState(), plAudioOutputState::Suspended);
    PL_TEST_BOOL(!result.m_bDeviceRunning);

    result = controller.Update(false, plTime::MakeFromMilliseconds(10), plAudioSuspendPolicy::Pause, plTime::MakeZero());
    PL_TEST_INT(controller.GetState(), plAudioOutputState::Running);
  }
}

PL_CREATE_SIMPLE_TEST(Platform, Backends)
{
  plDynamicArray<plString> names;
  plAudioBackendRegistry::GetBackendNames(names);

  plStringBuilder sNames;
  for (const plString& sName : names)
    sNames.AppendWithSeparator(", ", sName);
  Report(plStringBuilder("Backends: ", sNames));

#if PL_ENABLED(PL_PLATFORM_WINDOWS)
  PL_TEST_BOOL(names.Contains("WASAPI"));
  PL_TEST_BOOL(names.Contains("DirectSound"));
  PL_TEST_BOOL(names.Contains("WinMM"));
#endif
  PL_TEST_BOOL(names.Contains("Null"));
  PL_TEST_BOOL(!names.Contains("Custom"));

  PL_TEST_BOOL(!plAudioBackendRegistry::Resolve("").m_bFound);
  PL_TEST_BOOL(!plAudioBackendRegistry::Resolve("No Such Backend").m_bFound);
  PL_TEST_BOOL(plAudioBackendRegistry::Resolve("null").m_Backend == ma_backend_null);

  TestBackend backend;

  PL_TEST_BLOCK(plTestBlock::Enabled, "A registered custom backend")
  {
    plAudioBackendRegistry::RegisterCustomBackend(&backend);

    plAudioBackendRegistry::GetBackendNames(names);
    PL_TEST_BOOL(names.Contains("Test Backend"));

    const plAudioBackendRegistry::Choice choice = plAudioBackendRegistry::Resolve("test backend");
    PL_TEST_BOOL(choice.m_bFound);
    PL_TEST_BOOL(choice.m_Backend == ma_backend_custom);
    PL_TEST_BOOL(choice.m_pCustom == &backend);

    // a context and an engine on it: miniaudio drives the backend's device
    ma_context_config contextConfig = ma_context_config_init();
    backend.GetCallbacks(contextConfig.custom);

    const ma_backend backends[] = {ma_backend_custom};
    ma_context context;
    if (PL_TEST_BOOL(ma_context_init(backends, 1, &contextConfig, &context) == MA_SUCCESS))
    {
      PL_TEST_BOOL(context.backend == ma_backend_custom);

      ma_engine_config engineConfig = ma_engine_config_init();
      engineConfig.pContext = &context;

      ma_engine engine;
      if (PL_TEST_BOOL(ma_engine_init(&engineConfig, &engine) == MA_SUCCESS))
      {
        PL_TEST_STRING(ma_engine_get_device(&engine)->playback.name, "Test Output");
        PL_TEST_INT(ma_engine_get_channels(&engine), 2);
        PL_TEST_INT(ma_engine_get_sample_rate(&engine), 48000);

        // started by ma_engine_init; the writes come from miniaudio's worker thread
        for (plUInt32 i = 0; i < 100 && TestBackend::s_uiFramesWritten < 4800; ++i)
          plThreadUtils::Sleep(plTime::MakeFromMilliseconds(10));

        PL_TEST_INT(TestBackend::s_uiStarts, 1);
        PL_TEST_BOOL(TestBackend::s_uiFramesWritten >= 4800);

        ma_engine_uninit(&engine);
      }

      ma_context_uninit(&context);
    }

    plAudioBackendRegistry::UnregisterCustomBackend(&backend);
    plAudioBackendRegistry::GetBackendNames(names);
    PL_TEST_BOOL(!names.Contains("Test Backend"));
  }
}

PL_CREATE_SIMPLE_TEST(Platform, ConfigRoundTrip)
{
  plPlasmaAudioPlatformConfig config;
  config.m_sBackend = "DirectSound";
  config.m_eSpeakers = plAudioSpeakerLayout::Surround51;
  config.m_uiSampleRate = 44100;
  config.m_uiPeriodMs = 20;
  config.m_bFollowDefaultDevice = false;
  config.m_eSuspendPolicy = plAudioSuspendPolicy::StopAll;
  config.m_fSuspendFadeMs = 250.0f;

  plDefaultMemoryStreamStorage storage;
  {
    plMemoryStreamWriter writer(&storage);
    plChunkStreamWriter chunk(writer);
    chunk.BeginStream(1);
    config.SaveRuntimeData(chunk);
    chunk.EndStream();
  }

  plPlasmaAudioPlatformConfig loaded;
  {
    plMemoryStreamReader reader(&storage);
    plChunkStreamReader chunk(reader);
    chunk.BeginStream();
    while (chunk.GetCurrentChunk().m_bValid)
    {
      loaded.LoadRuntimeData(chunk);
      chunk.NextChunk();
    }
    chunk.EndStream();
  }

  PL_TEST_STRING(loaded.m_sBackend, "DirectSound");
  PL_TEST_INT(loaded.m_eSpeakers, plAudioSpeakerLayout::Surround51);
  PL_TEST_INT(loaded.m_uiSampleRate, 44100);
  PL_TEST_INT(loaded.m_uiPeriodMs, 20);
  PL_TEST_BOOL(!loaded.m_bFollowDefaultDevice);
  PL_TEST_INT(loaded.m_eSuspendPolicy, plAudioSuspendPolicy::StopAll);
  PL_TEST_FLOAT(loaded.m_fSuspendFadeMs, 250.0f, 0.0f);

  PL_TEST_INT(plAudioSpeakerLayout::GetChannelCount(plAudioSpeakerLayout::Device), 0);
  PL_TEST_INT(plAudioSpeakerLayout::GetChannelCount(plAudioSpeakerLayout::Surround71), 8);
}

PL_CREATE_SIMPLE_TEST(Platform, DecodeBenchmark)
{
  plAudioCodecBenchmark::Result result;
  PL_TEST_RESULT(plAudioCodecBenchmark::Run(plTime::MakeFromMilliseconds(250), result));

  plStringBuilder sMsg;
  sMsg.SetFormat("Decode benchmark (voices per 1 % of one core): Opus plain C {}, Opus {} {}, IMA ADPCM {}", plArgF(result.m_fOpusPlainVoices, 1), result.m_sSimd, plArgF(result.m_fOpusSimdVoices, 1), plArgF(result.m_fAdpcmVoices, 1));
  Report(sMsg);

  PL_TEST_BOOL(!result.m_sSimd.IsEmpty());
  PL_TEST_BOOL(result.m_fOpusSimdVoices > 0.0f);
  // the codecs' order depends on what else the machine runs at the time: only that each one was measured
  PL_TEST_BOOL(result.m_fAdpcmVoices > 0.0f);

#if defined(PL_OPUS_HAS_ARCH_CAP)
  PL_TEST_BOOL(result.m_fOpusPlainVoices > 0.0f);
#endif
}
