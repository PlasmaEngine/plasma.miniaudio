#pragma once

#include <AudioSystem/Mixer/AudioMixerData.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <MiniAudio/miniaudio.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

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

  // parameters: written by the audio thread, read once per processed chunk by the device thread
  std::atomic<float> m_fRoomSize = 0.5f;
  std::atomic<float> m_fDamping = 0.5f;
  std::atomic<float> m_fWidth = 1.0f; ///< stereo spread of the wet signal between channel pairs (0 = mono, 1 = full)
  std::atomic<float> m_fWet = 1.0f;
  std::atomic<float> m_fDry = 0.0f;
  std::atomic<bool> m_bBypass = false; ///< passes the input through unchanged

  // per channel: NumCombs combs + NumAllpasses allpasses
  Comb* m_pCombs = nullptr;         // [channels * NumCombs]
  Allpass* m_pAllpasses = nullptr;  // [channels * NumAllpasses]
};

/// \brief Feed-forward dynamics node (envelope follower, shared gain across channels): a compressor, a limiter or a
/// ducker.
///
/// A node created with a key input has a second input bus; the level is then measured on that input (a sidechain)
/// while the gain is applied to the first one. A ducker lowers its input by the depth while the key is above the
/// threshold.
struct plMaCompressorNode
{
  enum class Mode : plUInt8
  {
    Compressor,
    Limiter, ///< m_fThresholdDb is the ceiling, the ratio is ignored
    Ducker,  ///< lowers the signal by m_fDepthDb while the key is above m_fThresholdDb
  };

  ma_node_base m_Base; // must be first
  plUInt32 m_uiChannels = 0;
  plUInt32 m_uiSampleRate = 48000;
  plUInt32 m_InputChannels[2] = {};
  Mode m_eMode = Mode::Compressor;
  bool m_bHasKeyInput = false; ///< input bus 1 is the key

  // parameters: written by the audio thread, read once per processed chunk by the device thread
  std::atomic<float> m_fThresholdDb = -18.0f;
  std::atomic<float> m_fRatio = 3.0f;
  std::atomic<float> m_fAttackMs = 10.0f;
  std::atomic<float> m_fReleaseMs = 120.0f;
  std::atomic<float> m_fMakeupDb = 0.0f;
  std::atomic<float> m_fDepthDb = -7.0f;
  std::atomic<bool> m_bBypass = false;

  /// \brief The deepest gain reduction of the last processed chunk, in dB (0 or negative). Read by meters.
  std::atomic<float> m_fGainReductionDb = 0.0f;

  float m_fEnvelope = 0.0f;   ///< device thread only
  float m_fDuckGainDb = 0.0f; ///< device thread only
};

/// \brief A biquad-based filter (low pass and high pass of order 1..8, low shelf, peak, high shelf) whose settings can
/// change while it plays.
///
/// Set...() stores the new settings and bumps a generation counter; the device thread applies them at the start of the
/// next chunk it processes, so the filter state is never touched from another thread. A changed frequency, gain or Q
/// recomputes the coefficients and keeps the filter state; a changed low/high pass order re-initializes the filter in
/// memory reserved for the maximum order at init, without allocating. The setters must be called from one thread.
struct plMaFilterNode
{
  enum class Type : plUInt8
  {
    LowPass,
    HighPass,
    LowShelf,
    Peak,
    HighShelf,
  };

  static constexpr plUInt32 MaxOrder = 8;

  /// filter heap that fits a 2nd order low/high pass of up to 8 channels; larger ones are allocated at init
  static constexpr plUInt32 InlineHeapBytes = 1024;

  ma_node_base m_Base; // must be first
  Type m_eType = Type::LowPass;
  plUInt32 m_uiChannels = 0;
  plUInt32 m_uiSampleRate = 48000;
  plUInt32 m_uiMaxOrder = 2;

  // settings: stored by the writer, applied by the device thread when m_uiGeneration changed
  plAtomicInteger32 m_iFrequencyBits; ///< float bits, Hz
  plAtomicInteger32 m_iGainDbBits;    ///< float bits, shelves and peak only
  plAtomicInteger32 m_iQBits;         ///< float bits, peak only
  plAtomicInteger32 m_iOrder;         ///< low and high pass only, 1..m_uiMaxOrder
  plAtomicInteger32 m_iGeneration;
  plAtomicInteger32 m_iBypass; ///< non-zero passes the input through unchanged

  // device thread
  plInt32 m_iAppliedGeneration = 0;
  plUInt32 m_uiAppliedOrder = 0;

  // writer thread: the last stored values, so unchanged settings are not re-applied
  float m_fLastFrequency = -1.0f;
  float m_fLastGainDb = 0.0f;
  float m_fLastQ = 0.0f;
  plUInt32 m_uiLastOrder = 0;

  void* m_pHeap = nullptr;          ///< filter heap: m_InlineHeap or allocated with m_AllocationCallbacks
  ma_allocation_callbacks m_AllocationCallbacks = {};
  alignas(16) plUInt8 m_InlineHeap[InlineHeapBytes];

  union
  {
    ma_lpf m_Lpf;
    ma_hpf m_Hpf;
    ma_loshelf2 m_LoShelf;
    ma_peak2 m_Peak;
    ma_hishelf2 m_HiShelf;
  };

  bool m_bValid = false;
};

/// \brief A feedback delay whose delay time can change while it plays (up to the maximum given at init).
struct plMaDelayNode
{
  ma_node_base m_Base; // must be first
  plUInt32 m_uiChannels = 0;
  plUInt32 m_uiSampleRate = 48000;
  plUInt32 m_uiMaxDelayFrames = 0;

  // parameters: written by the audio thread, read once per processed chunk by the device thread
  std::atomic<plUInt32> m_uiDelayFrames = 1;
  std::atomic<float> m_fFeedback = 0.3f;
  std::atomic<float> m_fWet = 0.35f;
  std::atomic<float> m_fDry = 1.0f;
  std::atomic<bool> m_bBypass = false; ///< passes the input through unchanged

  // device thread
  float* m_pBuffer = nullptr; ///< m_uiMaxDelayFrames frames, interleaved
  plUInt32 m_uiWriteFrame = 0;
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

/// \brief Silent source node that stays attached to the graph endpoint for the lifetime of an engine.
///
/// miniaudio advances the engine clock only while the endpoint has at least one attachment. Voices, buses and PCM
/// streams come and go, so without this node the clock stops whenever nothing else is attached (no mixer loaded and
/// nothing playing), and voices scheduled for a later frame never start.
struct plMaClockNode
{
  ma_node_base m_Base; // must be first
  plUInt32 m_uiChannels = 0;
  bool m_bValid = false;
};

/// \brief Source node playing a test signal: noise in bursts, so meters, faders and dynamics can be judged without
/// game audio.
///
/// Each burst lasts m_fBurstSeconds out of every m_fPeriodSeconds, with short ramps; the bursts of different nodes
/// fall apart when their periods differ.
struct plMaTestSignalNode
{
  ma_node_base m_Base; // must be first
  plUInt32 m_uiChannels = 0;
  plUInt32 m_uiSampleRate = 48000;
  float m_fLevel = 0.125f; ///< linear peak of the noise
  float m_fPeriodSeconds = 4.0f;
  float m_fBurstSeconds = 2.0f;
  bool m_bValid = false;

  // device thread
  plUInt32 m_uiNoiseState = 0x12345u;
  plUInt64 m_uiFrame = 0;
  float m_fPink[3] = {};
};

namespace plMiniAudioDsp
{
  /// \brief Creates a test signal node; attach its output bus 0 where it should play.
  PL_PLASMAAUDIOPLUGIN_DLL plResult InitTestSignalNode(ma_engine* pEngine, plMaTestSignalNode* pNode);
  PL_PLASMAAUDIOPLUGIN_DLL void UninitTestSignalNode(plMaTestSignalNode* pNode);

  /// \brief Creates the clock node and attaches it to the engine's endpoint.
  PL_PLASMAAUDIOPLUGIN_DLL plResult InitClockNode(ma_engine* pEngine, plMaClockNode* pNode);
  PL_PLASMAAUDIOPLUGIN_DLL void UninitClockNode(plMaClockNode* pNode);

  /// \brief Mixes and discards delta worth of frames on an engine without a device, so its clock, fades and voice
  /// ends advance as they would with a device.
  ///
  /// The fractional frame left over is carried in ref_fFrameRemainder. At most one second is rendered per call; a
  /// longer delta (a stall) is dropped instead of being caught up. Returns the number of frames rendered.
  PL_PLASMAAUDIOPLUGIN_DLL ma_uint64 RenderWithoutDevice(ma_engine* pEngine, plTime delta, double& ref_fFrameRemainder, plDynamicArray<float>& ref_scratch);

  plResult InitMeterNode(ma_engine* pEngine, plMaMeterNode* pNode);
  void UninitMeterNode(plMaMeterNode* pNode);

  plResult InitPcmStreamNode(ma_engine* pEngine, plMaPcmStreamNode* pNode, plUInt32 uiSourceChannels, plUInt32 uiSourceSampleRate, plUInt32 uiRingFrames);
  void UninitPcmStreamNode(plMaPcmStreamNode* pNode);
  /// \brief Producer side: copies interleaved source frames into the ring. Returns frames accepted.
  plUInt32 WritePcmStreamFrames(plMaPcmStreamNode* pNode, const float* pInterleaved, plUInt32 uiFrames);
  plUInt32 GetPcmStreamFreeFrames(const plMaPcmStreamNode* pNode);

  plResult InitReverbNode(ma_engine* pEngine, plMaReverbNode* pNode);
  void UninitReverbNode(plMaReverbNode* pNode);

  /// \brief bHasKeyInput gives the node a second input bus whose level drives the gain.
  PL_PLASMAAUDIOPLUGIN_DLL plResult InitCompressorNode(ma_engine* pEngine, plMaCompressorNode* pNode, plMaCompressorNode::Mode mode, bool bHasKeyInput);
  PL_PLASMAAUDIOPLUGIN_DLL void UninitCompressorNode(plMaCompressorNode* pNode);

  /// \brief Initializes the filter with its first settings. uiMaxOrder bounds the low / high pass order the filter can
  /// later be set to. pCallbacks (may be null for miniaudio's default heap) allocates the node and a filter heap that
  /// does not fit inline; the node keeps them for UninitFilterNode.
  PL_PLASMAAUDIOPLUGIN_DLL plResult InitFilterNode(ma_engine* pEngine, plMaFilterNode* pNode, plMaFilterNode::Type type, plUInt32 uiMaxOrder, float fFrequency, plUInt32 uiOrder, float fGainDb, float fQ, const ma_allocation_callbacks* pCallbacks);
  PL_PLASMAAUDIOPLUGIN_DLL void UninitFilterNode(plMaFilterNode* pNode);

  /// \brief Changes the settings of a playing filter (see plMaFilterNode). Settings equal to the last ones are ignored;
  /// uiOrder is clamped to the node's maximum order. One writer thread only.
  PL_PLASMAAUDIOPLUGIN_DLL void SetFilter(plMaFilterNode* pNode, float fFrequency, plUInt32 uiOrder, float fGainDb, float fQ);

  /// \brief Initializes the delay with room for uiMaxDelayFrames frames of delay.
  plResult InitDelayNode(ma_engine* pEngine, plMaDelayNode* pNode, plUInt32 uiMaxDelayFrames);
  void UninitDelayNode(plMaDelayNode* pNode);
} // namespace plMiniAudioDsp

/// \brief One instantiated insert effect: owns the miniaudio node(s) realizing a plAudioEffectDesc.
///
/// Effects are node chains of their own (Eq3 uses three filter nodes); GetHead()/GetTail()
/// expose the ends for splicing into a bus or voice chain. An effect whose creation failed has no nodes: it keeps its
/// slot on the bus (so effect indices stay as authored) and acts as a bypass.
///
/// Parameter changes reach the device thread through the nodes' atomics; nothing the device thread reads is modified
/// in place.
class plMiniAudioEffect
{
public:
  /// \brief Longest delay a Delay effect can be set to while it plays (or its authored delay, when longer).
  static constexpr float MaxLiveDelayMs = 2000.0f;

  /// \brief bHasKeyInput gives a compressor or ducker the key input GetKeyInput returns.
  plResult Create(ma_engine* pEngine, const plAudioEffectDesc& desc, bool bHasKeyInput = false);
  void Destroy();

  /// \brief Live parameter update. Parameter indices follow plAudioEffectType documentation.
  void SetParameter(plUInt32 uiParamIndex, float fValue);

  /// \brief Bypasses the effect (its input passes through) or puts it back in.
  void SetBypass(bool bBypass);

  ma_node* GetHead() const { return m_Nodes.IsEmpty() ? nullptr : m_Nodes[0]; }
  ma_node* GetTail() const { return m_Nodes.IsEmpty() ? nullptr : m_Nodes.PeekBack(); }

  /// \brief The node whose input bus 1 takes the key signal; null without a key input.
  ma_node* GetKeyInput() const;

  /// \brief The gain reduction of a compressor, limiter or ducker in dB (0 or negative); 0 for other effects.
  float GetGainReductionDb() const;

  plAudioEffectType::Enum GetType() const { return m_eType; }

private:
  /// \brief Pushes the current m_fParams to the filter nodes of a LowPass, HighPass or Eq3 effect.
  void ApplyFilterParams();

  plAudioEffectType::Enum m_eType = plAudioEffectType::Default;
  float m_fParams[plAudioEffectMaxParams] = {};
  ma_engine* m_pEngine = nullptr;
  bool m_bHasKeyInput = false;
  plHybridArray<ma_node*, 3> m_Nodes; // head..tail, already attached to each other
};
