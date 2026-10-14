#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Time/Time.h>

class plStreamWriter;
class plStreamReader;

/// \brief The type of a mixer insert effect.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEffectType
{
  using StorageType = plUInt8;

  enum Enum : StorageType
  {
    LowPass = 0,   ///< params: 0 = cutoff Hz, 1 = order (1..8)
    HighPass = 1,  ///< params: 0 = cutoff Hz, 1 = order (1..8)
    Eq3 = 2,       ///< params: 0 = low gain dB, 1 = low freq Hz, 2 = mid gain dB, 3 = mid freq Hz, 4 = mid Q, 5 = high gain dB, 6 = high freq Hz
    Delay = 3,     ///< params: 0 = delay ms, 1 = feedback (0..1), 2 = wet (0..1), 3 = dry (0..1)
    Reverb = 4,    ///< params: 0 = room size (0..1), 1 = damping (0..1), 2 = width (0..1), 3 = wet (0..1), 4 = dry (0..1)
    Compressor = 5,///< params: 0 = threshold dB, 1 = ratio, 2 = attack ms, 3 = release ms, 4 = makeup dB
    Limiter = 6,   ///< params: 0 = ceiling dB, 1 = release ms

    Default = LowPass
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PLASMAAUDIOPLUGIN_DLL, plAudioEffectType);

/// \brief Number of parameter slots available on every effect. Unused slots are ignored per type.
constexpr plUInt32 plAudioEffectMaxParams = 8;

/// \brief Serialized description of one insert effect on a mixer bus.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioEffectDesc
{
  plAudioEffectType::Enum m_eType = plAudioEffectType::Default;
  float m_fParams[plAudioEffectMaxParams] = {0, 0, 0, 0, 0, 0, 0, 0};

  /// \brief Fills m_fParams with sensible defaults for the current type.
  void InitDefaults();

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// \brief An auxiliary send from one bus into an aux/return bus.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioBusSendDesc
{
  plUInt8 m_uiTargetBus = 0; ///< index into plAudioMixerDesc::m_Buses, must reference an aux bus
  float m_fLevelDb = 0.0f;

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// \brief One bus of the mixer hierarchy. Bus 0 is always the master bus.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioBusDesc
{
  plString m_sName;
  plUInt8 m_uiParent = 0xFF; ///< index of the parent bus, 0xFF for the master bus
  float m_fVolumeDb = 0.0f;
  float m_fPitch = 1.0f;
  bool m_bIsAux = false;             ///< aux/return buses receive sends and environment routing instead of event output
  plString m_sEnvironmentName;       ///< aux only: environment control name that feeds this bus (see plAudioSystemEnvironmentComponent)
  plHybridArray<plAudioEffectDesc, 2> m_Effects;
  plHybridArray<plAudioBusSendDesc, 2> m_Sends;

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// \brief A VCA: a named gain applied on top of a set of buses (typical options-menu volume sliders).
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioVcaDesc
{
  plString m_sName;
  float m_fVolumeDb = 0.0f;
  plHybridArray<plUInt8, 4> m_Buses; ///< indices into plAudioMixerDesc::m_Buses

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// \brief Which mixer property a snapshot override targets.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioSnapshotTarget
{
  using StorageType = plUInt8;

  enum Enum : StorageType
  {
    BusVolume = 0,   ///< m_fValue = absolute volume dB
    BusPitch = 1,    ///< m_fValue = absolute pitch factor
    SendLevel = 2,   ///< m_uiIndexA = send index, m_fValue = absolute level dB
    EffectParam = 3, ///< m_uiIndexA = effect index, m_uiIndexB = param index, m_fValue = absolute param value

    Default = BusVolume
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PLASMAAUDIOPLUGIN_DLL, plAudioSnapshotTarget);

/// \brief One overridden property inside a snapshot.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioSnapshotOverrideDesc
{
  plUInt8 m_uiBus = 0;
  plAudioSnapshotTarget::Enum m_eTarget = plAudioSnapshotTarget::Default;
  plUInt8 m_uiIndexA = 0;
  plUInt8 m_uiIndexB = 0;
  float m_fValue = 0.0f;

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// \brief A blendable mixer state. Higher priority snapshots are applied last (they win).
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioSnapshotDesc
{
  plString m_sName;
  plTime m_FadeIn = plTime::MakeFromSeconds(0.5);
  plTime m_FadeOut = plTime::MakeFromSeconds(0.5);
  plUInt8 m_uiPriority = 0;
  plHybridArray<plAudioSnapshotOverrideDesc, 4> m_Overrides;

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};

/// \brief The full serialized mixer setup: bus hierarchy, VCAs and snapshots.
///
/// This is the payload of a plAudioMixerResource (compiled from a .plAudioMixerAsset)
/// and what gets handed to plAudioMiddleware::LoadMixer(). All runtime blending
/// (VCAs, snapshots, mute/solo) happens in plAudioMixerService — backends only ever
/// receive final per-bus values.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioMixerDesc
{
  plDynamicArray<plAudioBusDesc> m_Buses;
  plDynamicArray<plAudioVcaDesc> m_Vcas;
  plDynamicArray<plAudioSnapshotDesc> m_Snapshots;

  plResult Save(plStreamWriter& inout_stream) const;
  plResult Load(plStreamReader& inout_stream);

  /// \brief Returns plInvalidIndex if no bus with that name exists. Case-insensitive.
  plUInt32 FindBusByName(plStringView sName) const;

  /// \brief Returns the index of the aux bus mapped to the given environment name, or plInvalidIndex.
  plUInt32 FindAuxBusForEnvironment(plStringView sEnvironmentName) const;

  /// \brief Returns plInvalidIndex if no VCA with that name exists. Case-insensitive.
  plUInt32 FindVcaByName(plStringView sName) const;

  /// \brief Returns plInvalidIndex if no snapshot with that name exists. Case-insensitive.
  plUInt32 FindSnapshotByName(plStringView sName) const;

  /// \brief Creates a minimal valid mixer: a single master bus.
  void MakeDefault();
};

namespace plAudioMixerUtils
{
  /// \brief dB to linear gain. -60 dB or below is treated as silence.
  PL_PLASMAAUDIOPLUGIN_DLL float DbToLinear(float fDb);

  /// \brief Linear gain to dB, clamped to -60 dB.
  PL_PLASMAAUDIOPLUGIN_DLL float LinearToDb(float fLinear);

  /// \brief Semitones to pitch factor.
  PL_PLASMAAUDIOPLUGIN_DLL float SemitonesToPitch(float fSemitones);
} // namespace plAudioMixerUtils
