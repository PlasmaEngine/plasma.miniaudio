#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Mixer/MiniAudioBusGraph.h>

plMiniAudioBusGraph::~plMiniAudioBusGraph()
{
  Teardown();
}

plResult plMiniAudioBusGraph::Build(const plAudioMixerDesc& desc, ma_engine* pEngine)
{
  Teardown();

  if (pEngine == nullptr || desc.m_Buses.IsEmpty())
    return PL_FAILURE;

  // mixers transformed by current editors are valid; older content is built anyway, with what the checks below drop
  {
    plStringBuilder sErrors;
    if (desc.Validate(sErrors).Failed())
    {
      plLog::Error("PlasmaAudio: the mixer has routing errors; affected parents and sends are not connected:\n{0}", sErrors);
    }
  }

  m_Desc = desc;
  m_pEngine = pEngine;

  ma_node* pEndpoint = ma_node_graph_get_endpoint(ma_engine_get_node_graph(pEngine));
  const plUInt32 uiChannels = ma_engine_get_channels(pEngine);

  // 1) create all groups first (sends and parents reference other buses)
  for (plUInt32 i = 0; i < desc.m_Buses.GetCount(); ++i)
  {
    auto& bus = m_Buses.ExpandAndGetRef();

    if (ma_sound_group_init(pEngine, 0, nullptr, &bus.m_Group) != MA_SUCCESS)
    {
      plLog::Error("PlasmaAudio: failed to create mixer bus '{0}'.", desc.m_Buses[i].m_sName);
      Teardown();
      return PL_FAILURE;
    }
    bus.m_bGroupValid = true;

    ma_sound_group_set_volume(&bus.m_Group, plAudioMixerUtils::DbToLinear(desc.m_Buses[i].m_fVolumeDb));
  }

  // 2) the effects of every bus before any wiring, since a bus's splitter feeds the key inputs of other buses' effects.
  // A failed effect stays in its slot as a bypass, so the effect indices of snapshot overrides stay valid.
  plDynamicArray<plHybridArray<ma_node*, 2>> keyTaps; // per key bus: the key inputs it feeds
  keyTaps.SetCount(desc.m_Buses.GetCount());

  for (plUInt32 i = 0; i < desc.m_Buses.GetCount(); ++i)
  {
    const auto& busDesc = desc.m_Buses[i];
    auto& bus = m_Buses[i];

    for (const auto& effectDesc : busDesc.m_Effects)
    {
      // a key bus fed by this bus would close a feedback loop; such an effect measures its own input instead
      const plUInt8 uiKey = effectDesc.m_uiSidechainBus;
      const bool bKeyed = plAudioEffectRegistry::GetTypeInfo(effectDesc.m_eType).m_bUsesSidechain && uiKey < desc.m_Buses.GetCount() && uiKey != i && !desc.IsRoutedTo(i, uiKey, true);

      auto& effect = bus.m_Effects.ExpandAndGetRef();
      if (effect.Create(pEngine, effectDesc, bKeyed).Failed())
      {
        plLog::Warning("PlasmaAudio: insert effect {0} on bus '{1}' could not be created and is bypassed.", bus.m_Effects.GetCount() - 1, busDesc.m_sName);
      }
      else if (effect.GetKeyInput() != nullptr)
      {
        keyTaps[uiKey].PushBack(effect.GetKeyInput());
      }
    }
  }

  // 3) build per-bus chains: group -> effects -> meter -> [splitter] -> parent
  for (plUInt32 i = 0; i < desc.m_Buses.GetCount(); ++i)
  {
    const auto& busDesc = desc.m_Buses[i];
    auto& bus = m_Buses[i];

    // meter
    if (plMiniAudioDsp::InitMeterNode(pEngine, &bus.m_Meter).Failed())
    {
      plLog::Error("PlasmaAudio: failed to create meter node for bus '{0}'.", busDesc.m_sName);
      Teardown();
      return PL_FAILURE;
    }
    bus.m_bMeterValid = true;

    // splitter (dry out 0 + one output per send + one per sidechain tap)
    if (!busDesc.m_Sends.IsEmpty() || !keyTaps[i].IsEmpty())
    {
      bus.m_pSplitter = PL_DEFAULT_NEW(ma_splitter_node);
      ma_splitter_node_config cfg = ma_splitter_node_config_init(uiChannels);
      cfg.outputBusCount = 1 + busDesc.m_Sends.GetCount() + keyTaps[i].GetCount();

      if (ma_splitter_node_init(ma_engine_get_node_graph(pEngine), &cfg, nullptr, bus.m_pSplitter) != MA_SUCCESS)
      {
        plLog::Error("PlasmaAudio: failed to create send splitter for bus '{0}'.", busDesc.m_sName);
        PL_DEFAULT_DELETE(bus.m_pSplitter);
        Teardown();
        return PL_FAILURE;
      }
    }

    // wire: group -> [fx0 ... fxN] -> meter
    ma_node* pCurrent = reinterpret_cast<ma_node*>(&bus.m_Group);
    for (auto& effect : bus.m_Effects)
    {
      if (effect.GetHead() == nullptr)
        continue;

      ma_node_attach_output_bus(pCurrent, 0, effect.GetHead(), 0);
      pCurrent = effect.GetTail();
    }
    ma_node_attach_output_bus(pCurrent, 0, reinterpret_cast<ma_node*>(&bus.m_Meter), 0);
    pCurrent = reinterpret_cast<ma_node*>(&bus.m_Meter);

    // meter -> [splitter]
    if (bus.m_pSplitter != nullptr)
    {
      ma_node_attach_output_bus(pCurrent, 0, reinterpret_cast<ma_node*>(bus.m_pSplitter), 0);
      pCurrent = reinterpret_cast<ma_node*>(bus.m_pSplitter);
    }

    // tail -> parent (or endpoint for the master bus); a parent that routes back to this bus would close a feedback
    // loop, so the bus goes to the endpoint instead
    ma_node* pParent = pEndpoint;
    if (busDesc.m_uiParent != 0xFF && busDesc.m_uiParent < m_Buses.GetCount() && busDesc.m_uiParent != i && !desc.IsRoutedTo(busDesc.m_uiParent, i))
    {
      pParent = reinterpret_cast<ma_node*>(&m_Buses[busDesc.m_uiParent].m_Group);
    }
    ma_node_attach_output_bus(pCurrent, 0, pParent, 0);

    // sends: splitter outputs 1..N -> aux bus groups; only to aux buses, and never back into this bus
    if (bus.m_pSplitter != nullptr)
    {
      for (plUInt32 s = 0; s < busDesc.m_Sends.GetCount(); ++s)
      {
        const plUInt8 uiTarget = busDesc.m_Sends[s].m_uiTargetBus;
        if (uiTarget >= m_Buses.GetCount() || uiTarget == i || !desc.m_Buses[uiTarget].m_bIsAux || desc.IsRoutedTo(uiTarget, i))
          continue;

        ma_node* pSplitterNode = reinterpret_cast<ma_node*>(bus.m_pSplitter);
        ma_node_attach_output_bus(pSplitterNode, 1 + s, reinterpret_cast<ma_node*>(&m_Buses[uiTarget].m_Group), 0);
        ma_node_set_output_bus_volume(pSplitterNode, 1 + s, plAudioMixerUtils::DbToLinear(busDesc.m_Sends[s].m_fLevelDb));
      }

      // sidechain taps: the bus's signal after its fader feeds input 1 of the effects it keys
      for (plUInt32 t = 0; t < keyTaps[i].GetCount(); ++t)
      {
        ma_node_attach_output_bus(reinterpret_cast<ma_node*>(bus.m_pSplitter), 1 + busDesc.m_Sends.GetCount() + t, keyTaps[i][t], 1);
      }
    }
  }

  GetAuthoredValues(m_Desc, m_BusValues);

  if (m_bTestSignal)
  {
    CreateTestSignals();
  }

  plLog::Info("PlasmaAudio: mixer graph built ({0} buses).", desc.m_Buses.GetCount());
  return PL_SUCCESS;
}

void plMiniAudioBusGraph::GetAuthoredValues(const plAudioMixerDesc& desc, plDynamicArray<BusValues>& out_values)
{
  out_values.Clear();
  out_values.SetCount(desc.m_Buses.GetCount());

  for (plUInt32 i = 0; i < desc.m_Buses.GetCount(); ++i)
  {
    const plAudioBusDesc& busDesc = desc.m_Buses[i];
    BusValues& values = out_values[i];

    // Build sets the authored volume and send levels; the pitch stays at miniaudio's default
    values.m_fGain = plAudioMixerUtils::DbToLinear(busDesc.m_fVolumeDb);
    values.m_fPitch = 1.0f;

    values.m_SendLevels.SetCount(busDesc.m_Sends.GetCount());
    for (plUInt32 s = 0; s < busDesc.m_Sends.GetCount(); ++s)
    {
      values.m_SendLevels[s] = plAudioMixerUtils::DbToLinear(busDesc.m_Sends[s].m_fLevelDb);
    }

    values.m_EffectParams.SetCount(busDesc.m_Effects.GetCount() * plAudioEffectMaxParams);
    values.m_EffectBypass.SetCount(busDesc.m_Effects.GetCount());
    for (plUInt32 e = 0; e < busDesc.m_Effects.GetCount(); ++e)
    {
      for (plUInt32 p = 0; p < plAudioEffectMaxParams; ++p)
      {
        values.m_EffectParams[e * plAudioEffectMaxParams + p] = busDesc.m_Effects[e].m_fParams[p];
      }

      values.m_EffectBypass[e] = busDesc.m_Effects[e].m_bBypass;
    }
  }
}

void plMiniAudioBusGraph::SetTestSignal(bool bEnable)
{
  if (m_bTestSignal == bEnable)
    return;

  m_bTestSignal = bEnable;

  if (!IsBuilt())
    return;

  if (bEnable)
  {
    CreateTestSignals();
  }
  else
  {
    DestroyTestSignals();
  }
}

void plMiniAudioBusGraph::CreateTestSignals()
{
  DestroyTestSignals();

  const plUInt32 uiBusCount = m_Desc.m_Buses.GetCount();

  for (plUInt32 i = 0; i < uiBusCount; ++i)
  {
    if (m_Desc.m_Buses[i].m_bIsAux)
      continue;

    // buses with children hear their children's signal
    bool bHasChild = false;
    for (plUInt32 j = 0; j < uiBusCount; ++j)
    {
      bHasChild |= (j != i && m_Desc.m_Buses[j].m_uiParent == i);
    }

    if (bHasChild)
      continue;

    plMaTestSignalNode& node = m_TestSignals.ExpandAndGetRef();
    node.m_fPeriodSeconds = 2.3f + 0.9f * static_cast<float>(m_TestSignals.GetCount() - 1);
    node.m_fBurstSeconds = node.m_fPeriodSeconds * 0.55f;
    node.m_uiNoiseState = 0x9E3779B9u * (i + 1);

    if (plMiniAudioDsp::InitTestSignalNode(m_pEngine, &node).Succeeded())
    {
      ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&node), 0, reinterpret_cast<ma_node*>(&m_Buses[i].m_Group), 0);
    }
  }
}

void plMiniAudioBusGraph::DestroyTestSignals()
{
  for (plMaTestSignalNode& node : m_TestSignals)
  {
    plMiniAudioDsp::UninitTestSignalNode(&node);
  }

  m_TestSignals.Clear();
}

void plMiniAudioBusGraph::Teardown()
{
  if (m_pEngine == nullptr && m_Buses.IsEmpty())
    return;

  // the test signals feed the bus groups, so they go first
  DestroyTestSignals();

  // detach and destroy in reverse creation order
  for (plUInt32 i = m_Buses.GetCount(); i-- > 0;)
  {
    auto& bus = m_Buses[i];

    if (bus.m_pSplitter != nullptr)
    {
      ma_splitter_node_uninit(bus.m_pSplitter, nullptr);
      PL_DEFAULT_DELETE(bus.m_pSplitter);
    }

    if (bus.m_bMeterValid)
    {
      plMiniAudioDsp::UninitMeterNode(&bus.m_Meter);
      bus.m_bMeterValid = false;
    }

    for (auto& effect : bus.m_Effects)
    {
      effect.Destroy();
    }
    bus.m_Effects.Clear();

    if (bus.m_bGroupValid)
    {
      ma_sound_group_uninit(&bus.m_Group);
      bus.m_bGroupValid = false;
    }
  }

  m_Buses.Clear();
  m_BusValues.Clear();
  m_Desc = plAudioMixerDesc();
  m_pEngine = nullptr;
}

ma_sound_group* plMiniAudioBusGraph::GetBusInput(plUInt32 uiBusIndex)
{
  if (uiBusIndex >= m_Buses.GetCount())
    return nullptr;

  return &m_Buses[uiBusIndex].m_Group;
}

plUInt32 plMiniAudioBusGraph::ResolveBus(plStringView sBusName) const
{
  if (!sBusName.IsEmpty())
  {
    const plUInt32 uiBus = m_Desc.FindBusByName(sBusName);

    // aux buses are reached by sends and environment routing only. A voice attached straight to one
    // would be 100% wet with no dry signal, so route to master instead — the editor no longer offers
    // aux buses here, but content authored before that could still name one.
    if (uiBus != plInvalidIndex && m_Desc.m_Buses[uiBus].m_bIsAux)
    {
      plLog::Warning("MiniAudio: '{0}' is an aux bus and cannot be an event output bus. Routing to master.", sBusName);
      return 0;
    }

    if (uiBus != plInvalidIndex)
      return uiBus;

    // a renamed or deleted bus: said once per name, so a misrouted event is noticed
    if (!m_WarnedUnknownBuses.Contains(sBusName))
    {
      m_WarnedUnknownBuses.Insert(sBusName);
      plLog::Warning("PlasmaAudio: the mixer has no bus '{0}'. Events that output to it play on master.", sBusName);
    }
  }

  return 0; // master
}

ma_sound_group* plMiniAudioBusGraph::GetEnvironmentAuxBus(plStringView sEnvironmentName)
{
  const plUInt32 uiBus = m_Desc.FindAuxBusForEnvironment(sEnvironmentName);
  if (uiBus == plInvalidIndex || uiBus >= m_Buses.GetCount())
    return nullptr;

  return &m_Buses[uiBus].m_Group;
}

ma_sound_group* plMiniAudioBusGraph::GetAuxBus(plStringView sBusName)
{
  for (plUInt32 i = 0; i < m_Desc.m_Buses.GetCount() && i < m_Buses.GetCount(); ++i)
  {
    if (m_Desc.m_Buses[i].m_bIsAux && m_Desc.m_Buses[i].m_sName == sBusName)
      return &m_Buses[i].m_Group;
  }

  return nullptr;
}

void plMiniAudioBusGraph::SetBusVolume(plUInt32 uiBusIndex, float fLinearGain)
{
  if (uiBusIndex >= m_Buses.GetCount())
    return;

  m_BusValues[uiBusIndex].m_fGain = fLinearGain;
  ma_sound_group_set_volume(&m_Buses[uiBusIndex].m_Group, fLinearGain);
}

void plMiniAudioBusGraph::SetBusPitch(plUInt32 uiBusIndex, float fPitch)
{
  if (uiBusIndex >= m_Buses.GetCount())
    return;

  m_BusValues[uiBusIndex].m_fPitch = fPitch;
  ma_sound_group_set_pitch(&m_Buses[uiBusIndex].m_Group, plMath::Max(fPitch, 0.01f));
}

void plMiniAudioBusGraph::SetBusSendLevel(plUInt32 uiBusIndex, plUInt32 uiSendIndex, float fLinearLevel)
{
  if (uiBusIndex >= m_Buses.GetCount())
    return;

  plHybridArray<float, 2>& sendLevels = m_BusValues[uiBusIndex].m_SendLevels;
  if (uiSendIndex < sendLevels.GetCount())
  {
    sendLevels[uiSendIndex] = fLinearLevel;
  }

  auto& bus = m_Buses[uiBusIndex];
  if (bus.m_pSplitter == nullptr)
    return;

  ma_node_set_output_bus_volume(reinterpret_cast<ma_node*>(bus.m_pSplitter), 1 + uiSendIndex, plMath::Max(fLinearLevel, 0.0f));
}

void plMiniAudioBusGraph::SetBusEffectParameter(plUInt32 uiBusIndex, plUInt32 uiEffectIndex, plUInt32 uiParamIndex, float fValue)
{
  if (uiBusIndex >= m_Buses.GetCount())
    return;

  plHybridArray<float, 8>& effectParams = m_BusValues[uiBusIndex].m_EffectParams;
  const plUInt32 uiSlot = uiEffectIndex * plAudioEffectMaxParams + uiParamIndex;
  if (uiParamIndex < plAudioEffectMaxParams && uiSlot < effectParams.GetCount())
  {
    effectParams[uiSlot] = fValue;
  }

  auto& bus = m_Buses[uiBusIndex];
  if (uiEffectIndex >= bus.m_Effects.GetCount())
    return;

  bus.m_Effects[uiEffectIndex].SetParameter(uiParamIndex, fValue);
}

void plMiniAudioBusGraph::SetBusEffectBypass(plUInt32 uiBusIndex, plUInt32 uiEffectIndex, bool bBypass)
{
  if (uiBusIndex >= m_Buses.GetCount())
    return;

  plHybridArray<bool, 2>& bypass = m_BusValues[uiBusIndex].m_EffectBypass;
  if (uiEffectIndex < bypass.GetCount())
  {
    bypass[uiEffectIndex] = bBypass;
  }

  auto& bus = m_Buses[uiBusIndex];
  if (uiEffectIndex < bus.m_Effects.GetCount())
  {
    bus.m_Effects[uiEffectIndex].SetBypass(bBypass);
  }
}

float plMiniAudioBusGraph::GetEffectGainReductionDb(plUInt32 uiBusIndex, plUInt32 uiEffectIndex) const
{
  if (uiBusIndex >= m_Buses.GetCount() || uiEffectIndex >= m_Buses[uiBusIndex].m_Effects.GetCount())
    return 0.0f;

  return m_Buses[uiBusIndex].m_Effects[uiEffectIndex].GetGainReductionDb();
}

void plMiniAudioBusGraph::GetBusMeter(plUInt32 uiBusIndex, float& out_fPeak, float& out_fRms) const
{
  out_fPeak = 0.0f;
  out_fRms = 0.0f;

  if (uiBusIndex >= m_Buses.GetCount() || !m_Buses[uiBusIndex].m_bMeterValid)
    return;

  out_fPeak = m_Buses[uiBusIndex].m_Meter.m_fPeak.load(std::memory_order_relaxed);
  out_fRms = m_Buses[uiBusIndex].m_Meter.m_fRms.load(std::memory_order_relaxed);
}

void plMiniAudioBusGraph::ApplyBusValues(const plDynamicArray<BusValues>& values)
{
  const plUInt32 uiBusCount = plMath::Min(values.GetCount(), m_BusValues.GetCount());

  for (plUInt32 uiBus = 0; uiBus < uiBusCount; ++uiBus)
  {
    const BusValues& busValues = values[uiBus];

    SetBusVolume(uiBus, busValues.m_fGain);
    SetBusPitch(uiBus, busValues.m_fPitch);

    for (plUInt32 s = 0; s < busValues.m_SendLevels.GetCount(); ++s)
    {
      SetBusSendLevel(uiBus, s, busValues.m_SendLevels[s]);
    }

    for (plUInt32 uiSlot = 0; uiSlot < busValues.m_EffectParams.GetCount(); ++uiSlot)
    {
      SetBusEffectParameter(uiBus, uiSlot / plAudioEffectMaxParams, uiSlot % plAudioEffectMaxParams, busValues.m_EffectParams[uiSlot]);
    }

    for (plUInt32 e = 0; e < busValues.m_EffectBypass.GetCount(); ++e)
    {
      SetBusEffectBypass(uiBus, e, busValues.m_EffectBypass[e]);
    }
  }
}

float plMiniAudioBusGraph::GetBusGain(plUInt32 uiBusIndex) const
{
  if (uiBusIndex >= m_BusValues.GetCount())
    return 1.0f;

  return m_BusValues[uiBusIndex].m_fGain;
}

float plMiniAudioBusGraph::GetEffectiveBusGain(plUInt32 uiBusIndex) const
{
  float fGain = 1.0f;

  // the depth bound guards against a cyclic parent chain in malformed data
  for (plUInt32 uiDepth = 0; uiBusIndex < m_BusValues.GetCount() && uiDepth < m_BusValues.GetCount(); ++uiDepth)
  {
    fGain *= m_BusValues[uiBusIndex].m_fGain;
    uiBusIndex = m_Desc.m_Buses[uiBusIndex].m_uiParent;
  }

  return fGain;
}
