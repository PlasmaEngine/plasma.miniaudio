#include <MiniAudioPlugin/MiniAudioPluginPCH.h>

#include <Foundation/Math/Math.h>
#include <MiniAudioPlugin/Mixer/MiniAudioDspNodes.h>

// ---------------------------------------------------------------------------------------------
// Meter node
// ---------------------------------------------------------------------------------------------

static void plMaMeterNodeProcess(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
  PL_IGNORE_UNUSED(pFrameCountIn);

  auto* pMeter = reinterpret_cast<plMaMeterNode*>(pNode);

  const float* pIn = ppFramesIn[0];
  float* pOut = ppFramesOut[0];
  const plUInt32 uiSamples = *pFrameCountOut * pMeter->m_uiChannels;

  float fPeak = 0.0f;
  float fSumSq = 0.0f;

  for (plUInt32 i = 0; i < uiSamples; ++i)
  {
    const float fSample = pIn[i];
    pOut[i] = fSample;

    const float fAbs = plMath::Abs(fSample);
    fPeak = plMath::Max(fPeak, fAbs);
    fSumSq += fSample * fSample;
  }

  const float fRms = uiSamples > 0 ? plMath::Sqrt(fSumSq / static_cast<float>(uiSamples)) : 0.0f;

  // decaying peak hold so the meter is readable when polled at frame rate
  const float fPrevPeak = pMeter->m_fPeak.load(std::memory_order_relaxed);
  pMeter->m_fPeak.store(plMath::Max(fPeak, fPrevPeak * 0.95f), std::memory_order_relaxed);

  const float fPrevRms = pMeter->m_fRms.load(std::memory_order_relaxed);
  pMeter->m_fRms.store(plMath::Lerp(fPrevRms, fRms, 0.3f), std::memory_order_relaxed);
}

static ma_node_vtable g_plMaMeterNodeVTable = {plMaMeterNodeProcess, nullptr, 1, 1, 0};

plResult plMiniAudioDsp::InitMeterNode(ma_engine* pEngine, plMaMeterNode* pNode)
{
  pNode->m_uiChannels = ma_engine_get_channels(pEngine);

  ma_node_config cfg = ma_node_config_init();
  cfg.vtable = &g_plMaMeterNodeVTable;
  cfg.pInputChannels = &pNode->m_uiChannels;
  cfg.pOutputChannels = &pNode->m_uiChannels;

  if (ma_node_init(ma_engine_get_node_graph(pEngine), &cfg, nullptr, &pNode->m_Base) != MA_SUCCESS)
    return PL_FAILURE;

  return PL_SUCCESS;
}

void plMiniAudioDsp::UninitMeterNode(plMaMeterNode* pNode)
{
  ma_node_uninit(&pNode->m_Base, nullptr);
}

// ---------------------------------------------------------------------------------------------
// Reverb node (Freeverb)
// ---------------------------------------------------------------------------------------------

static constexpr plUInt32 kCombTunings[plMaReverbNode::NumCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static constexpr plUInt32 kAllpassTunings[plMaReverbNode::NumAllpasses] = {556, 441, 341, 225};
static constexpr plUInt32 kChannelSpread = 23;
static constexpr float kFixedGain = 0.015f;

static void plMaReverbNodeProcess(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
  PL_IGNORE_UNUSED(pFrameCountIn);

  auto* pReverb = reinterpret_cast<plMaReverbNode*>(pNode);

  const float* pIn = ppFramesIn[0];
  float* pOut = ppFramesOut[0];
  const plUInt32 uiFrames = *pFrameCountOut;
  const plUInt32 uiChannels = pReverb->m_uiChannels;

  const float fFeedback = 0.7f + 0.28f * plMath::Clamp(pReverb->m_fRoomSize, 0.0f, 1.0f);
  const float fDamp1 = plMath::Clamp(pReverb->m_fDamping, 0.0f, 1.0f) * 0.4f;
  const float fDamp2 = 1.0f - fDamp1;
  const float fWet = pReverb->m_fWet;
  const float fDry = pReverb->m_fDry;

  for (plUInt32 f = 0; f < uiFrames; ++f)
  {
    for (plUInt32 c = 0; c < uiChannels; ++c)
    {
      const float fInput = pIn[f * uiChannels + c];
      const float fCombInput = fInput * kFixedGain;
      float fReverbOut = 0.0f;

      auto* pCombs = &pReverb->m_pCombs[c * plMaReverbNode::NumCombs];
      for (plUInt32 i = 0; i < plMaReverbNode::NumCombs; ++i)
      {
        auto& comb = pCombs[i];
        const float fY = comb.m_pBuffer[comb.m_uiIndex];
        comb.m_fFilterStore = fY * fDamp2 + comb.m_fFilterStore * fDamp1;
        comb.m_pBuffer[comb.m_uiIndex] = fCombInput + comb.m_fFilterStore * fFeedback;
        if (++comb.m_uiIndex >= comb.m_uiSize)
          comb.m_uiIndex = 0;
        fReverbOut += fY;
      }

      auto* pAllpasses = &pReverb->m_pAllpasses[c * plMaReverbNode::NumAllpasses];
      for (plUInt32 i = 0; i < plMaReverbNode::NumAllpasses; ++i)
      {
        auto& allpass = pAllpasses[i];
        const float fBufOut = allpass.m_pBuffer[allpass.m_uiIndex];
        const float fOutput = -fReverbOut + fBufOut;
        allpass.m_pBuffer[allpass.m_uiIndex] = fReverbOut + fBufOut * 0.5f;
        if (++allpass.m_uiIndex >= allpass.m_uiSize)
          allpass.m_uiIndex = 0;
        fReverbOut = fOutput;
      }

      pOut[f * uiChannels + c] = fInput * fDry + fReverbOut * fWet;
    }
  }
}

static ma_node_vtable g_plMaReverbNodeVTable = {plMaReverbNodeProcess, nullptr, 1, 1, 0};

plResult plMiniAudioDsp::InitReverbNode(ma_engine* pEngine, plMaReverbNode* pNode)
{
  const plUInt32 uiChannels = ma_engine_get_channels(pEngine);
  const plUInt32 uiSampleRate = ma_engine_get_sample_rate(pEngine);
  const float fSrScale = static_cast<float>(uiSampleRate) / 44100.0f;

  pNode->m_uiChannels = uiChannels;

  pNode->m_pCombs = PL_DEFAULT_NEW_ARRAY(plMaReverbNode::Comb, uiChannels * plMaReverbNode::NumCombs).GetPtr();
  pNode->m_pAllpasses = PL_DEFAULT_NEW_ARRAY(plMaReverbNode::Allpass, uiChannels * plMaReverbNode::NumAllpasses).GetPtr();

  for (plUInt32 c = 0; c < uiChannels; ++c)
  {
    for (plUInt32 i = 0; i < plMaReverbNode::NumCombs; ++i)
    {
      auto& comb = pNode->m_pCombs[c * plMaReverbNode::NumCombs + i];
      comb.m_uiSize = plMath::Max<plUInt32>(4, static_cast<plUInt32>((kCombTunings[i] + c * kChannelSpread) * fSrScale));
      auto buffer = PL_DEFAULT_NEW_ARRAY(float, comb.m_uiSize);
      plMemoryUtils::ZeroFill(buffer.GetPtr(), comb.m_uiSize);
      comb.m_pBuffer = buffer.GetPtr();
    }

    for (plUInt32 i = 0; i < plMaReverbNode::NumAllpasses; ++i)
    {
      auto& allpass = pNode->m_pAllpasses[c * plMaReverbNode::NumAllpasses + i];
      allpass.m_uiSize = plMath::Max<plUInt32>(4, static_cast<plUInt32>((kAllpassTunings[i] + c * kChannelSpread) * fSrScale));
      auto buffer = PL_DEFAULT_NEW_ARRAY(float, allpass.m_uiSize);
      plMemoryUtils::ZeroFill(buffer.GetPtr(), allpass.m_uiSize);
      allpass.m_pBuffer = buffer.GetPtr();
    }
  }

  ma_node_config cfg = ma_node_config_init();
  cfg.vtable = &g_plMaReverbNodeVTable;
  cfg.pInputChannels = &pNode->m_uiChannels;
  cfg.pOutputChannels = &pNode->m_uiChannels;

  if (ma_node_init(ma_engine_get_node_graph(pEngine), &cfg, nullptr, &pNode->m_Base) != MA_SUCCESS)
  {
    UninitReverbNode(pNode);
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

void plMiniAudioDsp::UninitReverbNode(plMaReverbNode* pNode)
{
  if (pNode->m_pCombs != nullptr || pNode->m_pAllpasses != nullptr)
  {
    // only uninit the node if it was successfully initialized (buffers exist but init may have failed)
    if (((ma_node_base*)&pNode->m_Base)->vtable != nullptr)
    {
      ma_node_uninit(&pNode->m_Base, nullptr);
    }
  }

  if (pNode->m_pCombs != nullptr)
  {
    for (plUInt32 i = 0; i < pNode->m_uiChannels * plMaReverbNode::NumCombs; ++i)
    {
      PL_DEFAULT_DELETE_ARRAY(plArrayPtr<float>(pNode->m_pCombs[i].m_pBuffer, pNode->m_pCombs[i].m_uiSize));
    }
    PL_DEFAULT_DELETE_ARRAY(plArrayPtr<plMaReverbNode::Comb>(pNode->m_pCombs, pNode->m_uiChannels * plMaReverbNode::NumCombs));
    pNode->m_pCombs = nullptr;
  }

  if (pNode->m_pAllpasses != nullptr)
  {
    for (plUInt32 i = 0; i < pNode->m_uiChannels * plMaReverbNode::NumAllpasses; ++i)
    {
      PL_DEFAULT_DELETE_ARRAY(plArrayPtr<float>(pNode->m_pAllpasses[i].m_pBuffer, pNode->m_pAllpasses[i].m_uiSize));
    }
    PL_DEFAULT_DELETE_ARRAY(plArrayPtr<plMaReverbNode::Allpass>(pNode->m_pAllpasses, pNode->m_uiChannels * plMaReverbNode::NumAllpasses));
    pNode->m_pAllpasses = nullptr;
  }
}

// ---------------------------------------------------------------------------------------------
// Compressor / limiter node
// ---------------------------------------------------------------------------------------------

static void plMaCompressorNodeProcess(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
  PL_IGNORE_UNUSED(pFrameCountIn);

  auto* pComp = reinterpret_cast<plMaCompressorNode*>(pNode);

  const float* pIn = ppFramesIn[0];
  float* pOut = ppFramesOut[0];
  const plUInt32 uiFrames = *pFrameCountOut;
  const plUInt32 uiChannels = pComp->m_uiChannels;
  const float fSampleRate = static_cast<float>(pComp->m_uiSampleRate);

  const float fAttackCoef = plMath::Exp(-1.0f / (plMath::Max(pComp->m_fAttackMs, 0.02f) * 0.001f * fSampleRate));
  const float fReleaseCoef = plMath::Exp(-1.0f / (plMath::Max(pComp->m_fReleaseMs, 1.0f) * 0.001f * fSampleRate));
  const float fThresholdLin = plAudioMixerUtils::DbToLinear(pComp->m_fThresholdDb);
  const float fMakeupLin = plAudioMixerUtils::DbToLinear(pComp->m_fMakeupDb);
  const float fInvRatio = pComp->m_bLimiter ? 0.0f : (1.0f / plMath::Max(pComp->m_fRatio, 1.0f));

  float fEnvelope = pComp->m_fEnvelope;

  for (plUInt32 f = 0; f < uiFrames; ++f)
  {
    float fLevel = 0.0f;
    for (plUInt32 c = 0; c < uiChannels; ++c)
      fLevel = plMath::Max(fLevel, plMath::Abs(pIn[f * uiChannels + c]));

    const float fCoef = (fLevel > fEnvelope) ? fAttackCoef : fReleaseCoef;
    fEnvelope = fCoef * fEnvelope + (1.0f - fCoef) * fLevel;

    float fGain = 1.0f;
    if (fEnvelope > fThresholdLin && fEnvelope > 0.0f)
    {
      // gain reduction so that (threshold + over * invRatio) is the resulting level
      const float fTarget = fThresholdLin * plMath::Pow(fEnvelope / fThresholdLin, fInvRatio);
      fGain = fTarget / fEnvelope;
    }

    const float fTotalGain = fGain * fMakeupLin;
    for (plUInt32 c = 0; c < uiChannels; ++c)
      pOut[f * uiChannels + c] = pIn[f * uiChannels + c] * fTotalGain;
  }

  pComp->m_fEnvelope = fEnvelope;
}

static ma_node_vtable g_plMaCompressorNodeVTable = {plMaCompressorNodeProcess, nullptr, 1, 1, 0};

plResult plMiniAudioDsp::InitCompressorNode(ma_engine* pEngine, plMaCompressorNode* pNode, bool bLimiter)
{
  pNode->m_uiChannels = ma_engine_get_channels(pEngine);
  pNode->m_uiSampleRate = ma_engine_get_sample_rate(pEngine);
  pNode->m_bLimiter = bLimiter;

  ma_node_config cfg = ma_node_config_init();
  cfg.vtable = &g_plMaCompressorNodeVTable;
  cfg.pInputChannels = &pNode->m_uiChannels;
  cfg.pOutputChannels = &pNode->m_uiChannels;

  if (ma_node_init(ma_engine_get_node_graph(pEngine), &cfg, nullptr, &pNode->m_Base) != MA_SUCCESS)
    return PL_FAILURE;

  return PL_SUCCESS;
}

void plMiniAudioDsp::UninitCompressorNode(plMaCompressorNode* pNode)
{
  ma_node_uninit(&pNode->m_Base, nullptr);
}

// ---------------------------------------------------------------------------------------------
// plMiniAudioEffect
// ---------------------------------------------------------------------------------------------

plResult plMiniAudioEffect::Create(ma_engine* pEngine, const plAudioEffectDesc& desc)
{
  m_pEngine = pEngine;
  m_eType = desc.m_eType;
  plMemoryUtils::Copy(m_fParams, desc.m_fParams, plAudioEffectMaxParams);

  const plUInt32 uiChannels = ma_engine_get_channels(pEngine);
  const plUInt32 uiSampleRate = ma_engine_get_sample_rate(pEngine);
  ma_node_graph* pGraph = ma_engine_get_node_graph(pEngine);

  switch (m_eType)
  {
    case plAudioEffectType::LowPass:
    {
      ma_lpf_node* pNode = PL_DEFAULT_NEW(ma_lpf_node);
      const ma_lpf_node_config cfg = ma_lpf_node_config_init(uiChannels, uiSampleRate, plMath::Clamp(m_fParams[0], 10.0f, 24000.0f), plMath::Clamp<plUInt32>(static_cast<plUInt32>(m_fParams[1]), 1, 8));
      if (ma_lpf_node_init(pGraph, &cfg, nullptr, pNode) != MA_SUCCESS)
      {
        PL_DEFAULT_DELETE(pNode);
        return PL_FAILURE;
      }
      m_Nodes.PushBack(reinterpret_cast<ma_node*>(pNode));
      break;
    }

    case plAudioEffectType::HighPass:
    {
      ma_hpf_node* pNode = PL_DEFAULT_NEW(ma_hpf_node);
      const ma_hpf_node_config cfg = ma_hpf_node_config_init(uiChannels, uiSampleRate, plMath::Clamp(m_fParams[0], 10.0f, 24000.0f), plMath::Clamp<plUInt32>(static_cast<plUInt32>(m_fParams[1]), 1, 8));
      if (ma_hpf_node_init(pGraph, &cfg, nullptr, pNode) != MA_SUCCESS)
      {
        PL_DEFAULT_DELETE(pNode);
        return PL_FAILURE;
      }
      m_Nodes.PushBack(reinterpret_cast<ma_node*>(pNode));
      break;
    }

    case plAudioEffectType::Eq3:
    {
      ma_loshelf_node* pLow = PL_DEFAULT_NEW(ma_loshelf_node);
      ma_peak_node* pMid = PL_DEFAULT_NEW(ma_peak_node);
      ma_hishelf_node* pHigh = PL_DEFAULT_NEW(ma_hishelf_node);

      const ma_loshelf_node_config lowCfg = ma_loshelf_node_config_init(uiChannels, uiSampleRate, m_fParams[0], 0.707, plMath::Max(m_fParams[1], 10.0f));
      const ma_peak_node_config midCfg = ma_peak_node_config_init(uiChannels, uiSampleRate, m_fParams[2], plMath::Max(m_fParams[4], 0.1f), plMath::Max(m_fParams[3], 10.0f));
      const ma_hishelf_node_config highCfg = ma_hishelf_node_config_init(uiChannels, uiSampleRate, m_fParams[5], 0.707, plMath::Max(m_fParams[6], 10.0f));

      if (ma_loshelf_node_init(pGraph, &lowCfg, nullptr, pLow) != MA_SUCCESS ||
          ma_peak_node_init(pGraph, &midCfg, nullptr, pMid) != MA_SUCCESS ||
          ma_hishelf_node_init(pGraph, &highCfg, nullptr, pHigh) != MA_SUCCESS)
      {
        PL_DEFAULT_DELETE(pLow);
        PL_DEFAULT_DELETE(pMid);
        PL_DEFAULT_DELETE(pHigh);
        return PL_FAILURE;
      }

      // chain: low -> mid -> high
      ma_node_attach_output_bus(reinterpret_cast<ma_node*>(pLow), 0, reinterpret_cast<ma_node*>(pMid), 0);
      ma_node_attach_output_bus(reinterpret_cast<ma_node*>(pMid), 0, reinterpret_cast<ma_node*>(pHigh), 0);

      m_Nodes.PushBack(reinterpret_cast<ma_node*>(pLow));
      m_Nodes.PushBack(reinterpret_cast<ma_node*>(pMid));
      m_Nodes.PushBack(reinterpret_cast<ma_node*>(pHigh));
      break;
    }

    case plAudioEffectType::Delay:
    {
      ma_delay_node* pNode = PL_DEFAULT_NEW(ma_delay_node);
      const plUInt32 uiDelayFrames = plMath::Max<plUInt32>(1, static_cast<plUInt32>(plMath::Max(m_fParams[0], 1.0f) * 0.001f * uiSampleRate));
      ma_delay_node_config cfg = ma_delay_node_config_init(uiChannels, uiSampleRate, uiDelayFrames, plMath::Clamp(m_fParams[1], 0.0f, 0.99f));
      cfg.delay.wet = plMath::Clamp(m_fParams[2], 0.0f, 1.0f);
      cfg.delay.dry = plMath::Clamp(m_fParams[3], 0.0f, 1.0f);
      if (ma_delay_node_init(pGraph, &cfg, nullptr, pNode) != MA_SUCCESS)
      {
        PL_DEFAULT_DELETE(pNode);
        return PL_FAILURE;
      }
      m_Nodes.PushBack(reinterpret_cast<ma_node*>(pNode));
      break;
    }

    case plAudioEffectType::Reverb:
    {
      plMaReverbNode* pNode = PL_DEFAULT_NEW(plMaReverbNode);
      pNode->m_fRoomSize = m_fParams[0];
      pNode->m_fDamping = m_fParams[1];
      pNode->m_fWidth = m_fParams[2];
      pNode->m_fWet = m_fParams[3];
      pNode->m_fDry = m_fParams[4];
      if (plMiniAudioDsp::InitReverbNode(pEngine, pNode).Failed())
      {
        PL_DEFAULT_DELETE(pNode);
        return PL_FAILURE;
      }
      m_Nodes.PushBack(reinterpret_cast<ma_node*>(pNode));
      break;
    }

    case plAudioEffectType::Compressor:
    case plAudioEffectType::Limiter:
    {
      const bool bLimiter = (m_eType == plAudioEffectType::Limiter);
      plMaCompressorNode* pNode = PL_DEFAULT_NEW(plMaCompressorNode);
      if (bLimiter)
      {
        pNode->m_fThresholdDb = m_fParams[0];
        pNode->m_fReleaseMs = m_fParams[1];
        pNode->m_fAttackMs = 0.1f;
      }
      else
      {
        pNode->m_fThresholdDb = m_fParams[0];
        pNode->m_fRatio = m_fParams[1];
        pNode->m_fAttackMs = m_fParams[2];
        pNode->m_fReleaseMs = m_fParams[3];
        pNode->m_fMakeupDb = m_fParams[4];
      }
      if (plMiniAudioDsp::InitCompressorNode(pEngine, pNode, bLimiter).Failed())
      {
        PL_DEFAULT_DELETE(pNode);
        return PL_FAILURE;
      }
      m_Nodes.PushBack(reinterpret_cast<ma_node*>(pNode));
      break;
    }
  }

  return m_Nodes.IsEmpty() ? PL_FAILURE : PL_SUCCESS;
}

void plMiniAudioEffect::Destroy()
{
  switch (m_eType)
  {
    case plAudioEffectType::LowPass:
      if (!m_Nodes.IsEmpty())
      {
        auto* pNode = reinterpret_cast<ma_lpf_node*>(m_Nodes[0]);
        ma_lpf_node_uninit(pNode, nullptr);
        PL_DEFAULT_DELETE(pNode);
      }
      break;

    case plAudioEffectType::HighPass:
      if (!m_Nodes.IsEmpty())
      {
        auto* pNode = reinterpret_cast<ma_hpf_node*>(m_Nodes[0]);
        ma_hpf_node_uninit(pNode, nullptr);
        PL_DEFAULT_DELETE(pNode);
      }
      break;

    case plAudioEffectType::Eq3:
      if (m_Nodes.GetCount() == 3)
      {
        auto* pLow = reinterpret_cast<ma_loshelf_node*>(m_Nodes[0]);
        auto* pMid = reinterpret_cast<ma_peak_node*>(m_Nodes[1]);
        auto* pHigh = reinterpret_cast<ma_hishelf_node*>(m_Nodes[2]);
        ma_loshelf_node_uninit(pLow, nullptr);
        ma_peak_node_uninit(pMid, nullptr);
        ma_hishelf_node_uninit(pHigh, nullptr);
        PL_DEFAULT_DELETE(pLow);
        PL_DEFAULT_DELETE(pMid);
        PL_DEFAULT_DELETE(pHigh);
      }
      break;

    case plAudioEffectType::Delay:
      if (!m_Nodes.IsEmpty())
      {
        auto* pNode = reinterpret_cast<ma_delay_node*>(m_Nodes[0]);
        ma_delay_node_uninit(pNode, nullptr);
        PL_DEFAULT_DELETE(pNode);
      }
      break;

    case plAudioEffectType::Reverb:
      if (!m_Nodes.IsEmpty())
      {
        auto* pNode = reinterpret_cast<plMaReverbNode*>(m_Nodes[0]);
        plMiniAudioDsp::UninitReverbNode(pNode);
        PL_DEFAULT_DELETE(pNode);
      }
      break;

    case plAudioEffectType::Compressor:
    case plAudioEffectType::Limiter:
      if (!m_Nodes.IsEmpty())
      {
        auto* pNode = reinterpret_cast<plMaCompressorNode*>(m_Nodes[0]);
        plMiniAudioDsp::UninitCompressorNode(pNode);
        PL_DEFAULT_DELETE(pNode);
      }
      break;
  }

  m_Nodes.Clear();
  m_pEngine = nullptr;
}

void plMiniAudioEffect::SetParameter(plUInt32 uiParamIndex, float fValue)
{
  if (uiParamIndex >= plAudioEffectMaxParams || m_Nodes.IsEmpty() || m_pEngine == nullptr)
    return;

  if (m_fParams[uiParamIndex] == fValue)
    return;

  m_fParams[uiParamIndex] = fValue;

  switch (m_eType)
  {
    case plAudioEffectType::LowPass:
    case plAudioEffectType::HighPass:
    case plAudioEffectType::Eq3:
      ReinitFilters();
      break;

    case plAudioEffectType::Delay:
    {
      auto* pNode = reinterpret_cast<ma_delay_node*>(m_Nodes[0]);
      // delay time (param 0) cannot be changed live â€” buffer is fixed at init
      ma_delay_node_set_decay(pNode, plMath::Clamp(m_fParams[1], 0.0f, 0.99f));
      ma_delay_node_set_wet(pNode, plMath::Clamp(m_fParams[2], 0.0f, 1.0f));
      ma_delay_node_set_dry(pNode, plMath::Clamp(m_fParams[3], 0.0f, 1.0f));
      break;
    }

    case plAudioEffectType::Reverb:
    {
      auto* pNode = reinterpret_cast<plMaReverbNode*>(m_Nodes[0]);
      pNode->m_fRoomSize = m_fParams[0];
      pNode->m_fDamping = m_fParams[1];
      pNode->m_fWidth = m_fParams[2];
      pNode->m_fWet = m_fParams[3];
      pNode->m_fDry = m_fParams[4];
      break;
    }

    case plAudioEffectType::Compressor:
    {
      auto* pNode = reinterpret_cast<plMaCompressorNode*>(m_Nodes[0]);
      pNode->m_fThresholdDb = m_fParams[0];
      pNode->m_fRatio = m_fParams[1];
      pNode->m_fAttackMs = m_fParams[2];
      pNode->m_fReleaseMs = m_fParams[3];
      pNode->m_fMakeupDb = m_fParams[4];
      break;
    }

    case plAudioEffectType::Limiter:
    {
      auto* pNode = reinterpret_cast<plMaCompressorNode*>(m_Nodes[0]);
      pNode->m_fThresholdDb = m_fParams[0];
      pNode->m_fReleaseMs = m_fParams[1];
      break;
    }
  }
}

void plMiniAudioEffect::ReinitFilters()
{
  const plUInt32 uiChannels = ma_engine_get_channels(m_pEngine);
  const plUInt32 uiSampleRate = ma_engine_get_sample_rate(m_pEngine);

  switch (m_eType)
  {
    case plAudioEffectType::LowPass:
    {
      const ma_lpf_config cfg = ma_lpf_config_init(ma_format_f32, uiChannels, uiSampleRate, plMath::Clamp(m_fParams[0], 10.0f, 24000.0f), plMath::Clamp<plUInt32>(static_cast<plUInt32>(m_fParams[1]), 1, 8));
      ma_lpf_node_reinit(&cfg, reinterpret_cast<ma_lpf_node*>(m_Nodes[0]));
      break;
    }

    case plAudioEffectType::HighPass:
    {
      const ma_hpf_config cfg = ma_hpf_config_init(ma_format_f32, uiChannels, uiSampleRate, plMath::Clamp(m_fParams[0], 10.0f, 24000.0f), plMath::Clamp<plUInt32>(static_cast<plUInt32>(m_fParams[1]), 1, 8));
      ma_hpf_node_reinit(&cfg, reinterpret_cast<ma_hpf_node*>(m_Nodes[0]));
      break;
    }

    case plAudioEffectType::Eq3:
    {
      const ma_loshelf2_config lowCfg = ma_loshelf2_config_init(ma_format_f32, uiChannels, uiSampleRate, m_fParams[0], 0.707, plMath::Max(m_fParams[1], 10.0f));
      const ma_peak2_config midCfg = ma_peak2_config_init(ma_format_f32, uiChannels, uiSampleRate, m_fParams[2], plMath::Max(m_fParams[4], 0.1f), plMath::Max(m_fParams[3], 10.0f));
      const ma_hishelf2_config highCfg = ma_hishelf2_config_init(ma_format_f32, uiChannels, uiSampleRate, m_fParams[5], 0.707, plMath::Max(m_fParams[6], 10.0f));
      ma_loshelf_node_reinit(&lowCfg, reinterpret_cast<ma_loshelf_node*>(m_Nodes[0]));
      ma_peak_node_reinit(&midCfg, reinterpret_cast<ma_peak_node*>(m_Nodes[1]));
      ma_hishelf_node_reinit(&highCfg, reinterpret_cast<ma_hishelf_node*>(m_Nodes[2]));
      break;
    }

    default:
      break;
  }
}
