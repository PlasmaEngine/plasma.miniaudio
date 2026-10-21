#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/Math/Math.h>
#include <PlasmaAudioPlugin/Mixer/MiniAudioDspNodes.h>

/// A bypassed effect node copies its input to its output.
static void plMaPassThrough(const float** ppFramesIn, float** ppFramesOut, plUInt32 uiFrames, plUInt32 uiChannels)
{
  plMemoryUtils::Copy(ppFramesOut[0], ppFramesIn[0], static_cast<size_t>(uiFrames) * uiChannels);
}

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

// ---------------------------------------------------------------------------------------------
// PCM stream node
// ---------------------------------------------------------------------------------------------

namespace
{
  /// Pulls one source frame from the ring into pNode->m_CurFrame. Returns false on underrun.
  bool PullSourceFrame(plMaPcmStreamNode* pNode)
  {
    ma_uint32 uiFrames = 1;
    void* pRead = nullptr;
    if (ma_pcm_rb_acquire_read(&pNode->m_Ring, &uiFrames, &pRead) != MA_SUCCESS || uiFrames == 0)
      return false;

    const float* pSrc = static_cast<const float*>(pRead);
    const plUInt32 uiSrcCh = pNode->m_uiSourceChannels;
    const plUInt32 uiEngCh = pNode->m_uiEngineChannels;

    plMemoryUtils::Copy(pNode->m_PrevFrame, pNode->m_CurFrame, plMaPcmStreamNode::MaxChannels);

    // channel mapping: mono fans out, stereo (or more) maps by index, missing channels stay silent
    for (plUInt32 c = 0; c < uiEngCh && c < plMaPcmStreamNode::MaxChannels; ++c)
    {
      if (uiSrcCh == 1)
        pNode->m_CurFrame[c] = pSrc[0];
      else
        pNode->m_CurFrame[c] = c < uiSrcCh ? pSrc[c] : 0.0f;
    }

    ma_pcm_rb_commit_read(&pNode->m_Ring, 1);
    pNode->m_uiFramesRendered.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void DrainRing(plMaPcmStreamNode* pNode)
  {
    while (true)
    {
      ma_uint32 uiFrames = 65536;
      void* pRead = nullptr;
      if (ma_pcm_rb_acquire_read(&pNode->m_Ring, &uiFrames, &pRead) != MA_SUCCESS || uiFrames == 0)
        break;
      ma_pcm_rb_commit_read(&pNode->m_Ring, uiFrames);
    }
  }
} // namespace

static void plMaPcmStreamNodeProcess(ma_node* pNodeBase, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
  PL_IGNORE_UNUSED(ppFramesIn);
  PL_IGNORE_UNUSED(pFrameCountIn);

  auto* pNode = reinterpret_cast<plMaPcmStreamNode*>(pNodeBase);

  float* pOut = ppFramesOut[0];
  const plUInt32 uiFrames = *pFrameCountOut;
  const plUInt32 uiCh = pNode->m_uiEngineChannels;

  // a seek: throw away what was queued for the old position and restart the clock
  const plUInt32 uiFlush = pNode->m_uiFlushRequested.load(std::memory_order_acquire);
  if (uiFlush != pNode->m_uiFlushHandled)
  {
    DrainRing(pNode);
    pNode->m_uiFramesRendered.store(0, std::memory_order_relaxed);
    pNode->m_fPhase = 1.0;
    pNode->m_bPrimed = false;
    pNode->m_uiFlushHandled = uiFlush;
  }

  if (pNode->m_bPaused.load(std::memory_order_relaxed) || !pNode->m_bRingValid)
  {
    plMemoryUtils::ZeroFill(pOut, uiFrames * uiCh);
    return;
  }

  const float fVolume = pNode->m_fVolume.load(std::memory_order_relaxed);
  const double fRatio = static_cast<double>(pNode->m_fPitch.load(std::memory_order_relaxed)) * static_cast<double>(pNode->m_uiSourceSampleRate) / static_cast<double>(plMath::Max(1u, pNode->m_uiEngineSampleRate));

  for (plUInt32 f = 0; f < uiFrames; ++f)
  {
    // advance the source cursor as far as the phase asks; an empty ring holds the last frame and stops the clock
    bool bUnderrun = false;
    while (pNode->m_fPhase >= 1.0)
    {
      if (!PullSourceFrame(pNode))
      {
        bUnderrun = true;
        break;
      }
      pNode->m_bPrimed = true;
      pNode->m_fPhase -= 1.0;
    }

    float* pDst = pOut + static_cast<size_t>(f) * uiCh;
    if (bUnderrun || !pNode->m_bPrimed)
    {
      plMemoryUtils::ZeroFill(pDst, uiCh);
      continue;
    }

    const float fT = static_cast<float>(pNode->m_fPhase);
    for (plUInt32 c = 0; c < uiCh; ++c)
    {
      pDst[c] = plMath::Lerp(pNode->m_PrevFrame[c], pNode->m_CurFrame[c], fT) * fVolume;
    }
    pNode->m_fPhase += fRatio;
  }
}

// no inputs, one output; continuous processing so the graph pulls it even without an input signal
static ma_node_vtable g_plMaPcmStreamNodeVTable = {plMaPcmStreamNodeProcess, nullptr, 0, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING};

plResult plMiniAudioDsp::InitPcmStreamNode(ma_engine* pEngine, plMaPcmStreamNode* pNode, plUInt32 uiSourceChannels, plUInt32 uiSourceSampleRate, plUInt32 uiRingFrames)
{
  pNode->m_uiEngineChannels = plMath::Min(ma_engine_get_channels(pEngine), plMaPcmStreamNode::MaxChannels);
  pNode->m_uiEngineSampleRate = ma_engine_get_sample_rate(pEngine);
  pNode->m_uiSourceChannels = plMath::Clamp(uiSourceChannels, 1u, plMaPcmStreamNode::MaxChannels);
  pNode->m_uiSourceSampleRate = plMath::Max(1u, uiSourceSampleRate);

  if (ma_pcm_rb_init(ma_format_f32, pNode->m_uiSourceChannels, plMath::Max(1024u, uiRingFrames), nullptr, nullptr, &pNode->m_Ring) != MA_SUCCESS)
    return PL_FAILURE;
  pNode->m_bRingValid = true;

  ma_node_config cfg = ma_node_config_init();
  cfg.vtable = &g_plMaPcmStreamNodeVTable;
  cfg.pInputChannels = nullptr;
  cfg.pOutputChannels = &pNode->m_uiEngineChannels;

  if (ma_node_init(ma_engine_get_node_graph(pEngine), &cfg, nullptr, &pNode->m_Base) != MA_SUCCESS)
  {
    ma_pcm_rb_uninit(&pNode->m_Ring);
    pNode->m_bRingValid = false;
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

void plMiniAudioDsp::UninitPcmStreamNode(plMaPcmStreamNode* pNode)
{
  ma_node_uninit(&pNode->m_Base, nullptr);
  if (pNode->m_bRingValid)
  {
    ma_pcm_rb_uninit(&pNode->m_Ring);
    pNode->m_bRingValid = false;
  }
}

plUInt32 plMiniAudioDsp::WritePcmStreamFrames(plMaPcmStreamNode* pNode, const float* pInterleaved, plUInt32 uiFrames)
{
  if (!pNode->m_bRingValid || uiFrames == 0)
    return 0;

  const plUInt32 uiCh = pNode->m_uiSourceChannels;
  plUInt32 uiWritten = 0;

  // the ring may wrap: write in up to two pieces
  for (plUInt32 uiPiece = 0; uiPiece < 2 && uiWritten < uiFrames; ++uiPiece)
  {
    ma_uint32 uiAvail = uiFrames - uiWritten;
    void* pWrite = nullptr;
    if (ma_pcm_rb_acquire_write(&pNode->m_Ring, &uiAvail, &pWrite) != MA_SUCCESS || uiAvail == 0)
      break;

    plMemoryUtils::Copy(static_cast<float*>(pWrite), pInterleaved + static_cast<size_t>(uiWritten) * uiCh, uiAvail * uiCh);
    ma_pcm_rb_commit_write(&pNode->m_Ring, uiAvail);
    uiWritten += uiAvail;
  }

  return uiWritten;
}

plUInt32 plMiniAudioDsp::GetPcmStreamFreeFrames(const plMaPcmStreamNode* pNode)
{
  if (!pNode->m_bRingValid)
    return 0;
  return ma_pcm_rb_available_write(const_cast<ma_pcm_rb*>(&pNode->m_Ring));
}

static void plMaClockNodeProcess(ma_node* pNodeBase, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
  PL_IGNORE_UNUSED(ppFramesIn);
  PL_IGNORE_UNUSED(pFrameCountIn);

  // the whole request is "produced", which is what lets the endpoint advance
  const auto* pNode = reinterpret_cast<const plMaClockNode*>(pNodeBase);
  if (ppFramesOut != nullptr && ppFramesOut[0] != nullptr)
  {
    ma_silence_pcm_frames(ppFramesOut[0], *pFrameCountOut, ma_format_f32, pNode->m_uiChannels);
  }
}

// no inputs, one output; flagged silent so the endpoint does not mix it
static ma_node_vtable g_plMaClockNodeVTable = {plMaClockNodeProcess, nullptr, 0, 1, MA_NODE_FLAG_SILENT_OUTPUT};

plResult plMiniAudioDsp::InitClockNode(ma_engine* pEngine, plMaClockNode* pNode)
{
  pNode->m_bValid = false;
  pNode->m_uiChannels = ma_engine_get_channels(pEngine);

  ma_node_config cfg = ma_node_config_init();
  cfg.vtable = &g_plMaClockNodeVTable;
  cfg.pInputChannels = nullptr;
  cfg.pOutputChannels = &pNode->m_uiChannels;

  ma_node_graph* pGraph = ma_engine_get_node_graph(pEngine);
  if (ma_node_init(pGraph, &cfg, nullptr, &pNode->m_Base) != MA_SUCCESS)
    return PL_FAILURE;

  if (ma_node_attach_output_bus(&pNode->m_Base, 0, ma_node_graph_get_endpoint(pGraph), 0) != MA_SUCCESS)
  {
    ma_node_uninit(&pNode->m_Base, nullptr);
    return PL_FAILURE;
  }

  pNode->m_bValid = true;
  return PL_SUCCESS;
}

void plMiniAudioDsp::UninitClockNode(plMaClockNode* pNode)
{
  if (!pNode->m_bValid)
    return;

  ma_node_uninit(&pNode->m_Base, nullptr);
  pNode->m_bValid = false;
}

// ---------------------------------------------------------------------------------------------
// Test signal node
// ---------------------------------------------------------------------------------------------

static void plMaTestSignalNodeProcess(ma_node* pNodeBase, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
  PL_IGNORE_UNUSED(ppFramesIn);
  PL_IGNORE_UNUSED(pFrameCountIn);

  auto* pNode = reinterpret_cast<plMaTestSignalNode*>(pNodeBase);

  float* pOut = ppFramesOut[0];
  const plUInt32 uiFrames = *pFrameCountOut;
  const plUInt32 uiChannels = pNode->m_uiChannels;
  const double fSampleRate = static_cast<double>(pNode->m_uiSampleRate);
  const double fRampSeconds = 0.02;

  for (plUInt32 f = 0; f < uiFrames; ++f)
  {
    // burst envelope with short ramps at both ends
    const double fTime = plMath::Mod(static_cast<double>(pNode->m_uiFrame++) / fSampleRate, static_cast<double>(pNode->m_fPeriodSeconds));
    const double fEnvelope = plMath::Clamp(plMath::Min(fTime, static_cast<double>(pNode->m_fBurstSeconds) - fTime) / fRampSeconds, 0.0, 1.0);

    // white noise (xorshift) through an economy pink filter
    pNode->m_uiNoiseState ^= pNode->m_uiNoiseState << 13;
    pNode->m_uiNoiseState ^= pNode->m_uiNoiseState >> 17;
    pNode->m_uiNoiseState ^= pNode->m_uiNoiseState << 5;
    const float fWhite = static_cast<float>(pNode->m_uiNoiseState) / 4294967295.0f * 2.0f - 1.0f;

    pNode->m_fPink[0] = 0.99765f * pNode->m_fPink[0] + fWhite * 0.0990460f;
    pNode->m_fPink[1] = 0.96300f * pNode->m_fPink[1] + fWhite * 0.2965164f;
    pNode->m_fPink[2] = 0.57000f * pNode->m_fPink[2] + fWhite * 1.0526913f;
    const float fPink = (pNode->m_fPink[0] + pNode->m_fPink[1] + pNode->m_fPink[2] + fWhite * 0.1848f) * 0.25f;

    const float fSample = fPink * pNode->m_fLevel * static_cast<float>(fEnvelope);
    for (plUInt32 c = 0; c < uiChannels; ++c)
      pOut[f * uiChannels + c] = fSample;
  }
}

static ma_node_vtable g_plMaTestSignalNodeVTable = {plMaTestSignalNodeProcess, nullptr, 0, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING};

plResult plMiniAudioDsp::InitTestSignalNode(ma_engine* pEngine, plMaTestSignalNode* pNode)
{
  pNode->m_uiChannels = ma_engine_get_channels(pEngine);
  pNode->m_uiSampleRate = ma_engine_get_sample_rate(pEngine);

  ma_node_config cfg = ma_node_config_init();
  cfg.vtable = &g_plMaTestSignalNodeVTable;
  cfg.pOutputChannels = &pNode->m_uiChannels;

  if (ma_node_init(ma_engine_get_node_graph(pEngine), &cfg, nullptr, &pNode->m_Base) != MA_SUCCESS)
    return PL_FAILURE;

  pNode->m_bValid = true;
  return PL_SUCCESS;
}

void plMiniAudioDsp::UninitTestSignalNode(plMaTestSignalNode* pNode)
{
  if (!pNode->m_bValid)
    return;

  ma_node_uninit(&pNode->m_Base, nullptr);
  pNode->m_bValid = false;
}

ma_uint64 plMiniAudioDsp::RenderWithoutDevice(ma_engine* pEngine, plTime delta, double& ref_fFrameRemainder, plDynamicArray<float>& ref_scratch)
{
  constexpr ma_uint32 uiChunkFrames = 1024;

  const ma_uint32 uiSampleRate = ma_engine_get_sample_rate(pEngine);
  const ma_uint32 uiChannels = ma_engine_get_channels(pEngine);
  if (uiSampleRate == 0 || uiChannels == 0)
    return 0;

  const double fFrames = plMath::Clamp(delta.GetSeconds(), 0.0, 1.0) * static_cast<double>(uiSampleRate) + ref_fFrameRemainder;
  const ma_uint64 uiFrames = static_cast<ma_uint64>(fFrames);
  ref_fFrameRemainder = fFrames - static_cast<double>(uiFrames);

  ref_scratch.SetCountUninitialized(uiChunkFrames * uiChannels);

  ma_uint64 uiRendered = 0;
  while (uiRendered < uiFrames)
  {
    const ma_uint64 uiToRead = plMath::Min<ma_uint64>(uiFrames - uiRendered, uiChunkFrames);

    ma_uint64 uiRead = 0;
    if (ma_engine_read_pcm_frames(pEngine, ref_scratch.GetData(), uiToRead, &uiRead) != MA_SUCCESS || uiRead == 0)
      break;

    uiRendered += uiRead;
  }

  return uiRendered;
}

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

  if (pReverb->m_bBypass.load(std::memory_order_relaxed))
  {
    plMaPassThrough(ppFramesIn, ppFramesOut, uiFrames, uiChannels);
    return;
  }

  const float fFeedback = 0.7f + 0.28f * plMath::Clamp(pReverb->m_fRoomSize.load(std::memory_order_relaxed), 0.0f, 1.0f);
  const float fDamp1 = plMath::Clamp(pReverb->m_fDamping.load(std::memory_order_relaxed), 0.0f, 1.0f) * 0.4f;
  const float fDamp2 = 1.0f - fDamp1;
  const float fWet = pReverb->m_fWet.load(std::memory_order_relaxed);
  const float fDry = pReverb->m_fDry.load(std::memory_order_relaxed);
  const float fWidth = plMath::Clamp(pReverb->m_fWidth.load(std::memory_order_relaxed), 0.0f, 1.0f);

  // Freeverb stereo width: each channel of a pair gets its own reverb times wet1 plus its partner's times wet2
  const float fWet1 = fWet * (0.5f + 0.5f * fWidth);
  const float fWet2 = fWet * (0.5f - 0.5f * fWidth);

  constexpr plUInt32 MaxPairedChannels = 16;
  float fReverbFrame[MaxPairedChannels];

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

      if (c < MaxPairedChannels)
      {
        fReverbFrame[c] = fReverbOut;
      }
      else
      {
        pOut[f * uiChannels + c] = fInput * fDry + fReverbOut * fWet;
      }
    }

    // channels 0/1, 2/3, ... are pairs; a channel without a partner gets its own reverb only
    for (plUInt32 c = 0; c < plMath::Min(uiChannels, MaxPairedChannels); ++c)
    {
      const plUInt32 uiPartner = c ^ 1u;
      const float fInput = pIn[f * uiChannels + c];

      if (uiPartner < uiChannels && uiPartner < MaxPairedChannels)
      {
        pOut[f * uiChannels + c] = fInput * fDry + fReverbFrame[c] * fWet1 + fReverbFrame[uiPartner] * fWet2;
      }
      else
      {
        pOut[f * uiChannels + c] = fInput * fDry + fReverbFrame[c] * fWet;
      }
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

  if (pComp->m_bBypass.load(std::memory_order_relaxed))
  {
    plMaPassThrough(ppFramesIn, ppFramesOut, uiFrames, uiChannels);
    pComp->m_fEnvelope = 0.0f;
    pComp->m_fDuckGainDb = 0.0f;
    pComp->m_fGainReductionDb.store(0.0f, std::memory_order_relaxed);
    return;
  }

  // the level is measured on the key when there is one, the gain applies to the input
  const float* pKey = (pComp->m_bHasKeyInput && ppFramesIn[1] != nullptr) ? ppFramesIn[1] : pIn;

  const float fAttackCoef = plMath::Exp(-1.0f / (plMath::Max(pComp->m_fAttackMs.load(std::memory_order_relaxed), 0.02f) * 0.001f * fSampleRate));
  const float fReleaseCoef = plMath::Exp(-1.0f / (plMath::Max(pComp->m_fReleaseMs.load(std::memory_order_relaxed), 1.0f) * 0.001f * fSampleRate));
  const float fThresholdLin = plAudioMixerUtils::DbToLinear(pComp->m_fThresholdDb.load(std::memory_order_relaxed));

  float fEnvelope = pComp->m_fEnvelope;
  float fMinGain = 1.0f;

  if (pComp->m_eMode == plMaCompressorNode::Mode::Ducker)
  {
    // the key's envelope opens the duck; the duck itself moves with the attack and release times
    const float fKeyAttackCoef = plMath::Exp(-1.0f / (0.002f * fSampleRate));
    const float fKeyReleaseCoef = plMath::Exp(-1.0f / (0.060f * fSampleRate));
    const float fDepthDb = plMath::Min(pComp->m_fDepthDb.load(std::memory_order_relaxed), 0.0f);
    float fDuckDb = pComp->m_fDuckGainDb;

    for (plUInt32 f = 0; f < uiFrames; ++f)
    {
      float fLevel = 0.0f;
      for (plUInt32 c = 0; c < uiChannels; ++c)
        fLevel = plMath::Max(fLevel, plMath::Abs(pKey[f * uiChannels + c]));

      const float fKeyCoef = (fLevel > fEnvelope) ? fKeyAttackCoef : fKeyReleaseCoef;
      fEnvelope = fKeyCoef * fEnvelope + (1.0f - fKeyCoef) * fLevel;

      const float fTargetDb = (fEnvelope > fThresholdLin) ? fDepthDb : 0.0f;
      const float fCoef = (fTargetDb < fDuckDb) ? fAttackCoef : fReleaseCoef;
      fDuckDb = fCoef * fDuckDb + (1.0f - fCoef) * fTargetDb;

      const float fGain = plMath::Pow(10.0f, fDuckDb / 20.0f);
      fMinGain = plMath::Min(fMinGain, fGain);

      for (plUInt32 c = 0; c < uiChannels; ++c)
        pOut[f * uiChannels + c] = pIn[f * uiChannels + c] * fGain;
    }

    pComp->m_fDuckGainDb = fDuckDb;
  }
  else
  {
    const float fMakeupLin = plAudioMixerUtils::DbToLinear(pComp->m_fMakeupDb.load(std::memory_order_relaxed));
    const float fInvRatio = (pComp->m_eMode == plMaCompressorNode::Mode::Limiter) ? 0.0f : (1.0f / plMath::Max(pComp->m_fRatio.load(std::memory_order_relaxed), 1.0f));

    for (plUInt32 f = 0; f < uiFrames; ++f)
    {
      float fLevel = 0.0f;
      for (plUInt32 c = 0; c < uiChannels; ++c)
        fLevel = plMath::Max(fLevel, plMath::Abs(pKey[f * uiChannels + c]));

      const float fCoef = (fLevel > fEnvelope) ? fAttackCoef : fReleaseCoef;
      fEnvelope = fCoef * fEnvelope + (1.0f - fCoef) * fLevel;

      float fGain = 1.0f;
      if (fEnvelope > fThresholdLin && fEnvelope > 0.0f)
      {
        // gain reduction so that (threshold + over * invRatio) is the resulting level
        const float fTarget = fThresholdLin * plMath::Pow(fEnvelope / fThresholdLin, fInvRatio);
        fGain = fTarget / fEnvelope;
      }

      fMinGain = plMath::Min(fMinGain, fGain);

      const float fTotalGain = fGain * fMakeupLin;
      for (plUInt32 c = 0; c < uiChannels; ++c)
        pOut[f * uiChannels + c] = pIn[f * uiChannels + c] * fTotalGain;
    }
  }

  pComp->m_fEnvelope = fEnvelope;
  pComp->m_fGainReductionDb.store(fMinGain < 1.0f ? 20.0f * plMath::Log10(plMath::Max(fMinGain, 0.0001f)) : 0.0f, std::memory_order_relaxed);
}

static ma_node_vtable g_plMaCompressorNodeVTable = {plMaCompressorNodeProcess, nullptr, MA_NODE_BUS_COUNT_UNKNOWN, 1, 0};

plResult plMiniAudioDsp::InitCompressorNode(ma_engine* pEngine, plMaCompressorNode* pNode, plMaCompressorNode::Mode mode, bool bHasKeyInput)
{
  pNode->m_uiChannels = ma_engine_get_channels(pEngine);
  pNode->m_uiSampleRate = ma_engine_get_sample_rate(pEngine);
  pNode->m_eMode = mode;
  pNode->m_bHasKeyInput = bHasKeyInput;
  pNode->m_InputChannels[0] = pNode->m_uiChannels;
  pNode->m_InputChannels[1] = pNode->m_uiChannels;

  ma_node_config cfg = ma_node_config_init();
  cfg.vtable = &g_plMaCompressorNodeVTable;
  cfg.inputBusCount = bHasKeyInput ? 2 : 1;
  cfg.pInputChannels = pNode->m_InputChannels;
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
// Filter node
// ---------------------------------------------------------------------------------------------

namespace
{
  plInt32 FloatToBits(float fValue)
  {
    plInt32 iBits = 0;
    plMemoryUtils::RawByteCopy(&iBits, &fValue, sizeof(float));
    return iBits;
  }

  float BitsToFloat(plInt32 iBits)
  {
    float fValue = 0.0f;
    plMemoryUtils::RawByteCopy(&fValue, &iBits, sizeof(float));
    return fValue;
  }

  /// Heap bytes the filter needs at the given order (the order only matters for low and high pass).
  size_t GetFilterHeapSize(plMaFilterNode::Type type, plUInt32 uiChannels, plUInt32 uiSampleRate, plUInt32 uiOrder)
  {
    size_t uiSize = 0;

    switch (type)
    {
      case plMaFilterNode::Type::LowPass:
      {
        const ma_lpf_config cfg = ma_lpf_config_init(ma_format_f32, uiChannels, uiSampleRate, 1000.0, uiOrder);
        ma_lpf_get_heap_size(&cfg, &uiSize);
        break;
      }
      case plMaFilterNode::Type::HighPass:
      {
        const ma_hpf_config cfg = ma_hpf_config_init(ma_format_f32, uiChannels, uiSampleRate, 1000.0, uiOrder);
        ma_hpf_get_heap_size(&cfg, &uiSize);
        break;
      }
      case plMaFilterNode::Type::LowShelf:
      {
        const ma_loshelf2_config cfg = ma_loshelf2_config_init(ma_format_f32, uiChannels, uiSampleRate, 0.0, 0.707, 1000.0);
        ma_loshelf2_get_heap_size(&cfg, &uiSize);
        break;
      }
      case plMaFilterNode::Type::Peak:
      {
        const ma_peak2_config cfg = ma_peak2_config_init(ma_format_f32, uiChannels, uiSampleRate, 0.0, 1.0, 1000.0);
        ma_peak2_get_heap_size(&cfg, &uiSize);
        break;
      }
      case plMaFilterNode::Type::HighShelf:
      {
        const ma_hishelf2_config cfg = ma_hishelf2_config_init(ma_format_f32, uiChannels, uiSampleRate, 0.0, 0.707, 1000.0);
        ma_hishelf2_get_heap_size(&cfg, &uiSize);
        break;
      }
    }

    return uiSize;
  }

  /// Brings the filter in line with the stored settings. With bInit the filter is initialized; otherwise the
  /// coefficients are recomputed (state kept) or, for a changed low/high pass order, the filter is initialized again in
  /// its heap. Called on the device thread, or before the node is attached.
  ma_result ApplyFilterSettings(plMaFilterNode* pNode, bool bInit)
  {
    const double fNyquistGuard = 0.49 * static_cast<double>(pNode->m_uiSampleRate);
    const double fFrequency = plMath::Clamp(static_cast<double>(BitsToFloat(pNode->m_iFrequencyBits)), 10.0, fNyquistGuard);
    const double fGainDb = BitsToFloat(pNode->m_iGainDbBits);
    const double fQ = plMath::Max(static_cast<double>(BitsToFloat(pNode->m_iQBits)), 0.1);
    const plUInt32 uiOrder = plMath::Clamp<plUInt32>(static_cast<plUInt32>(static_cast<plInt32>(pNode->m_iOrder)), 1u, pNode->m_uiMaxOrder);

    switch (pNode->m_eType)
    {
      case plMaFilterNode::Type::LowPass:
      {
        const ma_lpf_config cfg = ma_lpf_config_init(ma_format_f32, pNode->m_uiChannels, pNode->m_uiSampleRate, fFrequency, uiOrder);
        if (!bInit && uiOrder == pNode->m_uiAppliedOrder)
          return ma_lpf_reinit(&cfg, &pNode->m_Lpf);

        if (!bInit)
          ma_lpf_uninit(&pNode->m_Lpf, nullptr);

        pNode->m_uiAppliedOrder = uiOrder;
        return ma_lpf_init_preallocated(&cfg, pNode->m_pHeap, &pNode->m_Lpf);
      }

      case plMaFilterNode::Type::HighPass:
      {
        const ma_hpf_config cfg = ma_hpf_config_init(ma_format_f32, pNode->m_uiChannels, pNode->m_uiSampleRate, fFrequency, uiOrder);
        if (!bInit && uiOrder == pNode->m_uiAppliedOrder)
          return ma_hpf_reinit(&cfg, &pNode->m_Hpf);

        if (!bInit)
          ma_hpf_uninit(&pNode->m_Hpf, nullptr);

        pNode->m_uiAppliedOrder = uiOrder;
        return ma_hpf_init_preallocated(&cfg, pNode->m_pHeap, &pNode->m_Hpf);
      }

      case plMaFilterNode::Type::LowShelf:
      {
        const ma_loshelf2_config cfg = ma_loshelf2_config_init(ma_format_f32, pNode->m_uiChannels, pNode->m_uiSampleRate, fGainDb, 0.707, fFrequency);
        return bInit ? ma_loshelf2_init_preallocated(&cfg, pNode->m_pHeap, &pNode->m_LoShelf) : ma_loshelf2_reinit(&cfg, &pNode->m_LoShelf);
      }

      case plMaFilterNode::Type::Peak:
      {
        const ma_peak2_config cfg = ma_peak2_config_init(ma_format_f32, pNode->m_uiChannels, pNode->m_uiSampleRate, fGainDb, fQ, fFrequency);
        return bInit ? ma_peak2_init_preallocated(&cfg, pNode->m_pHeap, &pNode->m_Peak) : ma_peak2_reinit(&cfg, &pNode->m_Peak);
      }

      case plMaFilterNode::Type::HighShelf:
      {
        const ma_hishelf2_config cfg = ma_hishelf2_config_init(ma_format_f32, pNode->m_uiChannels, pNode->m_uiSampleRate, fGainDb, 0.707, fFrequency);
        return bInit ? ma_hishelf2_init_preallocated(&cfg, pNode->m_pHeap, &pNode->m_HiShelf) : ma_hishelf2_reinit(&cfg, &pNode->m_HiShelf);
      }
    }

    return MA_INVALID_ARGS;
  }
} // namespace

static void plMaFilterNodeProcess(ma_node* pNodeBase, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
  PL_IGNORE_UNUSED(pFrameCountIn);

  auto* pNode = reinterpret_cast<plMaFilterNode*>(pNodeBase);

  const plInt32 iGeneration = pNode->m_iGeneration;
  if (iGeneration != pNode->m_iAppliedGeneration)
  {
    // a rejected setting (e.g. an unsupported frequency) keeps the previous coefficients
    ApplyFilterSettings(pNode, false);
    pNode->m_iAppliedGeneration = iGeneration;
  }

  const ma_uint32 uiFrames = *pFrameCountOut;

  if (pNode->m_iBypass != 0)
  {
    plMaPassThrough(ppFramesIn, ppFramesOut, uiFrames, pNode->m_uiChannels);
    return;
  }

  switch (pNode->m_eType)
  {
    case plMaFilterNode::Type::LowPass:
      ma_lpf_process_pcm_frames(&pNode->m_Lpf, ppFramesOut[0], ppFramesIn[0], uiFrames);
      break;
    case plMaFilterNode::Type::HighPass:
      ma_hpf_process_pcm_frames(&pNode->m_Hpf, ppFramesOut[0], ppFramesIn[0], uiFrames);
      break;
    case plMaFilterNode::Type::LowShelf:
      ma_loshelf2_process_pcm_frames(&pNode->m_LoShelf, ppFramesOut[0], ppFramesIn[0], uiFrames);
      break;
    case plMaFilterNode::Type::Peak:
      ma_peak2_process_pcm_frames(&pNode->m_Peak, ppFramesOut[0], ppFramesIn[0], uiFrames);
      break;
    case plMaFilterNode::Type::HighShelf:
      ma_hishelf2_process_pcm_frames(&pNode->m_HiShelf, ppFramesOut[0], ppFramesIn[0], uiFrames);
      break;
  }
}

static ma_node_vtable g_plMaFilterNodeVTable = {plMaFilterNodeProcess, nullptr, 1, 1, 0};

plResult plMiniAudioDsp::InitFilterNode(ma_engine* pEngine, plMaFilterNode* pNode, plMaFilterNode::Type type, plUInt32 uiMaxOrder, float fFrequency, plUInt32 uiOrder, float fGainDb, float fQ, const ma_allocation_callbacks* pCallbacks)
{
  pNode->m_bValid = false;
  pNode->m_eType = type;
  pNode->m_uiChannels = ma_engine_get_channels(pEngine);
  pNode->m_uiSampleRate = ma_engine_get_sample_rate(pEngine);
  pNode->m_uiMaxOrder = plMath::Clamp<plUInt32>(uiMaxOrder, 1u, plMaFilterNode::MaxOrder);
  pNode->m_AllocationCallbacks = (pCallbacks != nullptr) ? *pCallbacks : ma_allocation_callbacks{};
  pNode->m_iAppliedGeneration = 0;
  pNode->m_iGeneration = 0;
  pNode->m_uiAppliedOrder = 0;

  pNode->m_fLastFrequency = fFrequency;
  pNode->m_fLastGainDb = fGainDb;
  pNode->m_fLastQ = fQ;
  pNode->m_uiLastOrder = plMath::Clamp<plUInt32>(uiOrder, 1u, pNode->m_uiMaxOrder);
  pNode->m_iFrequencyBits = FloatToBits(fFrequency);
  pNode->m_iGainDbBits = FloatToBits(fGainDb);
  pNode->m_iQBits = FloatToBits(fQ);
  pNode->m_iOrder = static_cast<plInt32>(pNode->m_uiLastOrder);

  // the heap is sized for the maximum order, so a later order change re-initializes in place
  const size_t uiHeapSize = GetFilterHeapSize(type, pNode->m_uiChannels, pNode->m_uiSampleRate, pNode->m_uiMaxOrder);
  if (uiHeapSize <= plMaFilterNode::InlineHeapBytes)
  {
    pNode->m_pHeap = pNode->m_InlineHeap;
  }
  else
  {
    pNode->m_pHeap = (pCallbacks != nullptr && pCallbacks->onMalloc != nullptr) ? pCallbacks->onMalloc(uiHeapSize, pCallbacks->pUserData) : ma_malloc(uiHeapSize, nullptr);
    if (pNode->m_pHeap == nullptr)
      return PL_FAILURE;
  }
  plMemoryUtils::ZeroFill(static_cast<plUInt8*>(pNode->m_pHeap), uiHeapSize);

  auto FreeHeap = [&]()
  {
    if (pNode->m_pHeap != nullptr && pNode->m_pHeap != pNode->m_InlineHeap)
    {
      if (pNode->m_AllocationCallbacks.onFree != nullptr)
        pNode->m_AllocationCallbacks.onFree(pNode->m_pHeap, pNode->m_AllocationCallbacks.pUserData);
      else
        ma_free(pNode->m_pHeap, nullptr);
    }
    pNode->m_pHeap = nullptr;
  };

  if (ApplyFilterSettings(pNode, true) != MA_SUCCESS)
  {
    FreeHeap();
    return PL_FAILURE;
  }

  ma_node_config cfg = ma_node_config_init();
  cfg.vtable = &g_plMaFilterNodeVTable;
  cfg.pInputChannels = &pNode->m_uiChannels;
  cfg.pOutputChannels = &pNode->m_uiChannels;

  if (ma_node_init(ma_engine_get_node_graph(pEngine), &cfg, pCallbacks, &pNode->m_Base) != MA_SUCCESS)
  {
    FreeHeap();
    return PL_FAILURE;
  }

  pNode->m_bValid = true;
  return PL_SUCCESS;
}

void plMiniAudioDsp::UninitFilterNode(plMaFilterNode* pNode)
{
  if (!pNode->m_bValid)
    return;

  const ma_allocation_callbacks* pCallbacks = (pNode->m_AllocationCallbacks.onFree != nullptr) ? &pNode->m_AllocationCallbacks : nullptr;
  ma_node_uninit(&pNode->m_Base, pCallbacks);

  if (pNode->m_pHeap != nullptr && pNode->m_pHeap != pNode->m_InlineHeap)
  {
    if (pCallbacks != nullptr)
      pCallbacks->onFree(pNode->m_pHeap, pCallbacks->pUserData);
    else
      ma_free(pNode->m_pHeap, nullptr);
  }

  pNode->m_pHeap = nullptr;
  pNode->m_bValid = false;
}

void plMiniAudioDsp::SetFilter(plMaFilterNode* pNode, float fFrequency, plUInt32 uiOrder, float fGainDb, float fQ)
{
  if (!pNode->m_bValid)
    return;

  uiOrder = plMath::Clamp<plUInt32>(uiOrder, 1u, pNode->m_uiMaxOrder);

  // frequency changes below 0.1 % are inaudible; skipping them keeps the device thread from recomputing for nothing
  const bool bFrequencyChanged = plMath::Abs(fFrequency - pNode->m_fLastFrequency) > 0.001f * plMath::Max(pNode->m_fLastFrequency, 1.0f);
  if (!bFrequencyChanged && uiOrder == pNode->m_uiLastOrder && fGainDb == pNode->m_fLastGainDb && fQ == pNode->m_fLastQ)
    return;

  pNode->m_fLastFrequency = fFrequency;
  pNode->m_fLastGainDb = fGainDb;
  pNode->m_fLastQ = fQ;
  pNode->m_uiLastOrder = uiOrder;

  pNode->m_iFrequencyBits = FloatToBits(fFrequency);
  pNode->m_iGainDbBits = FloatToBits(fGainDb);
  pNode->m_iQBits = FloatToBits(fQ);
  pNode->m_iOrder = static_cast<plInt32>(uiOrder);

  // published last: the device thread reads the settings after it saw the new generation
  pNode->m_iGeneration.Increment();
}

// ---------------------------------------------------------------------------------------------
// Delay node
// ---------------------------------------------------------------------------------------------

static void plMaDelayNodeProcess(ma_node* pNodeBase, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
  PL_IGNORE_UNUSED(pFrameCountIn);

  auto* pNode = reinterpret_cast<plMaDelayNode*>(pNodeBase);

  const float* pIn = ppFramesIn[0];
  float* pOut = ppFramesOut[0];
  const plUInt32 uiFrames = *pFrameCountOut;
  const plUInt32 uiChannels = pNode->m_uiChannels;
  const plUInt32 uiSize = pNode->m_uiMaxDelayFrames;

  if (pNode->m_bBypass.load(std::memory_order_relaxed))
  {
    plMaPassThrough(ppFramesIn, ppFramesOut, uiFrames, uiChannels);
    return;
  }

  const plUInt32 uiDelay = plMath::Clamp<plUInt32>(pNode->m_uiDelayFrames.load(std::memory_order_relaxed), 1u, uiSize - 1);
  const float fFeedback = plMath::Clamp(pNode->m_fFeedback.load(std::memory_order_relaxed), 0.0f, 0.99f);
  const float fWet = plMath::Clamp(pNode->m_fWet.load(std::memory_order_relaxed), 0.0f, 1.0f);
  const float fDry = plMath::Clamp(pNode->m_fDry.load(std::memory_order_relaxed), 0.0f, 1.0f);

  for (plUInt32 f = 0; f < uiFrames; ++f)
  {
    const plUInt32 uiWrite = pNode->m_uiWriteFrame;
    const plUInt32 uiRead = (uiWrite + uiSize - uiDelay) % uiSize;

    for (plUInt32 c = 0; c < uiChannels; ++c)
    {
      const float fInput = pIn[f * uiChannels + c];
      const float fDelayed = pNode->m_pBuffer[uiRead * uiChannels + c];

      pNode->m_pBuffer[uiWrite * uiChannels + c] = fInput + fDelayed * fFeedback;
      pOut[f * uiChannels + c] = fInput * fDry + fDelayed * fWet;
    }

    pNode->m_uiWriteFrame = (uiWrite + 1) % uiSize;
  }
}

static ma_node_vtable g_plMaDelayNodeVTable = {plMaDelayNodeProcess, nullptr, 1, 1, 0};

plResult plMiniAudioDsp::InitDelayNode(ma_engine* pEngine, plMaDelayNode* pNode, plUInt32 uiMaxDelayFrames)
{
  pNode->m_uiChannels = ma_engine_get_channels(pEngine);
  pNode->m_uiSampleRate = ma_engine_get_sample_rate(pEngine);
  pNode->m_uiMaxDelayFrames = plMath::Max<plUInt32>(uiMaxDelayFrames, 2) + 1;
  pNode->m_uiWriteFrame = 0;

  auto buffer = PL_DEFAULT_NEW_ARRAY(float, pNode->m_uiMaxDelayFrames * pNode->m_uiChannels);
  plMemoryUtils::ZeroFill(buffer.GetPtr(), buffer.GetCount());
  pNode->m_pBuffer = buffer.GetPtr();

  ma_node_config cfg = ma_node_config_init();
  cfg.vtable = &g_plMaDelayNodeVTable;
  cfg.pInputChannels = &pNode->m_uiChannels;
  cfg.pOutputChannels = &pNode->m_uiChannels;

  if (ma_node_init(ma_engine_get_node_graph(pEngine), &cfg, nullptr, &pNode->m_Base) != MA_SUCCESS)
  {
    PL_DEFAULT_DELETE_ARRAY(buffer);
    pNode->m_pBuffer = nullptr;
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

void plMiniAudioDsp::UninitDelayNode(plMaDelayNode* pNode)
{
  if (pNode->m_pBuffer == nullptr)
    return;

  ma_node_uninit(&pNode->m_Base, nullptr);

  PL_DEFAULT_DELETE_ARRAY(plArrayPtr<float>(pNode->m_pBuffer, pNode->m_uiMaxDelayFrames * pNode->m_uiChannels));
  pNode->m_pBuffer = nullptr;
}

// ---------------------------------------------------------------------------------------------
// plMiniAudioEffect
// ---------------------------------------------------------------------------------------------

plResult plMiniAudioEffect::Create(ma_engine* pEngine, const plAudioEffectDesc& desc, bool bHasKeyInput)
{
  m_pEngine = pEngine;
  m_eType = desc.m_eType;
  m_bHasKeyInput = false;
  plMemoryUtils::Copy(m_fParams, desc.m_fParams, plAudioEffectMaxParams);

  const plUInt32 uiSampleRate = ma_engine_get_sample_rate(pEngine);

  switch (m_eType)
  {
    case plAudioEffectType::LowPass:
    case plAudioEffectType::HighPass:
    {
      const plMaFilterNode::Type type = (m_eType == plAudioEffectType::LowPass) ? plMaFilterNode::Type::LowPass : plMaFilterNode::Type::HighPass;
      plMaFilterNode* pNode = PL_DEFAULT_NEW(plMaFilterNode);
      if (plMiniAudioDsp::InitFilterNode(pEngine, pNode, type, plMaFilterNode::MaxOrder, m_fParams[0], static_cast<plUInt32>(m_fParams[1]), 0.0f, 1.0f, nullptr).Failed())
      {
        PL_DEFAULT_DELETE(pNode);
        return PL_FAILURE;
      }
      m_Nodes.PushBack(reinterpret_cast<ma_node*>(pNode));
      break;
    }

    case plAudioEffectType::Eq3:
    {
      plMaFilterNode* pBands[3] = {PL_DEFAULT_NEW(plMaFilterNode), PL_DEFAULT_NEW(plMaFilterNode), PL_DEFAULT_NEW(plMaFilterNode)};

      bool bOk = plMiniAudioDsp::InitFilterNode(pEngine, pBands[0], plMaFilterNode::Type::LowShelf, 1, plMath::Max(m_fParams[1], 10.0f), 1, m_fParams[0], 1.0f, nullptr).Succeeded();
      bOk = bOk && plMiniAudioDsp::InitFilterNode(pEngine, pBands[1], plMaFilterNode::Type::Peak, 1, plMath::Max(m_fParams[3], 10.0f), 1, m_fParams[2], plMath::Max(m_fParams[4], 0.1f), nullptr).Succeeded();
      bOk = bOk && plMiniAudioDsp::InitFilterNode(pEngine, pBands[2], plMaFilterNode::Type::HighShelf, 1, plMath::Max(m_fParams[6], 10.0f), 1, m_fParams[5], 1.0f, nullptr).Succeeded();

      if (!bOk)
      {
        for (plMaFilterNode* pBand : pBands)
        {
          plMiniAudioDsp::UninitFilterNode(pBand);
          PL_DEFAULT_DELETE(pBand);
        }
        return PL_FAILURE;
      }

      // chain: low -> mid -> high
      ma_node_attach_output_bus(reinterpret_cast<ma_node*>(pBands[0]), 0, reinterpret_cast<ma_node*>(pBands[1]), 0);
      ma_node_attach_output_bus(reinterpret_cast<ma_node*>(pBands[1]), 0, reinterpret_cast<ma_node*>(pBands[2]), 0);

      for (plMaFilterNode* pBand : pBands)
      {
        m_Nodes.PushBack(reinterpret_cast<ma_node*>(pBand));
      }
      break;
    }

    case plAudioEffectType::Delay:
    {
      // the buffer covers the longest delay the effect may be set to while it plays
      const float fMaxDelayMs = plMath::Max(MaxLiveDelayMs, m_fParams[0]);
      const plUInt32 uiMaxDelayFrames = static_cast<plUInt32>(fMaxDelayMs * 0.001f * uiSampleRate) + 1;

      plMaDelayNode* pNode = PL_DEFAULT_NEW(plMaDelayNode);
      if (plMiniAudioDsp::InitDelayNode(pEngine, pNode, uiMaxDelayFrames).Failed())
      {
        PL_DEFAULT_DELETE(pNode);
        return PL_FAILURE;
      }

      pNode->m_uiDelayFrames.store(plMath::Max<plUInt32>(1, static_cast<plUInt32>(plMath::Max(m_fParams[0], 1.0f) * 0.001f * uiSampleRate)), std::memory_order_relaxed);
      pNode->m_fFeedback.store(m_fParams[1], std::memory_order_relaxed);
      pNode->m_fWet.store(m_fParams[2], std::memory_order_relaxed);
      pNode->m_fDry.store(m_fParams[3], std::memory_order_relaxed);
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
    case plAudioEffectType::Ducker:
    {
      plMaCompressorNode* pNode = PL_DEFAULT_NEW(plMaCompressorNode);
      plMaCompressorNode::Mode mode = plMaCompressorNode::Mode::Compressor;

      if (m_eType == plAudioEffectType::Limiter)
      {
        mode = plMaCompressorNode::Mode::Limiter;
        pNode->m_fThresholdDb = m_fParams[0];
        pNode->m_fReleaseMs = m_fParams[1];
        pNode->m_fAttackMs = 0.1f;
      }
      else if (m_eType == plAudioEffectType::Ducker)
      {
        mode = plMaCompressorNode::Mode::Ducker;
        pNode->m_fDepthDb = m_fParams[0];
        pNode->m_fThresholdDb = m_fParams[1];
        pNode->m_fAttackMs = m_fParams[2];
        pNode->m_fReleaseMs = m_fParams[3];
      }
      else
      {
        pNode->m_fThresholdDb = m_fParams[0];
        pNode->m_fRatio = m_fParams[1];
        pNode->m_fAttackMs = m_fParams[2];
        pNode->m_fReleaseMs = m_fParams[3];
        pNode->m_fMakeupDb = m_fParams[4];
      }

      // a limiter always measures its own input
      const bool bKey = bHasKeyInput && m_eType != plAudioEffectType::Limiter;
      if (plMiniAudioDsp::InitCompressorNode(pEngine, pNode, mode, bKey).Failed())
      {
        PL_DEFAULT_DELETE(pNode);
        return PL_FAILURE;
      }
      m_bHasKeyInput = bKey;
      m_Nodes.PushBack(reinterpret_cast<ma_node*>(pNode));
      break;
    }
  }

  if (m_Nodes.IsEmpty())
    return PL_FAILURE;

  SetBypass(desc.m_bBypass);
  return PL_SUCCESS;
}

void plMiniAudioEffect::SetBypass(bool bBypass)
{
  for (ma_node* pNodeBase : m_Nodes)
  {
    switch (m_eType)
    {
      case plAudioEffectType::LowPass:
      case plAudioEffectType::HighPass:
      case plAudioEffectType::Eq3:
        reinterpret_cast<plMaFilterNode*>(pNodeBase)->m_iBypass = bBypass ? 1 : 0;
        break;
      case plAudioEffectType::Delay:
        reinterpret_cast<plMaDelayNode*>(pNodeBase)->m_bBypass.store(bBypass, std::memory_order_relaxed);
        break;
      case plAudioEffectType::Reverb:
        reinterpret_cast<plMaReverbNode*>(pNodeBase)->m_bBypass.store(bBypass, std::memory_order_relaxed);
        break;
      case plAudioEffectType::Compressor:
      case plAudioEffectType::Limiter:
      case plAudioEffectType::Ducker:
        reinterpret_cast<plMaCompressorNode*>(pNodeBase)->m_bBypass.store(bBypass, std::memory_order_relaxed);
        break;
    }
  }
}

ma_node* plMiniAudioEffect::GetKeyInput() const
{
  return m_bHasKeyInput && !m_Nodes.IsEmpty() ? m_Nodes[0] : nullptr;
}

float plMiniAudioEffect::GetGainReductionDb() const
{
  switch (m_eType)
  {
    case plAudioEffectType::Compressor:
    case plAudioEffectType::Limiter:
    case plAudioEffectType::Ducker:
      if (!m_Nodes.IsEmpty())
        return reinterpret_cast<const plMaCompressorNode*>(m_Nodes[0])->m_fGainReductionDb.load(std::memory_order_relaxed);
      break;

    default:
      break;
  }

  return 0.0f;
}

void plMiniAudioEffect::Destroy()
{
  switch (m_eType)
  {
    case plAudioEffectType::LowPass:
    case plAudioEffectType::HighPass:
    case plAudioEffectType::Eq3:
      for (ma_node* pNodeBase : m_Nodes)
      {
        auto* pNode = reinterpret_cast<plMaFilterNode*>(pNodeBase);
        plMiniAudioDsp::UninitFilterNode(pNode);
        PL_DEFAULT_DELETE(pNode);
      }
      break;

    case plAudioEffectType::Delay:
      if (!m_Nodes.IsEmpty())
      {
        auto* pNode = reinterpret_cast<plMaDelayNode*>(m_Nodes[0]);
        plMiniAudioDsp::UninitDelayNode(pNode);
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
    case plAudioEffectType::Ducker:
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
  m_bHasKeyInput = false;
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
      ApplyFilterParams();
      break;

    case plAudioEffectType::Delay:
    {
      auto* pNode = reinterpret_cast<plMaDelayNode*>(m_Nodes[0]);
      const plUInt32 uiDelayFrames = static_cast<plUInt32>(plMath::Max(m_fParams[0], 1.0f) * 0.001f * pNode->m_uiSampleRate);
      pNode->m_uiDelayFrames.store(plMath::Clamp<plUInt32>(uiDelayFrames, 1u, pNode->m_uiMaxDelayFrames - 1), std::memory_order_relaxed);
      pNode->m_fFeedback.store(m_fParams[1], std::memory_order_relaxed);
      pNode->m_fWet.store(m_fParams[2], std::memory_order_relaxed);
      pNode->m_fDry.store(m_fParams[3], std::memory_order_relaxed);
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

    case plAudioEffectType::Ducker:
    {
      auto* pNode = reinterpret_cast<plMaCompressorNode*>(m_Nodes[0]);
      pNode->m_fDepthDb = m_fParams[0];
      pNode->m_fThresholdDb = m_fParams[1];
      pNode->m_fAttackMs = m_fParams[2];
      pNode->m_fReleaseMs = m_fParams[3];
      break;
    }
  }
}

void plMiniAudioEffect::ApplyFilterParams()
{
  switch (m_eType)
  {
    case plAudioEffectType::LowPass:
    case plAudioEffectType::HighPass:
      plMiniAudioDsp::SetFilter(reinterpret_cast<plMaFilterNode*>(m_Nodes[0]), m_fParams[0], static_cast<plUInt32>(m_fParams[1]), 0.0f, 1.0f);
      break;

    case plAudioEffectType::Eq3:
      if (m_Nodes.GetCount() == 3)
      {
        plMiniAudioDsp::SetFilter(reinterpret_cast<plMaFilterNode*>(m_Nodes[0]), plMath::Max(m_fParams[1], 10.0f), 1, m_fParams[0], 1.0f);
        plMiniAudioDsp::SetFilter(reinterpret_cast<plMaFilterNode*>(m_Nodes[1]), plMath::Max(m_fParams[3], 10.0f), 1, m_fParams[2], plMath::Max(m_fParams[4], 0.1f));
        plMiniAudioDsp::SetFilter(reinterpret_cast<plMaFilterNode*>(m_Nodes[2]), plMath::Max(m_fParams[6], 10.0f), 1, m_fParams[5], 1.0f);
      }
      break;

    default:
      break;
  }
}
