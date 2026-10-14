#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <PlasmaAudioPlugin/Mixer/AudioMixerResource.h>

class plMiniAudioBusGraph;

/// \brief Mixer runtime: owns the loaded mixer desc, blends VCAs and snapshots
/// and pushes final per-bus values into a plMiniAudioBusGraph.
///
/// All methods are expected to be called from the audio thread (plPlasmaAudioMiddleware
/// forwards the name-based mixer SPI here and ticks Update()).
/// The bus graph never sees VCAs, snapshots, mute or solo, only final volumes, pitches,
/// send levels and effect parameters.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioMixerService
{
public:
  /// \brief Takes over an already loaded mixer desc and resets the runtime state.
  /// The caller builds the bus graph from the same desc and then calls PushValues(pGraph, true).
  void InitFromDesc(const plAudioMixerResourceHandle& hResource, const plAudioMixerDesc& desc);

  /// \brief Forgets the current mixer. The bus graph is torn down by its owner.
  void UnloadMixer(plMiniAudioBusGraph* pGraph);

  PL_NODISCARD bool IsLoaded() const { return m_bLoaded; }

  PL_NODISCARD const plAudioMixerDesc& GetDesc() const { return m_Desc; }

  /// \brief Sets the user volume of a bus (as authored volume replacement, in dB).
  void SetBusVolume(plStringView sBusName, float fVolumeDb);

  /// \brief Sets the user volume of a VCA (in dB).
  void SetVcaVolume(plStringView sVcaName, float fVolumeDb);

  /// \brief Sets the target intensity (0..1) of a snapshot. Fades with the snapshot's authored fade times.
  void SetSnapshotIntensity(plStringView sSnapshotName, float fIntensity);

  /// \brief Immediately deactivates all snapshots (no fade).
  void StopAllSnapshots();

  /// \brief Legacy plSoundInterface::SetSoundGroupVolume mapping: linear volume onto a bus or VCA by name.
  /// Returns false if no bus or VCA with that name exists.
  bool SetGroupVolume(plStringView sGroupName, float fLinearVolume);

  PL_NODISCARD float GetGroupVolume(plStringView sGroupName) const;

  /// \brief Advances snapshot fades and pushes changed values to the bus graph.
  void Update(plTime delta, plMiniAudioBusGraph* pGraph);

  /// \brief Pushes the blended per-bus values to the bus graph. Only changed values unless bForce.
  void PushValues(plMiniAudioBusGraph* pGraph, bool bForce);

  /// \brief The linear gain a voice on bus uiBus gets from the mixer: the product of the last pushed gains of the bus
  /// and all its parents up to master. 1 when no mixer is loaded or the values were not pushed yet.
  PL_NODISCARD float GetEffectiveBusGain(plUInt32 uiBus) const;

  /// \brief The linear gain last pushed to bus uiBus alone: its volume with snapshots and VCAs applied, without its
  /// parents. 1 when no mixer is loaded or the values were not pushed yet.
  PL_NODISCARD float GetBusGain(plUInt32 uiBus) const;

  /// \brief The current (faded) intensity of snapshot uiSnapshot of the desc; 0 when there is no such snapshot.
  PL_NODISCARD float GetSnapshotIntensity(plUInt32 uiSnapshot) const;

private:
  struct BusState
  {
    float m_fUserVolumeDb = 0.0f;
    float m_fLastSentGain = -1.0f;
    float m_fLastSentPitch = -1.0f;
    plHybridArray<float, 2> m_LastSentSends;
    plHybridArray<float, 8> m_LastSentEffectParams; // effectIndex * plAudioEffectMaxParams + paramIndex
  };

  struct VcaState
  {
    float m_fUserVolumeDb = 0.0f;
  };

  struct SnapshotState
  {
    float m_fCurrent = 0.0f;
    float m_fTarget = 0.0f;
  };

  void ResetRuntimeState();

  plAudioMixerResourceHandle m_hResource;
  plAudioMixerDesc m_Desc;

  plDynamicArray<BusState> m_BusStates;
  plDynamicArray<VcaState> m_VcaStates;
  plDynamicArray<SnapshotState> m_SnapshotStates;

  bool m_bLoaded = false;
  bool m_bDirty = false;
};
