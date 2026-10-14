#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Codecs/LoudnessMeter.h>

#include <cmath>

namespace
{
  // BS.1770-4 K-weighting at 48 kHz: stage 1 high shelf (head effects), stage 2 high pass (RLB)
  constexpr double s_Stage1B[3] = {1.53512485958697, -2.69169618940638, 1.19839281085285};
  constexpr double s_Stage1A[2] = {-1.69065929318241, 0.73248077421585};
  constexpr double s_Stage2B[3] = {1.0, -2.0, 1.0};
  constexpr double s_Stage2A[2] = {-1.99004745483398, 0.99007225036621};

  constexpr plUInt32 s_uiSubBlockFrames = plLoudnessMeter::SampleRate / 10; // 100 ms
  constexpr plUInt32 s_uiSubBlocksPerBlock = 4;                             // 400 ms gating block, 75 % overlap
  constexpr double s_fAbsoluteGateLufs = -70.0;
  constexpr double s_fRelativeGateLu = -10.0;

  PL_ALWAYS_INLINE double Filter(double x, const double* b, const double* a, double* z)
  {
    const double y = b[0] * x + z[0];
    z[0] = b[1] * x - a[0] * y + z[1];
    z[1] = b[2] * x - a[1] * y;
    return y;
  }

  PL_ALWAYS_INLINE double PowerToLufs(double fPower)
  {
    return fPower > 0.0 ? -0.691 + 10.0 * std::log10(fPower) : -plMath::Infinity<double>();
  }
} // namespace

plLoudnessMeter::plLoudnessMeter(plUInt8 uiChannels)
  : m_uiChannels(uiChannels)
{
  m_Channels.SetCount(uiChannels);

  if (uiChannels == 6)
  {
    m_Channels[3].m_fWeight = 0.0;
    m_Channels[4].m_fWeight = 1.41;
    m_Channels[5].m_fWeight = 1.41;
  }
}

void plLoudnessMeter::Process(const float* pInterleaved, plUInt64 uiFrames)
{
  for (plUInt64 f = 0; f < uiFrames; ++f)
  {
    const float* pFrame = pInterleaved + f * m_uiChannels;

    for (plUInt32 c = 0; c < m_uiChannels; ++c)
    {
      ChannelState& ch = m_Channels[c];
      const double x = Filter(pFrame[c], s_Stage1B, s_Stage1A, ch.m_z[0]);
      const double y = Filter(x, s_Stage2B, s_Stage2A, ch.m_z[1]);
      m_fSubBlockSum += ch.m_fWeight * y * y;
    }

    if (++m_uiSubBlockFrames == s_uiSubBlockFrames)
    {
      m_SubBlockPower.PushBack(m_fSubBlockSum / s_uiSubBlockFrames);
      m_fSubBlockSum = 0.0;
      m_uiSubBlockFrames = 0;
    }
  }
}

bool plLoudnessMeter::HasIntegratedLoudness() const
{
  return GetIntegratedLufs() > -plMath::Infinity<double>();
}

double plLoudnessMeter::GetIntegratedLufs() const
{
  if (m_SubBlockPower.GetCount() < s_uiSubBlocksPerBlock)
    return -plMath::Infinity<double>();

  const plUInt32 uiBlocks = m_SubBlockPower.GetCount() - s_uiSubBlocksPerBlock + 1;

  plDynamicArray<double> blockPower;
  blockPower.SetCountUninitialized(uiBlocks);

  for (plUInt32 b = 0; b < uiBlocks; ++b)
  {
    double fSum = 0.0;
    for (plUInt32 s = 0; s < s_uiSubBlocksPerBlock; ++s)
      fSum += m_SubBlockPower[b + s];

    blockPower[b] = fSum / s_uiSubBlocksPerBlock;
  }

  auto GatedMean = [&](double fGateLufs, double& out_fMean) -> bool
  {
    double fSum = 0.0;
    plUInt32 uiCount = 0;
    for (double fPower : blockPower)
    {
      if (PowerToLufs(fPower) > fGateLufs)
      {
        fSum += fPower;
        ++uiCount;
      }
    }

    if (uiCount == 0)
      return false;

    out_fMean = fSum / uiCount;
    return true;
  };

  double fAbsMean = 0.0;
  if (!GatedMean(s_fAbsoluteGateLufs, fAbsMean))
    return -plMath::Infinity<double>();

  const double fRelativeGate = plMath::Max(PowerToLufs(fAbsMean) + s_fRelativeGateLu, s_fAbsoluteGateLufs);

  double fMean = 0.0;
  if (!GatedMean(fRelativeGate, fMean))
    return -plMath::Infinity<double>();

  return PowerToLufs(fMean);
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Codecs_LoudnessMeter);
