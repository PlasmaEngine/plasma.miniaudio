#pragma once

#include <PlasmaAudioPlugin/Mixer/AudioMixerData.h>
#include <Foundation/Containers/Deque.h>
#include <MiniAudio/miniaudio.h>
#include <PlasmaAudioPlugin/Mixer/MiniAudioDspNodes.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

/// \brief Realizes a plAudioMixerDesc as a miniaudio node graph.
///
/// Per bus: ma_sound_group (input / fader) -> insert effect chain -> meter node ->
/// [splitter when the bus has aux sends] -> parent bus group (master -> engine endpoint).
/// Aux sends are extra splitter output buses feeding the target aux bus's group.
/// Voices attach to GetBusInput() of their routed bus.
///
/// Must only be used from the audio thread.
class PL_PLASMAAUDIOPLUGIN_DLL plMiniAudioBusGraph
{
  PL_DISALLOW_COPY_AND_ASSIGN(plMiniAudioBusGraph);

public:
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

  void GetBusMeter(plUInt32 uiBusIndex, float& out_fPeak, float& out_fRms) const;

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

  plAudioMixerDesc m_Desc;
  plDeque<Bus> m_Buses; // deque: stable addresses for miniaudio node structs
  ma_engine* m_pEngine = nullptr;
};
