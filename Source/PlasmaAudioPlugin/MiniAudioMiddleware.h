#pragma once

#include <AudioSystem/Core/AudioMiddleware.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Configuration/Singleton.h>
#include <PlasmaAudioPlugin/Banks/AudioBankResource.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResource.h>
#include <PlasmaAudioPlugin/Events/AudioAudition.h>
#include <PlasmaAudioPlugin/Events/AudioEventResource.h>
#include <PlasmaAudioPlugin/Events/MiniAudioAudioEvents.h>
#include <PlasmaAudioPlugin/Mixer/MiniAudioBusGraph.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>
#include <PlasmaAudioPlugin/Streaming/AudioStreamService.h>

#include <atomic>

class plMiniAudioSingleton;

class PL_PLASMAAUDIOPLUGIN_DLL plPlasmaAudioMiddleware final : public plAudioMiddleware, public plAudioPcmStreamProvider
{
  PL_DECLARE_SINGLETON_OF_INTERFACE(plPlasmaAudioMiddleware, plAudioMiddleware);

public:
  plPlasmaAudioMiddleware();
  ~plPlasmaAudioMiddleware() override;

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

  /// \brief Global parameters feed every event parameter of the same name that has no instance-local value.
  /// Thread-safe; the event manager picks up changes on the next Update.
  void SetGlobalParameter(plStringView sName, float fValue) override;
  float GetGlobalParameter(plStringView sName) const override;

  bool IsEventDataPlaying(const plAudioSystemEventData* pEventData) const override;
  void TakeEventMarkers(plAudioSystemEventData* pEventData, plDynamicArray<plString>& out_markers) override;

  // mixer graph SPI: the engine's plAudioMixerService loads the mixer resource, blends VCAs and snapshots and
  // pushes the final per-bus values; they land on the plMiniAudioBusGraph built from the desc
  plResult LoadMixer(const plAudioMixerDesc& desc) override;
  plResult UnloadMixer() override;

  /// \brief A changed layout is rebuilt without stopping the voices: the output fades out for MixerRebuildFade, the
  /// graph is rebuilt and the voices and PCM streams move onto it, then the output fades back in. Values set while the
  /// rebuild waits for the fade apply to the new graph.
  plResult UpdateMixer(const plAudioMixerDesc& desc, bool bLayoutChanged) override;
  plResult SetBusVolume(plUInt32 uiBusIndex, float fLinearGain) override;
  plResult SetBusPitch(plUInt32 uiBusIndex, float fPitch) override;
  plResult SetBusSendLevel(plUInt32 uiBusIndex, plUInt32 uiSendIndex, float fLinearLevel) override;
  plResult SetBusEffectParameter(plUInt32 uiBusIndex, plUInt32 uiEffectIndex, plUInt32 uiParamIndex, float fValue) override;
  plResult SetBusEffectBypass(plUInt32 uiBusIndex, plUInt32 uiEffectIndex, bool bBypass) override;
  void GetBusMeterInfo(plUInt32 uiBusIndex, plAudioBusMeterInfo& out_info) const override;

  /// \brief How long the output fades out, and then in again, around an in-place rebuild of the mixer graph.
  static constexpr plTime MixerRebuildFade = plTime::MakeFromMilliseconds(25);

  /// \brief The loaded mixer graph. Audio thread only.
  PL_NODISCARD const plMiniAudioBusGraph& GetBusGraph() const { return m_BusGraph; }

  /// \brief Whether an in-place rebuild of the mixer graph waits for its fade-out. Audio thread only.
  PL_NODISCARD bool IsMixerRebuildPending() const { return m_bRebuildPending; }
  plResult SetEventParameter(plAudioSystemEntityData* pEntityData, plStringView sParamName, float fValue) override;
  void GetProfilingInfo(plAudioSystemProfilingInfo& out_info) const override;

  /// \brief Wall time the last Update() took, in milliseconds. Any thread.
  float GetLastUpdateMilliseconds() const { return m_fLastUpdateMs.load(std::memory_order_relaxed); }

  /// \brief Publishes the latest profiling snapshot of the audio thread as plStats: "PlasmaAudio/VoiceSnapshot" (the
  /// serialized plAudioProfilingSnapshot as a plDataBuffer) and the scalars "PlasmaAudio/RealVoices",
  /// "PlasmaAudio/VirtualVoices", "PlasmaAudio/DspLoad" and "PlasmaAudio/UpdateMs". The stats are set only when a new
  /// snapshot arrived and removed once PlasmaAudio.ProfilerSnapshotHz is 0. Main thread only (plStats is not
  /// synchronized with its readers).
  void PublishStats();

  /// \brief Removes the stats PublishStats set, if any. Main thread only.
  void RemoveProfilingStats();

  /// \brief Peak and RMS of a bus of the loaded mixer (zero without a mixer). Audio thread only.
  void GetBusMeter(plUInt32 uiBusIndex, float& out_fPeak, float& out_fRms) const;

  /// \brief Whether the bank was loaded through LoadBank and the load of its events and clips has been started.
  /// Audio thread only.
  bool IsBankContentRequested(plAudioSystemDataID uiBankId) const;

  /// \brief Whether every event and clip of the bank finished loading (or is missing), so playing them causes no load.
  /// Audio thread only.
  bool IsBankReady(plAudioSystemDataID uiBankId) const;

  /// \brief Whether the loaded content was over a category of the memory budget when the bank finished loading. Audio
  /// thread only.
  bool IsBankOverBudget(plAudioSystemDataID uiBankId) const;

  /// \brief The memory budget of the last loaded bank that has one; banks bake the budget of the platform they were
  /// built for. A bank that finishes loading with the loaded content over a category of it logs a warning and a
  /// BudgetExceeded event log entry. Audio thread only.
  const plAudioMemoryBudget& GetMemoryBudget() const { return m_MemoryBudget; }

  /// \brief Memory of the loaded clips by load mode, and of the stream rings. Any thread.
  plAudioMemoryUsage GetMemoryUsage() const;

  /// \brief The language clips are loaded in ("" = default language). SetLanguage() takes effect on the next Update().
  plString GetLanguage() const;

  // PCM stream SPI: a plMaPcmStreamNode per stream, attached to its bus (or the endpoint without a mixer)
  plResult CreatePcmStream(const plAudioPcmStreamDesc& desc, plUInt32& out_uiStreamId) override;
  void DestroyPcmStream(plUInt32 uiStreamId) override;
  plUInt32 WritePcmFrames(plUInt32 uiStreamId, plArrayPtr<const float> interleaved) override;
  plUInt32 GetPcmStreamFreeFrames(plUInt32 uiStreamId) const override;
  plUInt64 GetPcmStreamFramesRendered(plUInt32 uiStreamId) const override;
  void SetPcmStreamVolume(plUInt32 uiStreamId, float fLinearGain) override;
  void SetPcmStreamPitch(plUInt32 uiStreamId, float fPitch) override;
  void SetPcmStreamPaused(plUInt32 uiStreamId, bool bPaused) override;
  void FlushPcmStream(plUInt32 uiStreamId) override;

private:
  struct PcmStream
  {
    plUInt32 m_uiId = 0;
    plString m_sBus;
    plUInt32 m_uiChannels = 0;
    plUInt32 m_uiSampleRate = 0;
    plUInt32 m_uiRingFrames = 0;
    plUniquePtr<plMaPcmStreamNode> m_pNode;
  };

  PcmStream* FindPcmStream(plUInt32 uiStreamId);
  const PcmStream* FindPcmStream(plUInt32 uiStreamId) const;
  void AttachPcmStream(PcmStream& ref_stream);
  void ReattachPcmStreams();
  void DetachPcmStreamsToEndpoint();
  void DestroyAllPcmStreams();

  /// \brief Recreates the miniaudio engine on the requested / default output device and rebuilds what hangs off it.
  void RecoverDevice();

  /// \brief Rebuilds the graph from m_RebuildDesc once the fade-out of UpdateMixer is over.
  void FinishMixerRebuild();

  /// \brief The values the SetBus... functions change: those of the pending rebuild while one waits, else nullptr.
  plMiniAudioBusGraph::BusValues* GetPendingRebuildValues(plUInt32 uiBusIndex);

  /// \brief Registers the mixer's environment aux buses as ATL environments, and unregisters them again.
  void RegisterMixerEnvironments(const plAudioMixerDesc& desc);
  void UnregisterMixerEnvironments();

  plMiniAudioSingleton* m_pBackend = nullptr;

  /// duration of the last Update(), written by the audio thread
  std::atomic<float> m_fLastUpdateMs = 0.0f;

  /// which listener slots 1..MaxListeners-1 are taken by a listener; slot 0 is shared by default listeners
  bool m_bListenerSlotTaken[4] = {};

  /// the mixer loaded through LoadMixer (not built without one); rebuilt with its current values after a device re-init
  plMiniAudioBusGraph m_BusGraph;

  /// the mixer to rebuild once a device re-init that failed is retried successfully
  plAudioMixerDesc m_PendingMixerDesc;
  plDynamicArray<plMiniAudioBusGraph::BusValues> m_PendingMixerValues;
  bool m_bMixerPendingRecovery = false;

  /// an in-place rebuild (UpdateMixer) waiting for the output's fade-out
  plAudioMixerDesc m_RebuildDesc;
  plDynamicArray<plMiniAudioBusGraph::BusValues> m_RebuildValues;
  bool m_bRebuildPending = false;
  plUInt64 m_uiRebuildFrame = 0; ///< engine time at which the fade-out is over

  /// declared before the event manager: voices of streamed clips release their streams into it
  plAudioStreamService m_StreamService;
  plPlasmaAudioEventManager m_EventManager;

  /// the editor's auditions, played through m_EventManager
  plAudioAudition m_Audition;

  /// environment ids this middleware registered for the current mixer (unregistered on unload)
  plHybridArray<plAudioSystemDataID, 4> m_RegisteredEnvironmentIds;

  /// global parameters (name hash -> value); copied into the event manager in Update when dirty
  mutable plMutex m_GlobalParameterMutex;
  plHashTable<plUInt64, float> m_GlobalParameters;
  bool m_bGlobalParametersDirty = false;

  mutable plMutex m_PcmStreamMutex;
  plHybridArray<PcmStream, 4> m_PcmStreams;
  plUInt32 m_uiNextPcmStreamId = 1;

  /// A bank loaded through LoadBank: the bank resource and, once its content list is loaded, a handle per listed
  /// event and clip. The held handles keep those resources from being freed.
  struct LoadedBank
  {
    plAudioBankResourceHandle m_hBank;
    plDynamicArray<plAudioEventResourceHandle> m_Events;
    plDynamicArray<plAudioClipResourceHandle> m_Clips;
    bool m_bContentRequested = false;
    bool m_bReady = false;      ///< every event and clip finished loading; the budget was checked then
    bool m_bOverBudget = false; ///< that check found the loaded content over a category of the budget
  };

  /// Starts loading the events and clips of the bank once its resource is loaded, from the bank file when it packs
  /// them. Returns false while it is still loading.
  bool RequestBankContent(plAudioSystemDataID uiBankId, LoadedBank& ref_bank);
  void UpdatePendingBanks();

  /// Checks the banks that finished loading against the memory budget and refreshes the audition's bank monitor while
  /// it is watched (audio thread).
  void UpdateBankReadiness();

  /// Logs it when the loaded content is over a category of m_MemoryBudget.
  void CheckMemoryBudget(LoadedBank& ref_bank);

  /// Whether every event and clip the bank holds a handle to has finished loading or is missing.
  static bool IsBankContentLoaded(const LoadedBank& bank);

  /// The events and clips of the bank that finished loading or are missing.
  static plUInt32 CountReadyItems(const LoadedBank& bank);

  plAudioMemoryBudget m_MemoryBudget;
  plTime m_LastBankMonitorRefresh;

  /// stream underruns already reported in the event log
  plUInt64 m_uiLoggedUnderruns = 0;

  /// Applies a language set through SetLanguage (audio thread).
  void ApplyPendingLanguage();

  /// Reloads the localized clips of m_LanguageReloads that are loaded in an older language (audio thread).
  void RetryLanguageReloads();

  /// localized clips a language switch still has to reload, until m_LanguageReloadDeadline
  plDynamicArray<plAudioClipResourceHandle> m_LanguageReloads;
  plTime m_LanguageReloadDeadline;

  void OnLanguageCVarEvent(const plCVarEvent& e);

  plHashTable<plAudioSystemDataID, LoadedBank> m_Banks;

  /// banks in m_Banks whose content has not been requested yet
  plHybridArray<plAudioSystemDataID, 4> m_PendingBanks;

  /// language requested through SetLanguage, applied in Update
  mutable plMutex m_LanguageMutex;
  plString m_sPendingLanguage;
  bool m_bLanguagePending = false;

  plCVar* m_pLanguageCVar = nullptr;
  plEventSubscriptionID m_LanguageCVarSubscription = 0;

  /// Serializes a profiling snapshot when PlasmaAudio.ProfilerSnapshotHz asks for one (audio thread).
  void UpdateProfilingSnapshot();

  /// Fills the bus section of m_ProfilingSnapshot from the bus graph's gains and meters (audio thread).
  void FillBusSnapshot();

  /// Fills the memory section of m_ProfilingSnapshot from the clip, event and bank registries (audio thread).
  void FillMemorySnapshot();

  /// audio thread: the snapshot and its serialized form are reused, so their capacity carries over
  plAudioProfilingSnapshot m_ProfilingSnapshot;
  plDataBuffer m_ProfilingScratch;
  plTime m_LastProfilingSnapshot;

  /// A snapshot carries the event log entries added since the last ProfilingLogOverlap snapshots, so a reader that
  /// polls the published snapshot less often than it is produced still sees every entry.
  static constexpr plUInt32 ProfilingLogOverlap = 8;

  /// the event log's next sequence number at each of the last ProfilingLogOverlap snapshots, a ring
  plUInt64 m_ProfilingLogCursors[ProfilingLogOverlap] = {};
  plUInt32 m_uiProfilingLogCursorSlot = 0;

  /// The latest serialized snapshot, swapped in by the audio thread and copied out by PublishStats, both under
  /// m_ProfilingMutex. The audio thread holds it only for the swap.
  plMutex m_ProfilingMutex;
  plDataBuffer m_ProfilingPublished;
  plUInt32 m_uiProfilingGeneration = 0;
  plUInt32 m_uiProfilingPublishedReal = 0;
  plUInt32 m_uiProfilingPublishedVirtual = 0;
  float m_fProfilingPublishedDspLoad = 0.0f;
  float m_fProfilingPublishedUpdateMs = 0.0f;

  /// main thread (PublishStats)
  plDataBuffer m_ProfilingStatBuffer;
  plUInt32 m_uiProfilingGenerationSeen = 0;
  bool m_bProfilingStatsSet = false;
};
