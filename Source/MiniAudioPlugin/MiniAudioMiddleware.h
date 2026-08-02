#pragma once

#include <AudioSystem/Core/AudioMiddleware.h>
#include <MiniAudioPlugin/Events/MiniAudioAudioEvents.h>
#include <MiniAudioPlugin/Mixer/MiniAudioBusGraph.h>
#include <MiniAudioPlugin/MiniAudioPluginDLL.h>

class plMiniAudioSingleton;

class PL_MINIAUDIOPLUGIN_DLL plMiniAudioMiddleware final : public plAudioMiddleware
{
  PL_DECLARE_SINGLETON_OF_INTERFACE(plMiniAudioMiddleware, plAudioMiddleware);

public:
  plMiniAudioMiddleware();
  ~plMiniAudioMiddleware() override;

  plResult LoadConfiguration(const plOpenDdlReaderElement& reader) override;
  plResult Startup() override;
  void Update(plTime delta) override;
  plResult Shutdown() override;
  plResult Release() override;
  plResult StopAllSounds() override;
  plResult AddEntity(plAudioSystemEntityData* pEntityData, const char* szEntityName) override;
  plResult ResetEntity(plAudioSystemEntityData* pEntityData) override;
  plResult UpdateEntity(plAudioSystemEntityData* pEntityData) override;
  plResult RemoveEntity(plAudioSystemEntityData* pEntityData) override;
  plResult SetEntityTransform(plAudioSystemEntityData* pEntityData, const plAudioSystemTransform& Transform) override;
  plResult LoadTrigger(plAudioSystemEntityData* pEntityData, const plAudioSystemTriggerData* pTriggerData, plAudioSystemEventData* pEventData) override;
  plResult ActivateTrigger(plAudioSystemEntityData* pEntityData, const plAudioSystemTriggerData* pTriggerData, plAudioSystemEventData* pEventData) override;
  plResult UnloadTrigger(plAudioSystemEntityData* pEntityData, const plAudioSystemTriggerData* pTriggerData) override;
  plResult StopEvent(plAudioSystemEntityData* pEntityData, const plAudioSystemEventData* pEventData) override;
  plResult StopAllEvents(plAudioSystemEntityData* pEntityData) override;
  plResult SetRtpc(plAudioSystemEntityData* pEntityData, const plAudioSystemRtpcData* pRtpcData, float fValue) override;
  plResult ResetRtpc(plAudioSystemEntityData* pEntityData, const plAudioSystemRtpcData* pRtpcData) override;
  plResult SetSwitchState(plAudioSystemEntityData* pEntityData, const plAudioSystemSwitchStateData* pSwitchStateData) override;
  plResult SetObstructionAndOcclusion(plAudioSystemEntityData* pEntityData, float fObstruction, float fOcclusion) override;
  plResult SetEnvironmentAmount(plAudioSystemEntityData* pEntityData, const plAudioSystemEnvironmentData* pEnvironmentData, float fAmount) override;
  plResult AddListener(plAudioSystemListenerData* pListenerData, const char* szListenerName, bool bIsDefault) override;
  plResult ResetListener(plAudioSystemListenerData* pListenerData) override;
  plResult RemoveListener(plAudioSystemListenerData* pListenerData) override;
  plResult SetListenerTransform(plAudioSystemListenerData* pListenerData, const plAudioSystemTransform& Transform) override;
  plResult LoadBank(plAudioSystemBankData* pBankData) override;
  plResult UnloadBank(plAudioSystemBankData* pBankData) override;
  plAudioSystemEntityData* CreateWorldEntity(plAudioSystemDataID uiEntityId) override;
  plAudioSystemEntityData* CreateEntityData(plAudioSystemDataID uiEntityId) override;
  plResult DestroyEntityData(plAudioSystemEntityData* pEntityData) override;
  plAudioSystemListenerData* CreateListenerData(plAudioSystemDataID uiListenerId) override;
  plResult DestroyListenerData(plAudioSystemListenerData* pListenerData) override;
  plAudioSystemEventData* CreateEventData(plAudioSystemDataID uiEventId) override;
  plResult ResetEventData(plAudioSystemEventData* pEventData) override;
  plResult DestroyEventData(plAudioSystemEventData* pEventData) override;
  plResult DestroyBank(plAudioSystemBankData* pBankData) override;
  plResult DestroyTriggerData(plAudioSystemTriggerData* pTriggerData) override;
  plResult DestroyRtpcData(plAudioSystemRtpcData* pRtpcData) override;
  plResult DestroySwitchStateData(plAudioSystemSwitchStateData* pSwitchStateData) override;
  plResult DestroyEnvironmentData(plAudioSystemEnvironmentData* pEnvironmentData) override;
  plResult SetLanguage(const char* szLanguage) override;
  const char* GetMiddlewareName() const override;
  float GetMasterGain() const override;
  bool GetMute() const override;
  void OnMasterGainChange(float fGain) override;
  void OnMuteChange(bool bMute) override;
  void OnMasterPauseChange(bool bPaused) override;
  bool GetMasterPaused() const override;
  void SetSoundGroupVolume(plStringView sGroupGuid, float fVolume) override;
  float GetSoundGroupVolume(plStringView sGroupGuid) const override;
  void SetNumListeners(plUInt8 uiNumListeners) override;
  plUInt8 GetNumListeners() const override;
  void OnLoseFocus() override;
  void OnGainFocus() override;

  bool IsEventDataPlaying(const plAudioSystemEventData* pEventData) const override;

  // mixer graph SPI
  plResult LoadMixer(const plAudioMixerDesc& desc) override;
  plResult UnloadMixer() override;
  plResult SetBusVolume(plUInt32 uiBusIndex, float fLinearGain) override;
  plResult SetBusPitch(plUInt32 uiBusIndex, float fPitch) override;
  plResult SetBusSendLevel(plUInt32 uiBusIndex, plUInt32 uiSendIndex, float fLinearLevel) override;
  plResult SetBusEffectParameter(plUInt32 uiBusIndex, plUInt32 uiEffectIndex, plUInt32 uiParamIndex, float fValue) override;
  plResult SetEventParameter(plAudioSystemEntityData* pEntityData, plStringView sParamName, float fValue) override;
  void GetProfilingInfo(plAudioSystemProfilingInfo& out_info) const override;
  void GetBusMeterInfo(plUInt32 uiBusIndex, plAudioBusMeterInfo& out_info) const override;

private:
  plMiniAudioSingleton* m_pBackend = nullptr;

  plMiniAudioBusGraph m_BusGraph;
  plMiniAudioEventManager m_EventManager;

  /// environment ids this middleware registered for the current mixer (unregistered on unload)
  plHybridArray<plAudioSystemDataID, 4> m_RegisteredEnvironmentIds;
};
