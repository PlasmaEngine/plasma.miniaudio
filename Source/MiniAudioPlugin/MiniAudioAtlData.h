#pragma once

#include <AudioSystem/Core/AudioSystemData.h>
#include <MiniAudioPlugin/MiniAudioPluginDLL.h>

class PL_MINIAUDIOPLUGIN_DLL plMiniAudioEntityData final : public plAudioSystemEntityData
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioEntityData, plAudioSystemEntityData);

public:
  plMiniAudioEntityData() = default;
  explicit plMiniAudioEntityData(plAudioSystemDataID uiEntityId, bool bIsGlobal = false);

  plAudioSystemDataID m_uiEntityId = 0;
  bool m_bGlobalEntity = false;
  plAudioSystemTransform m_LastTransform;

  // obstruction/occlusion fed by the ATL (targets) and smoothed on the audio thread (currents)
  float m_fTargetObstruction = 0.0f;
  float m_fTargetOcclusion = 0.0f;
  float m_fCurrentObstruction = 0.0f;
  float m_fCurrentOcclusion = 0.0f;

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

class PL_MINIAUDIOPLUGIN_DLL plMiniAudioListenerData final : public plAudioSystemListenerData
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioListenerData, plAudioSystemListenerData);

public:
  plMiniAudioListenerData() = default;
  explicit plMiniAudioListenerData(plAudioSystemDataID uiListenerId);
  plAudioSystemDataID m_uiListenerId = 0;
};

class plMiniAudioEventInstance;

class PL_MINIAUDIOPLUGIN_DLL plMiniAudioEventData final : public plAudioSystemEventData
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioEventData, plAudioSystemEventData);

public:
  plMiniAudioEventData() = default;
  explicit plMiniAudioEventData(plAudioSystemDataID uiEventId);
  plAudioSystemDataID m_uiEventId = 0;

  /// \brief The event instance currently bound to this event data (audio-thread only, owned by the event manager).
  plMiniAudioEventInstance* m_pInstance = nullptr;
};

class PL_MINIAUDIOPLUGIN_DLL plMiniAudioTriggerData final : public plAudioSystemTriggerData
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioTriggerData, plAudioSystemTriggerData);

public:
  plMiniAudioTriggerData() = default;
  explicit plMiniAudioTriggerData(plAudioSystemDataID uiTriggerId, plStringView sResourceId);

  plAudioSystemDataID m_uiTriggerId = 0;
  plString m_sResourceId;
};

class PL_MINIAUDIOPLUGIN_DLL plMiniAudioBankData final : public plAudioSystemBankData
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioBankData, plAudioSystemBankData);

public:
  plMiniAudioBankData() = default;
  explicit plMiniAudioBankData(plAudioSystemDataID uiBankId, plStringView sBankName);

  plAudioSystemDataID m_uiBankId = 0;
  plString m_sBankName;
};

class PL_MINIAUDIOPLUGIN_DLL plMiniAudioRtpcData final : public plAudioSystemRtpcData
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioRtpcData, plAudioSystemRtpcData);
};

class PL_MINIAUDIOPLUGIN_DLL plMiniAudioSwitchStateData final : public plAudioSystemSwitchStateData
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioSwitchStateData, plAudioSystemSwitchStateData);
};

class PL_MINIAUDIOPLUGIN_DLL plMiniAudioEnvironmentData final : public plAudioSystemEnvironmentData
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioEnvironmentData, plAudioSystemEnvironmentData);
};
