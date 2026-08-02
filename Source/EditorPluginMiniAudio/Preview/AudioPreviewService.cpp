#include <EditorPluginMiniAudio/EditorPluginMiniAudioPCH.h>

#include <EditorPluginMiniAudio/Preview/AudioPreviewService.h>

#include <Foundation/Math/Math.h>

PL_DEFINE_AS_POD_TYPE(ma_sound);

// ---------------------------------------------------------------------------------------------
// meter node
// ---------------------------------------------------------------------------------------------

static void plAudioPreviewMeterProcess(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
  PL_IGNORE_UNUSED(pFrameCountIn);

  auto* pMeter = reinterpret_cast<plAudioPreviewMeterNode*>(pNode);

  const float* pIn = ppFramesIn[0];
  float* pOut = ppFramesOut[0];
  const plUInt32 uiSamples = *pFrameCountOut * pMeter->m_uiChannels;

  float fPeak = 0.0f;
  float fSumSq = 0.0f;

  for (plUInt32 i = 0; i < uiSamples; ++i)
  {
    const float fSample = pIn[i];
    pOut[i] = fSample;

    fPeak = plMath::Max(fPeak, plMath::Abs(fSample));
    fSumSq += fSample * fSample;
  }

  const float fRms = uiSamples > 0 ? plMath::Sqrt(fSumSq / static_cast<float>(uiSamples)) : 0.0f;

  // decaying hold, so a UI polling at frame rate still catches transients
  const float fPrevPeak = pMeter->m_fPeak.load(std::memory_order_relaxed);
  pMeter->m_fPeak.store(plMath::Max(fPeak, fPrevPeak * 0.90f), std::memory_order_relaxed);

  const float fPrevRms = pMeter->m_fRms.load(std::memory_order_relaxed);
  pMeter->m_fRms.store(plMath::Lerp(fPrevRms, fRms, 0.35f), std::memory_order_relaxed);
}

static ma_node_vtable g_plAudioPreviewMeterVTable = {plAudioPreviewMeterProcess, nullptr, 1, 1, 0};

static plResult InitMeter(ma_engine* pEngine, plAudioPreviewMeterNode& ref_meter)
{
  ref_meter.m_uiChannels = ma_engine_get_channels(pEngine);

  ma_node_config cfg = ma_node_config_init();
  cfg.vtable = &g_plAudioPreviewMeterVTable;
  cfg.pInputChannels = &ref_meter.m_uiChannels;
  cfg.pOutputChannels = &ref_meter.m_uiChannels;

  if (ma_node_init(ma_engine_get_node_graph(pEngine), &cfg, nullptr, &ref_meter.m_Base) != MA_SUCCESS)
    return PL_FAILURE;

  return PL_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// plAudioPreviewService
// ---------------------------------------------------------------------------------------------

plAudioPreviewService& plAudioPreviewService::GetSingleton()
{
  static plAudioPreviewService s_Instance;
  return s_Instance;
}

plAudioPreviewService::~plAudioPreviewService()
{
  Shutdown();
}

bool plAudioPreviewService::EnsureInit()
{
  if (m_bInitialized)
    return true;

  ma_engine_config cfg = ma_engine_config_init();
  if (ma_engine_init(&cfg, &m_Engine) != MA_SUCCESS)
    return false;

  m_bInitialized = true;

  // master meter feeding the endpoint; used when no mixer is installed
  if (InitMeter(&m_Engine, m_MasterMeter).Succeeded())
  {
    ma_node_attach_output_bus(&m_MasterMeter.m_Base, 0, ma_node_graph_get_endpoint(ma_engine_get_node_graph(&m_Engine)), 0);
    m_bMasterMeterValid = true;
  }

  return true;
}

void plAudioPreviewService::Shutdown()
{
  StopAll();
  DestroyBuses();

  if (m_bMasterMeterValid)
  {
    ma_node_uninit(&m_MasterMeter.m_Base, nullptr);
    m_bMasterMeterValid = false;
  }

  if (m_bInitialized)
  {
    ma_engine_uninit(&m_Engine);
    m_bInitialized = false;
  }
}

void plAudioPreviewService::DestroyBuses()
{
  // reverse order: children before their parents
  for (plUInt32 i = m_Buses.GetCount(); i-- > 0;)
  {
    Bus& bus = m_Buses[i];

    if (bus.m_bMeterValid)
    {
      ma_node_uninit(&bus.m_Meter.m_Base, nullptr);
      bus.m_bMeterValid = false;
    }

    if (bus.m_bGroupValid)
    {
      ma_sound_group_uninit(&bus.m_Group);
      bus.m_bGroupValid = false;
    }
  }

  m_Buses.Clear();
}

void plAudioPreviewService::SetMixer(const plAudioMixerDesc& desc)
{
  if (!EnsureInit())
    return;

  // voices are attached to the old groups
  StopAll();
  DestroyBuses();

  if (desc.m_Buses.IsEmpty())
    return;

  ma_node* pEndpoint = ma_node_graph_get_endpoint(ma_engine_get_node_graph(&m_Engine));

  // 1) create every group first, so parents can be referenced in any order
  for (plUInt32 i = 0; i < desc.m_Buses.GetCount(); ++i)
  {
    Bus& bus = m_Buses.ExpandAndGetRef();
    bus.m_sName = desc.m_Buses[i].m_sName;
    bus.m_uiParent = desc.m_Buses[i].m_uiParent;
    bus.m_bIsAux = desc.m_Buses[i].m_bIsAux;

    if (ma_sound_group_init(&m_Engine, 0, nullptr, &bus.m_Group) != MA_SUCCESS)
      continue;

    bus.m_bGroupValid = true;
    ma_sound_group_set_volume(&bus.m_Group, plAudioMixerUtils::DbToLinear(desc.m_Buses[i].m_fVolumeDb));
    ma_sound_group_set_pitch(&bus.m_Group, plMath::Max(desc.m_Buses[i].m_fPitch, 0.01f));

    if (InitMeter(&m_Engine, bus.m_Meter).Succeeded())
    {
      bus.m_bMeterValid = true;
    }
  }

  // 2) wire group -> meter -> parent group (or the endpoint for the master bus)
  for (plUInt32 i = 0; i < m_Buses.GetCount(); ++i)
  {
    Bus& bus = m_Buses[i];

    if (!bus.m_bGroupValid)
      continue;

    ma_node* pTarget = pEndpoint;
    if (bus.m_uiParent != 0xFF && bus.m_uiParent < m_Buses.GetCount() && bus.m_uiParent != i && m_Buses[bus.m_uiParent].m_bGroupValid)
    {
      pTarget = reinterpret_cast<ma_node*>(&m_Buses[bus.m_uiParent].m_Group);
    }

    if (bus.m_bMeterValid)
    {
      ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&bus.m_Group), 0, &bus.m_Meter.m_Base, 0);
      ma_node_attach_output_bus(&bus.m_Meter.m_Base, 0, pTarget, 0);
    }
    else
    {
      ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&bus.m_Group), 0, pTarget, 0);
    }
  }
}

void plAudioPreviewService::ClearMixer()
{
  StopAll();
  DestroyBuses();
}

plUInt32 plAudioPreviewService::ResolveBus(plStringView sBusName) const
{
  if (m_Buses.IsEmpty())
    return plInvalidIndex;

  if (!sBusName.IsEmpty())
  {
    for (plUInt32 i = 0; i < m_Buses.GetCount(); ++i)
    {
      if (m_Buses[i].m_sName.IsEqual_NoCase(sBusName))
      {
        // match the runtime: aux buses take sends only, never a voice's direct output
        return m_Buses[i].m_bIsAux ? 0 : i;
      }
    }
  }

  return 0; // master
}

void plAudioPreviewService::GetBusMeter(plUInt32 uiBusIndex, float& out_fPeak, float& out_fRms) const
{
  out_fPeak = 0.0f;
  out_fRms = 0.0f;

  if (uiBusIndex >= m_Buses.GetCount() || !m_Buses[uiBusIndex].m_bMeterValid)
    return;

  out_fPeak = m_Buses[uiBusIndex].m_Meter.m_fPeak.load(std::memory_order_relaxed);
  out_fRms = m_Buses[uiBusIndex].m_Meter.m_fRms.load(std::memory_order_relaxed);
}

float plAudioPreviewService::GetMasterPeak() const
{
  // with a mixer installed the master bus is the true output
  if (!m_Buses.IsEmpty() && m_Buses[0].m_bMeterValid)
    return m_Buses[0].m_Meter.m_fPeak.load(std::memory_order_relaxed);

  return m_bMasterMeterValid ? m_MasterMeter.m_fPeak.load(std::memory_order_relaxed) : 0.0f;
}

plResult plAudioPreviewService::PlayFile(const char* szAbsPath, float fVolume, float fPitch, double fDelaySeconds, bool bLoop, plStringView sBusName)
{
  if (!EnsureInit())
    return PL_FAILURE;

  ma_sound& sound = m_Sounds.ExpandAndGetRef();

  if (ma_sound_init_from_file(&m_Engine, szAbsPath, MA_SOUND_FLAG_DECODE, nullptr, nullptr, &sound) != MA_SUCCESS)
  {
    m_Sounds.PopBack();
    return PL_FAILURE;
  }

  ma_sound_set_volume(&sound, fVolume);
  ma_sound_set_pitch(&sound, fPitch);
  ma_sound_set_looping(&sound, bLoop);
  ma_sound_set_spatialization_enabled(&sound, false);

  // route into the event's mixer bus when a mixer is installed, else through the master meter
  const plUInt32 uiBus = ResolveBus(sBusName);

  if (uiBus != plInvalidIndex && m_Buses[uiBus].m_bGroupValid)
  {
    ma_node_attach_output_bus(&sound, 0, reinterpret_cast<ma_node*>(&m_Buses[uiBus].m_Group), 0);
  }
  else if (m_bMasterMeterValid)
  {
    ma_node_attach_output_bus(&sound, 0, &m_MasterMeter.m_Base, 0);
  }

  if (fDelaySeconds > 0.0)
  {
    const ma_uint64 uiStartFrame = ma_engine_get_time_in_pcm_frames(&m_Engine) + static_cast<ma_uint64>(fDelaySeconds * ma_engine_get_sample_rate(&m_Engine));
    ma_sound_set_start_time_in_pcm_frames(&sound, uiStartFrame);
  }

  if (ma_sound_start(&sound) != MA_SUCCESS)
  {
    ma_sound_uninit(&sound);
    m_Sounds.PopBack();
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

void plAudioPreviewService::StopAll()
{
  for (ma_sound& sound : m_Sounds)
  {
    ma_sound_uninit(&sound);
  }

  m_Sounds.Clear();
}

bool plAudioPreviewService::IsAnythingPlaying() const
{
  for (const ma_sound& sound : m_Sounds)
  {
    if (ma_sound_is_playing(const_cast<ma_sound*>(&sound)) != 0)
      return true;
  }

  return false;
}
