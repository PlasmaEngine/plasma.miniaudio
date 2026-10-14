#pragma once

#include <Core/ResourceManager/ResourceManager.h>
#include <PlasmaAudioPlugin/Events/AudioEventResource.h>
#include <PlasmaAudioPlugin/Events/MiniAudioAudioEvents.h>
#include <PlasmaAudioPlugin/Mixer/MiniAudioBusGraph.h>

/// \brief A miniaudio engine without a device, rendered by hand in fixed blocks.
///
/// Every test that drives the event manager renders through RenderBlocks(): one block of BlockFrames frames
/// is mixed, then the manager is updated with exactly the block duration. The scheduling look-ahead is set to
/// two blocks, so every trigger is committed with its exact frame before the engine reaches it and the output
/// is identical from run to run.
///
/// The engine owns a default mixer (a single master bus), as the middleware has once a mixer is loaded.
/// miniaudio only advances the engine clock while something is attached to the graph endpoint, and the
/// master bus keeps it attached, so the clock runs while no voice exists.
class plOfflineAudioEngine
{
public:
  static constexpr ma_uint32 SampleRate = 48000;
  static constexpr ma_uint32 Channels = 2;
  static constexpr ma_uint32 BlockFrames = 480;
  static constexpr ma_uint64 LookAheadFrames = 2 * BlockFrames;

  plOfflineAudioEngine();
  ~plOfflineAudioEngine();

  PL_NODISCARD bool IsValid() const { return m_bValid; }
  PL_NODISCARD ma_engine* GetEngine() { return &m_Engine; }
  PL_NODISCARD plMiniAudioBusGraph* GetBusGraph() { return &m_BusGraph; }
  PL_NODISCARD ma_uint64 GetTime() { return ma_engine_get_time_in_pcm_frames(&m_Engine); }

  /// \brief Mixes uiFrames frames and appends channel 0 of the result to pChannel0 (when not null).
  void Render(ma_uint32 uiFrames, plDynamicArray<float>* pChannel0);

  /// \brief Renders uiBlocks blocks of BlockFrames frames, updating pManager after each block.
  void RenderBlocks(plUInt32 uiBlocks, plPlasmaAudioEventManager* pManager, plDynamicArray<float>* pChannel0);

  static plTime GetBlockDuration() { return plTime::MakeFromSeconds(static_cast<double>(BlockFrames) / SampleRate); }

private:
  ma_engine m_Engine = {};
  plMiniAudioBusGraph m_BusGraph;
  bool m_bEngineInitialized = false;
  bool m_bValid = false;
  plDynamicArray<float> m_Scratch;
};

namespace plAudioTestUtils
{
  /// \brief Encodes mono float samples as an in-memory RIFF/WAVE file (IEEE float, 32 bit).
  plDataBuffer MakeWavF32(const plDynamicArray<float>& samples, plUInt32 uiSampleRate);

  /// \brief uiFrames frames of the constant value fValue.
  plDynamicArray<float> MakeDc(plUInt32 uiFrames, float fValue);

  /// \brief A cosine of uiPeriods whole periods over uiFrames frames, so the sample loops without a seam.
  plDynamicArray<float> MakeCosineCycle(plUInt32 uiFrames, plUInt32 uiPeriods, float fAmplitude);

  /// \brief An event with one instrument playing sample 0, spatialization and fades off.
  plAudioEventResourceDescriptor MakeSingleInstrumentEvent(const plDynamicArray<float>& samples);

  /// \brief Creates the event resource under the given id.
  plAudioEventResourceHandle CreateEventResource(plStringView sId, plAudioEventResourceDescriptor&& descriptor);

  /// \brief Index of the first sample whose magnitude exceeds fThreshold at or after uiStart, or plInvalidIndex.
  plUInt32 FindFirstAbove(const plDynamicArray<float>& samples, float fThreshold, plUInt32 uiStart = 0);

  /// \brief Indices where the magnitude rises above fThreshold after at least uiMinSilentFrames samples below it.
  ///
  /// The minimum silence keeps the ringing of a voice's filters after its end from counting as a new onset.
  plDynamicArray<plUInt32> FindOnsets(const plDynamicArray<float>& samples, float fThreshold, plUInt32 uiMinSilentFrames);

  /// \brief Largest |x[i] - x[i-1]| for i in [uiBegin, uiEnd).
  float MaxStep(const plDynamicArray<float>& samples, plUInt32 uiBegin, plUInt32 uiEnd);
} // namespace plAudioTestUtils
