#pragma once

#include <AudioSystem/Mixer/AudioMixerData.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Containers/HashSet.h>
#include <MiniAudio/miniaudio.h>
#include <PlasmaAudioPlugin/Mixer/MiniAudioDspNodes.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

/// \brief Realizes a plAudioMixerDesc as a miniaudio node graph.
///
/// Per bus: ma_sound_group (input / fader) -> insert effect chain -> meter node ->
/// [splitter when the bus has aux sends or keys a sidechain] -> parent bus group (master -> engine endpoint).
/// Aux sends are extra splitter output buses feeding the target aux bus's group; sidechain taps are further outputs
/// feeding the key input of the compressors and duckers the bus keys.
/// Voices attach to GetBusInput() of their routed bus.
///
/// The graph remembers the values its buses currently have (the authored ones after Build, then whatever the
/// Set functions changed), so voices can ask for the gain of their bus and the graph can be rebuilt as it was.
///
/// Must only be used from the audio thread.
class PL_PLASMAAUDIOPLUGIN_DLL plMiniAudioBusGraph
{
  PL_DISALLOW_COPY_AND_ASSIGN(plMiniAudioBusGraph);

public:
  /// \brief The values one bus currently has.
  struct BusValues
  {
    float m_fGain = 1.0f;                   ///< linear
    float m_fPitch = 1.0f;
    plHybridArray<float, 2> m_SendLevels;   ///< linear, one per send of the desc
    plHybridArray<float, 8> m_EffectParams; ///< effect index * plAudioEffectMaxParams + param index, per effect of the desc
    plHybridArray<bool, 2> m_EffectBypass;  ///< one per effect of the desc
  };

  /// \brief The values Build gives the buses of a desc: its authored volumes, send levels, parameters and bypasses.
  static void GetAuthoredValues(const plAudioMixerDesc& desc, plDynamicArray<BusValues>& out_values);

  plMiniAudioBusGraph() = default;
  ~plMiniAudioBusGraph();

  plResult Build(const plAudioMixerDesc& desc, ma_engine* pEngine);
  void Teardown();

  PL_NODISCARD bool IsBuilt() const { return m_pEngine != nullptr; }

  PL_NODISCARD const plAudioMixerDesc& GetDesc() const { return m_Desc; }

  /// \brief The node voices should attach their output to for the given bus. Never null when built.
  ma_sound_group* GetBusInput(plUInt32 uiBusIndex);

  /// \brief Resolves an authored bus name to an index (master when empty/unknown).
  plUInt32 ResolveBus(plStringView sBusName) const;

  /// \brief Aux bus group mapped to the given environment name, or nullptr.
  ma_sound_group* GetEnvironmentAuxBus(plStringView sEnvironmentName);

  void SetBusVolume(plUInt32 uiBusIndex, float fLinearGain);
  void SetBusPitch(plUInt32 uiBusIndex, float fPitch);
  void SetBusSendLevel(plUInt32 uiBusIndex, plUInt32 uiSendIndex, float fLinearLevel);
  void SetBusEffectParameter(plUInt32 uiBusIndex, plUInt32 uiEffectIndex, plUInt32 uiParamIndex, float fValue);
  void SetBusEffectBypass(plUInt32 uiBusIndex, plUInt32 uiEffectIndex, bool bBypass);

  void GetBusMeter(plUInt32 uiBusIndex, float& out_fPeak, float& out_fRms) const;

  /// \brief The gain reduction of a compressor, limiter or ducker in dB (0 or negative); 0 for anything else.
  PL_NODISCARD float GetEffectGainReductionDb(plUInt32 uiBusIndex, plUInt32 uiEffectIndex) const;

  /// \brief Plays a test signal (noise in bursts, each bus its own rhythm) into every bus that has no child bus and is
  /// not an aux bus. Kept across rebuilds until switched off.
  void SetTestSignal(bool bEnable);
  PL_NODISCARD bool IsTestSignalOn() const { return m_bTestSignal; }

  /// \brief The values every bus currently has, indexed like the buses of the desc. Empty when not built.
  PL_NODISCARD const plDynamicArray<BusValues>& GetBusValues() const { return m_BusValues; }

  /// \brief Sets the given values on the buses, e.g. ones taken with GetBusValues before the graph was rebuilt from
  /// the same desc.
  void ApplyBusValues(const plDynamicArray<BusValues>& values);

  /// \brief The linear gain of the bus alone, without its parents. 1 for an unknown bus.
  PL_NODISCARD float GetBusGain(plUInt32 uiBusIndex) const;

  /// \brief The linear gain a voice on the bus gets from the mixer: the product of the gains of the bus and all its
  /// parents up to master. 1 when the graph is not built.
  PL_NODISCARD float GetEffectiveBusGain(plUInt32 uiBusIndex) const;

private:
  struct Bus
  {
    PL_DISALLOW_COPY_AND_ASSIGN(Bus);

    Bus() = default;

    ma_sound_group m_Group = {};
    plMaMeterNode m_Meter = {};
    ma_splitter_node* m_pSplitter = nullptr; // only when the bus has sends
    plHybridArray<plMiniAudioEffect, 2> m_Effects;
    bool m_bGroupValid = false;
    bool m_bMeterValid = false;
  };

  void CreateTestSignals();
  void DestroyTestSignals();

  plAudioMixerDesc m_Desc;
  mutable plHashSet<plString> m_WarnedUnknownBuses; ///< output bus names ResolveBus already warned about
  plDeque<Bus> m_Buses; // deque: stable addresses for miniaudio node structs
  plDynamicArray<BusValues> m_BusValues;
  plDeque<plMaTestSignalNode> m_TestSignals;
  bool m_bTestSignal = false;
  ma_engine* m_pEngine = nullptr;
};
