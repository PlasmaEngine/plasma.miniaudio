#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <PlasmaAudioTest/Harness/OfflineAudio.h>

plOfflineAudioEngine::plOfflineAudioEngine()
{
  ma_engine_config cfg = ma_engine_config_init();
  cfg.noDevice = MA_TRUE;
  cfg.channels = Channels;
  cfg.sampleRate = SampleRate;

  m_bEngineInitialized = ma_engine_init(&cfg, &m_Engine) == MA_SUCCESS;
  m_bValid = m_bEngineInitialized;

  if (m_bValid)
  {
    plAudioMixerDesc mixer;
    mixer.MakeDefault();
    m_bValid = m_BusGraph.Build(mixer, &m_Engine).Succeeded();
  }

  plPlasmaAudioEventManager::SetLookAheadFrames(LookAheadFrames);
}

plOfflineAudioEngine::~plOfflineAudioEngine()
{
  plPlasmaAudioEventManager::SetLookAheadFrames(0);

  m_BusGraph.Teardown();

  if (m_bEngineInitialized)
  {
    ma_engine_uninit(&m_Engine);
  }
}

void plOfflineAudioEngine::Render(ma_uint32 uiFrames, plDynamicArray<float>* pChannel0)
{
  if (!m_bValid)
    return;

  m_Scratch.SetCountUninitialized(uiFrames * Channels);

  ma_uint64 uiRead = 0;
  const ma_result result = ma_engine_read_pcm_frames(&m_Engine, m_Scratch.GetData(), uiFrames, &uiRead);
  PL_TEST_INT(result, MA_SUCCESS);
  PL_TEST_INT(uiRead, uiFrames);

  if (pChannel0 != nullptr)
  {
    for (ma_uint64 i = 0; i < uiFrames; ++i)
    {
      pChannel0->PushBack(i < uiRead ? m_Scratch[static_cast<plUInt32>(i * Channels)] : 0.0f);
    }
  }
}

void plOfflineAudioEngine::RenderBlocks(plUInt32 uiBlocks, plPlasmaAudioEventManager* pManager, plDynamicArray<float>* pChannel0)
{
  for (plUInt32 i = 0; i < uiBlocks; ++i)
  {
    Render(BlockFrames, pChannel0);

    if (pManager != nullptr)
    {
      pManager->Update(GetBlockDuration(), plVec3::MakeZero());
    }
  }
}

namespace
{
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
} // namespace

plDataBuffer plAudioTestUtils::MakeWavF32(const plDynamicArray<float>& samples, plUInt32 uiSampleRate)
{
  constexpr plUInt16 uiChannels = 1;
  constexpr plUInt16 uiBitsPerSample = 32;
  constexpr plUInt16 uiBlockAlign = uiChannels * uiBitsPerSample / 8;
  const plUInt32 uiDataBytes = samples.GetCount() * uiBlockAlign;

  plDataBuffer data;
  data.Reserve(44 + uiDataBytes);

  AppendTag(data, "RIFF");
  AppendU32(data, 36 + uiDataBytes);
  AppendTag(data, "WAVE");

  AppendTag(data, "fmt ");
  AppendU32(data, 16);
  AppendU16(data, 3); // WAVE_FORMAT_IEEE_FLOAT
  AppendU16(data, uiChannels);
  AppendU32(data, uiSampleRate);
  AppendU32(data, uiSampleRate * uiBlockAlign);
  AppendU16(data, uiBlockAlign);
  AppendU16(data, uiBitsPerSample);

  AppendTag(data, "data");
  AppendU32(data, uiDataBytes);

  for (float f : samples)
  {
    plUInt32 uiBits = 0;
    memcpy(&uiBits, &f, sizeof(float));
    AppendU32(data, uiBits);
  }

  return data;
}

plDynamicArray<float> plAudioTestUtils::MakeDc(plUInt32 uiFrames, float fValue)
{
  plDynamicArray<float> samples;
  samples.SetCount(uiFrames, fValue);
  return samples;
}

plDynamicArray<float> plAudioTestUtils::MakeCosineCycle(plUInt32 uiFrames, plUInt32 uiPeriods, float fAmplitude)
{
  plDynamicArray<float> samples;
  samples.SetCountUninitialized(uiFrames);

  for (plUInt32 i = 0; i < uiFrames; ++i)
  {
    const double fPhase = 2.0 * plMath::Pi<double>() * static_cast<double>(uiPeriods) * static_cast<double>(i) / static_cast<double>(uiFrames);
    samples[i] = fAmplitude * static_cast<float>(plMath::Cos(plAngle::MakeFromRadian(fPhase)));
  }

  return samples;
}

plAudioEventResourceDescriptor plAudioTestUtils::MakeSingleInstrumentEvent(const plDynamicArray<float>& samples)
{
  plAudioEventResourceDescriptor res;
  plAudioEventDesc& desc = res.m_Desc;

  desc.m_bSpatialize = false;
  desc.m_FadeIn = plTime::MakeZero();
  desc.m_StopFadeOut = plTime::MakeZero();

  plAudioEventInstrumentDesc& instrument = desc.m_Instruments.ExpandAndGetRef();
  instrument.m_sName = "Instrument";
  instrument.m_ePlaylistMode = plAudioEventPlaylistMode::Sequential;

  plAudioEventPlaylistEntryDesc& entry = instrument.m_Playlist.ExpandAndGetRef();
  entry.m_uiSample = 0;
  entry.m_fWeight = 1.0f;

  desc.m_SampleNames.PushBack("Sample0.wav");
  res.m_SampleData.PushBack(MakeWavF32(samples, plOfflineAudioEngine::SampleRate));

  return res;
}

plAudioEventResourceHandle plAudioTestUtils::CreateEventResource(plStringView sId, plAudioEventResourceDescriptor&& descriptor)
{
  return plResourceManager::CreateResource<plAudioEventResource>(sId, std::move(descriptor));
}

plUInt32 plAudioTestUtils::FindFirstAbove(const plDynamicArray<float>& samples, float fThreshold, plUInt32 uiStart)
{
  for (plUInt32 i = uiStart; i < samples.GetCount(); ++i)
  {
    if (plMath::Abs(samples[i]) > fThreshold)
      return i;
  }

  return plInvalidIndex;
}

plDynamicArray<plUInt32> plAudioTestUtils::FindOnsets(const plDynamicArray<float>& samples, float fThreshold, plUInt32 uiMinSilentFrames)
{
  plDynamicArray<plUInt32> onsets;
  plUInt32 uiSilentRun = uiMinSilentFrames; // the start of the buffer counts as silence

  for (plUInt32 i = 0; i < samples.GetCount(); ++i)
  {
    if (plMath::Abs(samples[i]) > fThreshold)
    {
      if (uiSilentRun >= uiMinSilentFrames)
        onsets.PushBack(i);

      uiSilentRun = 0;
    }
    else
    {
      ++uiSilentRun;
    }
  }

  return onsets;
}

float plAudioTestUtils::MaxStep(const plDynamicArray<float>& samples, plUInt32 uiBegin, plUInt32 uiEnd)
{
  float fMax = 0.0f;
  uiBegin = plMath::Max(uiBegin, 1u);
  uiEnd = plMath::Min(uiEnd, samples.GetCount());

  for (plUInt32 i = uiBegin; i < uiEnd; ++i)
  {
    fMax = plMath::Max(fMax, plMath::Abs(samples[i] - samples[i - 1]));
  }

  return fMax;
}
