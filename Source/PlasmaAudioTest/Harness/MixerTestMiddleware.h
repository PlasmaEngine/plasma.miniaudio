#pragma once

#include <AudioSystem/Core/AudioMiddleware.h>
#include <PlasmaAudioPlugin/Mixer/MiniAudioBusGraph.h>

/// \brief A middleware that only implements the mixer graph SPI, the same way plPlasmaAudioMiddleware does: the
/// engine's plAudioMixerService drives a plMiniAudioBusGraph built on the offline engine.
///
/// UpdateMixer keeps the default behaviour (a changed layout loads the mixer again); m_uiLoads counts the loads.
class plMixerTestMiddleware final : public plAudioMiddleware
{
public:
  explicit plMixerTestMiddleware(ma_engine* pEngine)
    : m_pEngine(pEngine)
  {
  }

  PL_NODISCARD plMiniAudioBusGraph& GetBusGraph() { return m_BusGraph; }

  plUInt32 m_uiLoads = 0;

  plResult LoadMixer(const plAudioMixerDesc& desc) override
  {
    ++m_uiLoads;
    return m_BusGraph.Build(desc, m_pEngine);
  }

  plResult UnloadMixer() override
  {
    m_BusGraph.Teardown();
    return PL_SUCCESS;
  }

  plResult SetBusVolume(plUInt32 uiBusIndex, float fLinearGain) override
  {
    m_BusGraph.SetBusVolume(uiBusIndex, fLinearGain);
    return PL_SUCCESS;
  }

  plResult SetBusPitch(plUInt32 uiBusIndex, float fPitch) override
  {
    m_BusGraph.SetBusPitch(uiBusIndex, fPitch);
    return PL_SUCCESS;
  }

  plResult SetBusSendLevel(plUInt32 uiBusIndex, plUInt32 uiSendIndex, float fLinearLevel) override
  {
    m_BusGraph.SetBusSendLevel(uiBusIndex, uiSendIndex, fLinearLevel);
    return PL_SUCCESS;
  }

  plResult SetBusEffectParameter(plUInt32 uiBusIndex, plUInt32 uiEffectIndex, plUInt32 uiParamIndex, float fValue) override
  {
    m_BusGraph.SetBusEffectParameter(uiBusIndex, uiEffectIndex, uiParamIndex, fValue);
    return PL_SUCCESS;
  }

  plResult SetBusEffectBypass(plUInt32 uiBusIndex, plUInt32 uiEffectIndex, bool bBypass) override
  {
    m_BusGraph.SetBusEffectBypass(uiBusIndex, uiEffectIndex, bBypass);
    return PL_SUCCESS;
  }

  // the rest of the middleware interface is not used by the mixer service
  plResult LoadConfiguration(const plOpenDdlReaderElement&) override { return PL_SUCCESS; }
  plResult Startup() override { return PL_SUCCESS; }
  void Update(plTime) override {}
  plResult Shutdown() override { return PL_SUCCESS; }
  plResult Release() override { return PL_SUCCESS; }
  plResult StopAllSounds() override { return PL_SUCCESS; }
  plResult AddEntity(plAudioSystemEntityData*, const char*) override { return PL_SUCCESS; }
  plResult ResetEntity(plAudioSystemEntityData*) override { return PL_SUCCESS; }
  plResult UpdateEntity(plAudioSystemEntityData*) override { return PL_SUCCESS; }
  plResult RemoveEntity(plAudioSystemEntityData*) override { return PL_SUCCESS; }
  plResult SetEntityTransform(plAudioSystemEntityData*, const plAudioSystemTransform&) override { return PL_SUCCESS; }
  plResult LoadTrigger(plAudioSystemEntityData*, const plAudioSystemTriggerData*, plAudioSystemEventData*) override { return PL_SUCCESS; }
  plResult ActivateTrigger(plAudioSystemEntityData*, const plAudioSystemTriggerData*, plAudioSystemEventData*) override { return PL_SUCCESS; }
  plResult UnloadTrigger(plAudioSystemEntityData*, const plAudioSystemTriggerData*) override { return PL_SUCCESS; }
  plResult StopEvent(plAudioSystemEntityData*, const plAudioSystemEventData*) override { return PL_SUCCESS; }
  plResult StopAllEvents(plAudioSystemEntityData*) override { return PL_SUCCESS; }
  plResult SetRtpc(plAudioSystemEntityData*, const plAudioSystemRtpcData*, float) override { return PL_SUCCESS; }
  plResult ResetRtpc(plAudioSystemEntityData*, const plAudioSystemRtpcData*) override { return PL_SUCCESS; }
  plResult SetSwitchState(plAudioSystemEntityData*, const plAudioSystemSwitchStateData*) override { return PL_SUCCESS; }
  plResult SetObstructionAndOcclusion(plAudioSystemEntityData*, float, float) override { return PL_SUCCESS; }
  plResult SetEnvironmentAmount(plAudioSystemEntityData*, const plAudioSystemEnvironmentData*, float) override { return PL_SUCCESS; }
  plResult AddListener(plAudioSystemListenerData*, const char*, bool) override { return PL_SUCCESS; }
  plResult ResetListener(plAudioSystemListenerData*) override { return PL_SUCCESS; }
  plResult RemoveListener(plAudioSystemListenerData*) override { return PL_SUCCESS; }
  plResult SetListenerTransform(plAudioSystemListenerData*, const plAudioSystemTransform&) override { return PL_SUCCESS; }
  plResult LoadBank(plAudioSystemBankData*) override { return PL_SUCCESS; }
  plResult UnloadBank(plAudioSystemBankData*) override { return PL_SUCCESS; }
  plAudioSystemEntityData* CreateWorldEntity(plAudioSystemDataID) override { return nullptr; }
  plAudioSystemEntityData* CreateEntityData(plAudioSystemDataID) override { return nullptr; }
  plResult DestroyEntityData(plAudioSystemEntityData*) override { return PL_SUCCESS; }
  plAudioSystemListenerData* CreateListenerData(plAudioSystemDataID) override { return nullptr; }
  plResult DestroyListenerData(plAudioSystemListenerData*) override { return PL_SUCCESS; }
  plAudioSystemEventData* CreateEventData(plAudioSystemDataID) override { return nullptr; }
  plResult ResetEventData(plAudioSystemEventData*) override { return PL_SUCCESS; }
  plResult DestroyEventData(plAudioSystemEventData*) override { return PL_SUCCESS; }
  plResult DestroyBank(plAudioSystemBankData*) override { return PL_SUCCESS; }
  plResult DestroyTriggerData(plAudioSystemTriggerData*) override { return PL_SUCCESS; }
  plResult DestroyRtpcData(plAudioSystemRtpcData*) override { return PL_SUCCESS; }
  plResult DestroySwitchStateData(plAudioSystemSwitchStateData*) override { return PL_SUCCESS; }
  plResult DestroyEnvironmentData(plAudioSystemEnvironmentData*) override { return PL_SUCCESS; }
  plResult SetLanguage(const char*) override { return PL_SUCCESS; }
  const char* GetMiddlewareName() const override { return "MixerTest"; }
  float GetMasterGain() const override { return 1.0f; }
  bool GetMute() const override { return false; }
  void OnMasterGainChange(float) override {}
  void OnMuteChange(bool) override {}
  void OnLoseFocus() override {}
  void OnGainFocus() override {}

private:
  ma_engine* m_pEngine = nullptr;
  plMiniAudioBusGraph m_BusGraph;
};
