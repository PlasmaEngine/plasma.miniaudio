#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HybridArray.h>

/// \brief Integrated loudness (EBU R128 / ITU-R BS.1770-4) of a 48 kHz signal.
///
/// K-weighting uses the fixed 48 kHz biquad coefficients of BS.1770, so the input must be 48 kHz (the clip
/// builder resamples a copy before measuring). The signal is cut into 100 ms sub-blocks; gating blocks are
/// 400 ms long with 75 % overlap, gated absolutely at -70 LUFS and relatively at 10 LU below the ungated
/// loudness of the remaining blocks.
///
/// Channel weights: 1.0 for every channel, except the 6 channel (5.1) layout, where channel 3 (LFE) is left out
/// and channels 4 and 5 (surrounds) get 1.41.
///
/// A signal shorter than one gating block (400 ms), or one that is silent after gating, has no integrated
/// loudness: HasIntegratedLoudness() returns false and GetIntegratedLufs() returns -infinity.
class PL_PLASMAAUDIOPLUGIN_DLL plLoudnessMeter
{
public:
  static constexpr plUInt32 SampleRate = 48000;

  explicit plLoudnessMeter(plUInt8 uiChannels);

  /// \brief Feeds uiFrames interleaved frames. Can be called repeatedly.
  void Process(const float* pInterleaved, plUInt64 uiFrames);

  bool HasIntegratedLoudness() const;
  double GetIntegratedLufs() const;

private:
  struct Biquad
  {
    double m_b0, m_b1, m_b2, m_a1, m_a2;
  };

  struct ChannelState
  {
    double m_z[2][2] = {}; ///< direct form II transposed state of the two stages
    double m_fWeight = 1.0;
  };

  plUInt8 m_uiChannels = 0;
  plHybridArray<ChannelState, 8> m_Channels;

  double m_fSubBlockSum = 0.0; ///< weighted sum of squares of the current sub-block
  plUInt32 m_uiSubBlockFrames = 0;
  plDynamicArray<double> m_SubBlockPower; ///< weighted mean square of every completed 100 ms sub-block
};
