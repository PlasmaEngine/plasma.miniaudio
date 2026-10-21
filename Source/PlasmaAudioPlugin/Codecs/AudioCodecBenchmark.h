#pragma once

#include <Foundation/Strings/String.h>
#include <Foundation/Time/Time.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

/// \brief Measures how many voices of each codec one CPU core decodes, on this machine.
///
/// Encodes a few seconds of a stereo test signal once, then decodes it again and again for a time budget per codec:
/// Opus with its plain C kernels, Opus with the SIMD kernels the CPU has, and IMA ADPCM. The result is the real-time
/// factor divided by 100, i.e. the voices that fit into one percent of one core. Runs on the calling thread.
///
/// While it measures the plain C kernels, Opus coders created on other threads use them too.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioCodecBenchmark
{
public:
  struct Result
  {
    float m_fOpusPlainVoices = 0.0f; ///< voices per 1 % of one core; 0 where the build cannot measure it
    float m_fOpusSimdVoices = 0.0f;
    float m_fAdpcmVoices = 0.0f;
    plString m_sSimd;                ///< the kernels Opus uses here, e.g. "AVX2"; "plain C" without SIMD
  };

  /// \brief Runs the benchmark, spending about budget per measurement.
  static plResult Run(plTime budget, Result& out_result);

  /// \brief The kernels Opus picks on this CPU, e.g. "AVX2".
  static plString GetOpusSimdName();
};
