#pragma once

#include <AudioSystem/Core/AudioSystemData.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

/// \brief Event parameter names the PlasmaAudio middleware handles itself instead of forwarding them to event parameters.
/// Sent with plAudioSystemRequestSetEventParameter, they set linear multipliers on every instance playing on the entity.
namespace plPlasmaAudioReservedParameter
{
  /// \brief Linear volume multiplier of the entity (default 1).
  constexpr const char* Volume = "PlasmaAudio.Volume";

  /// \brief Pitch multiplier of the entity (default 1).
  constexpr const char* Pitch = "PlasmaAudio.Pitch";
} // namespace plPlasmaAudioReservedParameter

class PL_PLASMAAUDIOPLUGIN_DLL plPlasmaAudioEntityData final : public plAudioSystemEntityData
{
  PL_ADD_DYNAMIC_REFLECTION(plPlasmaAudioEntityData, plAudioSystemEntityData);

public:
  plPlasmaAudioEntityData() = default;
  explicit plPlasmaAudioEntityData(plAudioSystemDataID uiEntityId, bool bIsGlobal = false);

  plAudioSystemDataID m_uiEntityId = 0;
  bool m_bGlobalEntity = false;
  plAudioSystemTransform m_LastTransform;

  /// \brief A global entity of the editor's audition is heard at this distance when it is not negative.
  float m_fAuditionDistance = -1.0f;

  // obstruction/occlusion fed by the ATL (targets) and smoothed on the audio thread (currents)
  float m_fTargetObstruction = 0.0f;
  float m_fTargetOcclusion = 0.0f;
  float m_fCurrentObstruction = 0.0f;
  float m_fCurrentOcclusion = 0.0f;

  // multipliers set through the reserved event parameters (plPlasmaAudioReservedParameter)
  float m_fVolumeMultiplier = 1.0f;
  float m_fPitchMultiplier = 1.0f;

  // environment amounts fed by the ATL, keyed by environment name; the max-amount entry wins
  struct EnvironmentAmount
  {
    plString m_sName;
    float m_fAmount = 0.0f;
  };

  plHybridArray<EnvironmentAmount, 2> m_Environments;

  /// \brief Active switch states, one per group ("Surface" -> "Surface/Grass").
  struct SwitchState
  {
    plString m_sGroup;
    plString m_sFullName;
  };

  plHybridArray<SwitchState, 2> m_SwitchStates;

  /// \brief Sets the state of a switch group, replacing any previous state of that group.
  void SetSwitchState(plStringView sGroup, plStringView sFullName)
  {
    for (auto& state : m_SwitchStates)
    {
      if (state.m_sGroup == sGroup)
      {
        state.m_sFullName = sFullName;
        return;
      }
    }

    auto& state = m_SwitchStates.ExpandAndGetRef();
    state.m_sGroup = sGroup;
    state.m_sFullName = sFullName;
  }

  /// \brief Whether an instrument with this switch filter may currently play.
  /// An empty filter always passes; an unset group also passes, so content keeps working
  /// until gameplay selects a state.
  bool MatchesSwitchFilter(plStringView sFilter) const
  {
    if (sFilter.IsEmpty())
      return true;

    const char* szSeparator = sFilter.FindLastSubString("/");
    const plStringView sGroup = szSeparator != nullptr ? plStringView(sFilter.GetStartPointer(), szSeparator) : plStringView();

    for (const auto& state : m_SwitchStates)
    {
      if (state.m_sGroup == sGroup)
        return state.m_sFullName == sFilter;
    }

    return true;
  }

  /// \brief Returns the environment with the highest amount, or nullptr when all are zero.
  const EnvironmentAmount* GetDominantEnvironment() const
  {
    const EnvironmentAmount* pBest = nullptr;
    for (const auto& env : m_Environments)
    {
      if (env.m_fAmount > 0.0001f && (pBest == nullptr || env.m_fAmount > pBest->m_fAmount))
        pBest = &env;
    }
    return pBest;
  }
};

class PL_PLASMAAUDIOPLUGIN_DLL plPlasmaAudioListenerData final : public plAudioSystemListenerData
{
  PL_ADD_DYNAMIC_REFLECTION(plPlasmaAudioListenerData, plAudioSystemListenerData);

public:
  plPlasmaAudioListenerData() = default;
  explicit plPlasmaAudioListenerData(plAudioSystemDataID uiListenerId);
  plAudioSystemDataID m_uiListenerId = 0;

  /// \brief miniaudio listener slot driven by this listener; 0xFF when it has none.
  plUInt8 m_uiListenerSlot = 0xFF;
};

class plPlasmaAudioEventInstance;

class PL_PLASMAAUDIOPLUGIN_DLL plPlasmaAudioEventData final : public plAudioSystemEventData
{
  PL_ADD_DYNAMIC_REFLECTION(plPlasmaAudioEventData, plAudioSystemEventData);

public:
  plPlasmaAudioEventData() = default;
  explicit plPlasmaAudioEventData(plAudioSystemDataID uiEventId);
  plAudioSystemDataID m_uiEventId = 0;

  /// \brief The event instance currently bound to this event data (audio-thread only, owned by the event manager).
  plPlasmaAudioEventInstance* m_pInstance = nullptr;

  /// \brief With m_bSeeded, the instance started for this event data draws its random picks from a generator seeded
  /// with m_uiSeed, so the same seed plays the same entries, volumes and pitches. Read when the instance starts.
  plUInt32 m_uiSeed = 0;
  bool m_bSeeded = false;

  /// \brief Where on its timeline the instance starts. Instruments that started earlier begin mid-sample. The offset is
  /// limited to the time the audio engine has been running.
  plTime m_StartOffset = plTime::MakeZero();
};

class PL_PLASMAAUDIOPLUGIN_DLL plPlasmaAudioTriggerData final : public plAudioSystemTriggerData
{
  PL_ADD_DYNAMIC_REFLECTION(plPlasmaAudioTriggerData, plAudioSystemTriggerData);

public:
  plPlasmaAudioTriggerData() = default;
  explicit plPlasmaAudioTriggerData(plAudioSystemDataID uiTriggerId, plStringView sResourceId);

  plAudioSystemDataID m_uiTriggerId = 0;
  plString m_sResourceId;
};

class PL_PLASMAAUDIOPLUGIN_DLL plPlasmaAudioBankData final : public plAudioSystemBankData
{
  PL_ADD_DYNAMIC_REFLECTION(plPlasmaAudioBankData, plAudioSystemBankData);

public:
  plPlasmaAudioBankData() = default;
  explicit plPlasmaAudioBankData(plAudioSystemDataID uiBankId, plStringView sBankName);

  plAudioSystemDataID m_uiBankId = 0;
  plString m_sBankName;
};

class PL_PLASMAAUDIOPLUGIN_DLL plPlasmaAudioRtpcData final : public plAudioSystemRtpcData
{
  PL_ADD_DYNAMIC_REFLECTION(plPlasmaAudioRtpcData, plAudioSystemRtpcData);
};

class PL_PLASMAAUDIOPLUGIN_DLL plPlasmaAudioSwitchStateData final : public plAudioSystemSwitchStateData
{
  PL_ADD_DYNAMIC_REFLECTION(plPlasmaAudioSwitchStateData, plAudioSystemSwitchStateData);
};

class PL_PLASMAAUDIOPLUGIN_DLL plPlasmaAudioEnvironmentData final : public plAudioSystemEnvironmentData
{
  PL_ADD_DYNAMIC_REFLECTION(plPlasmaAudioEnvironmentData, plAudioSystemEnvironmentData);
};
