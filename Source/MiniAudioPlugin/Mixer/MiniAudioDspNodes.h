#pragma once

#include <AudioSystem/Mixer/AudioMixerData.h>
#include <Foundation/Containers/HybridArray.h>
#include <MiniAudio/miniaudio.h>
#include <MiniAudioPlugin/MiniAudioPluginDLL.h>

#include <atomic>

/// \brief Pass-through metering node: measures peak and RMS of the signal flowing through it.
struct plMaMeterNode
{
  ma_node_base m_Base; // must be first
  plUInt32 m_uiChannels = 0;
  std::atomic<float> m_fPeak = 0.0f;
  std::atomic<float> m_fRms = 0.0f;
};

/// \brief Freeverb-style reverb node (N-channel, per-channel comb/allpass banks).
struct plMaReverbNode
{
  static constexpr plUInt32 NumCombs = 8;
  static constexpr plUInt32 NumAllpasses = 4;

  struct Comb
  {
    float* m_pBuffer = nullptr;
    plUInt32 m_uiSize = 0;
    plUInt32 m_uiIndex = 0;
    float m_fFilterStore = 0.0f;
  };

  struct Allpass
  {
    float* m_pBuffer = nullptr;
    plUInt32 m_uiSize = 0;
    plUInt32 m_uiIndex = 0;
  };

  ma_node_base m_Base; // must be first
  plUInt32 m_uiChannels = 0;

  // parameters (audio-thread written, mixer-thread written — floats, tolerated)
  float m_fRoomSize = 0.5f;
  float m_fDamping = 0.5f;
  float m_fWidth = 1.0f;
  float m_fWet = 1.0f;
  float m_fDry = 0.0f;

  // per channel: NumCombs combs + NumAllpasses allpasses
  Comb* m_pCombs = nullptr;         // [channels * NumCombs]
  Allpass* m_pAllpasses = nullptr;  // [channels * NumAllpasses]
};

/// \brief Feed-forward compressor node (envelope follower, shared gain across channels).
struct plMaCompressorNode
{
  ma_node_base m_Base; // must be first
  plUInt32 m_uiChannels = 0;
  plUInt32 m_uiSampleRate = 48000;

  float m_fThresholdDb = -18.0f;
  float m_fRatio = 3.0f;
  float m_fAttackMs = 10.0f;
  float m_fReleaseMs = 120.0f;
  float m_fMakeupDb = 0.0f;

  /// \brief When true the node acts as a hard limiter: m_fThresholdDb is the ceiling, ratio is ignored.
  bool m_bLimiter = false;

  float m_fEnvelope = 0.0f;
};

/// \brief Source node fed with PCM by a producer thread (a video decoder) through a ring buffer.
///
/// No inputs, one output at the engine's channel count. Resamples from the source rate (times the pitch) with linear
/// interpolation, maps mono / stereo / N channels onto the engine layout, and counts the source frames it consumed:
/// that counter is the producer's playback clock, advancing exactly as fast as the device consumes.
struct plMaPcmStreamNode
{
  static constexpr plUInt32 MaxChannels = 8;

  ma_node_base m_Base; // must be first
  plUInt32 m_uiEngineChannels = 0;
  plUInt32 m_uiEngineSampleRate = 48000;
  plUInt32 m_uiSourceChannels = 0;
  plUInt32 m_uiSourceSampleRate = 48000;

  ma_pcm_rb m_Ring = {};
  bool m_bRingValid = false;

  std::atomic<plUInt64> m_uiFramesRendered = 0;
  std::atomic<float> m_fVolume = 1.0f;
  std::atomic<float> m_fPitch = 1.0f;
  std::atomic<bool> m_bPaused = false;
  std::atomic<plUInt32> m_uiFlushRequested = 0; ///< generation the process callback still has to honour
  plUInt32 m_uiFlushHandled = 0;

  // resampler state, audio thread only
  float m_PrevFrame[MaxChannels] = {};
  float m_CurFrame[MaxChannels] = {};
  double m_fPhase = 1.0; ///< starts at 1 so the first output frame pulls a source frame
  bool m_bPrimed = false;
};

namespace plMiniAudioDsp
{
  plResult InitMeterNode(ma_engine* pEngine, plMaMeterNode* pNode);
  void UninitMeterNode(plMaMeterNode* pNode);

  plResult InitPcmStreamNode(ma_engine* pEngine, plMaPcmStreamNode* pNode, plUInt32 uiSourceChannels, plUInt32 uiSourceSampleRate, plUInt32 uiRingFrames);
  void UninitPcmStreamNode(plMaPcmStreamNode* pNode);
  /// \brief Producer side: copies interleaved source frames into the ring. Returns frames accepted.
  plUInt32 WritePcmStreamFrames(plMaPcmStreamNode* pNode, const float* pInterleaved, plUInt32 uiFrames);
  plUInt32 GetPcmStreamFreeFrames(const plMaPcmStreamNode* pNode);

  plResult InitReverbNode(ma_engine* pEngine, plMaReverbNode* pNode);
  void UninitReverbNode(plMaReverbNode* pNode);

  plResult InitCompressorNode(ma_engine* pEngine, plMaCompressorNode* pNode, bool bLimiter);
  void UninitCompressorNode(plMaCompressorNode* pNode);
} // namespace plMiniAudioDsp

/// \brief One instantiated insert effect: owns the miniaudio node(s) realizing a plAudioEffectDesc.
///
/// Effects are node chains of their own (Eq3 uses three filter nodes); GetHead()/GetTail()
/// expose the ends for splicing into a bus or voice chain.
class plMiniAudioEffect
{
public:
  plResult Create(ma_engine* pEngine, const plAudioEffectDesc& desc);
  void Destroy();

  /// \brief Live parameter update. Parameter indices follow plAudioEffectType documentation.
  void SetParameter(plUInt32 uiParamIndex, float fValue);

  ma_node* GetHead() const { return m_Nodes.IsEmpty() ? nullptr : m_Nodes[0]; }
  ma_node* GetTail() const { return m_Nodes.IsEmpty() ? nullptr : m_Nodes.PeekBack(); }

  plAudioEffectType::Enum GetType() const { return m_eType; }

private:
  void ReinitFilters();

  plAudioEffectType::Enum m_eType = plAudioEffectType::Default;
  float m_fParams[plAudioEffectMaxParams] = {};
  ma_engine* m_pEngine = nullptr;
  plHybridArray<ma_node*, 3> m_Nodes; // head..tail, already attached to each other
};
