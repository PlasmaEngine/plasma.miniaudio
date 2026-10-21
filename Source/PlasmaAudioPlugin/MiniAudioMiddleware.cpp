#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <AudioSystem/Core/AudioSystem.h>
#include <AudioSystem/Core/AudioSystemAllocator.h>
#include <AudioSystem/Core/AudioSystemData.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Types/ScopeExit.h>
#include <Foundation/Utilities/Stats.h>
#include <PlasmaAudioPlugin/Banks/AudioPackedContent.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResourceLoader.h>
#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioPlugin/MiniAudioMiddleware.h>
#include <PlasmaAudioPlugin/MiniAudioSingleton.h>
#include <PlasmaAudioPlugin/Profiling/AudioDebugDraw.h>
#include <PlasmaAudioPlugin/Resources/MiniAudioSoundResource.h>

PL_IMPLEMENT_SINGLETON(plPlasmaAudioMiddleware);

plCVarInt cvar_PlasmaAudioProfilerSnapshotHz("PlasmaAudio.ProfilerSnapshotHz", 0, plCVarFlags::Default, "Rate at which the audio thread captures a voice snapshot for the editor's Audio Profiler; 0 disables it");

plPlasmaAudioEntityData::plPlasmaAudioEntityData(plAudioSystemDataID uiEntityId, bool bIsGlobal)
  : m_uiEntityId(uiEntityId)
  , m_bGlobalEntity(bIsGlobal)
{
}

plPlasmaAudioListenerData::plPlasmaAudioListenerData(plAudioSystemDataID uiListenerId)
  : m_uiListenerId(uiListenerId)
{
}

plPlasmaAudioEventData::plPlasmaAudioEventData(plAudioSystemDataID uiEventId)
  : m_uiEventId(uiEventId)
{
}

plPlasmaAudioTriggerData::plPlasmaAudioTriggerData(plAudioSystemDataID uiTriggerId, plStringView sResourceId)
  : m_uiTriggerId(uiTriggerId)
  , m_sResourceId(sResourceId)
{
}

plPlasmaAudioBankData::plPlasmaAudioBankData(plAudioSystemDataID uiBankId, plStringView sBankName)
  : m_uiBankId(uiBankId)
  , m_sBankName(sBankName)
{
}

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plPlasmaAudioEntityData, 1, plRTTIDefaultAllocator<plPlasmaAudioEntityData>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plPlasmaAudioListenerData, 1, plRTTIDefaultAllocator<plPlasmaAudioListenerData>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plPlasmaAudioEventData, 1, plRTTIDefaultAllocator<plPlasmaAudioEventData>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plPlasmaAudioTriggerData, 1, plRTTIDefaultAllocator<plPlasmaAudioTriggerData>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plPlasmaAudioBankData, 1, plRTTIDefaultAllocator<plPlasmaAudioBankData>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plPlasmaAudioRtpcData, 1, plRTTIDefaultAllocator<plPlasmaAudioRtpcData>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plPlasmaAudioSwitchStateData, 1, plRTTIDefaultAllocator<plPlasmaAudioSwitchStateData>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plPlasmaAudioEnvironmentData, 1, plRTTIDefaultAllocator<plPlasmaAudioEnvironmentData>)
PL_END_DYNAMIC_REFLECTED_TYPE;

plPlasmaAudioMiddleware::plPlasmaAudioMiddleware()
  : m_SingletonRegistrar(this)
{
  // the PCM stream interface is registered on its own, so consumers find it only when this middleware has it
  plSingletonRegistry::Register<plAudioPcmStreamProvider>(this);
}

plPlasmaAudioMiddleware::~plPlasmaAudioMiddleware()
{
  plSingletonRegistry::Unregister<plAudioPcmStreamProvider>();
}

plResult plPlasmaAudioMiddleware::LoadConfiguration(const plOpenDdlReaderElement& reader)
{
  PL_IGNORE_UNUSED(reader);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::Startup()
{
  m_pBackend = plMiniAudioSingleton::GetSingleton();
  if (m_pBackend == nullptr)
    return PL_FAILURE;

  m_pBackend->Startup();

  m_StreamService.Startup();
  m_EventManager.SetStreamService(&m_StreamService);

  plAudioAudition::s_pSingleton = &m_Audition;
  plAudioDebugDraw::Startup();

  // the engine never calls SetLanguage itself: follow the App.Language cvar of the game application instead
  m_pLanguageCVar = plCVar::FindCVarByName("App.Language");
  if (m_pLanguageCVar != nullptr && m_pLanguageCVar->GetType() == plCVarType::String)
  {
    m_LanguageCVarSubscription = m_pLanguageCVar->m_CVarEvents.AddEventHandler(plMakeDelegate(&plPlasmaAudioMiddleware::OnLanguageCVarEvent, this));
    SetLanguage(static_cast<plCVarString*>(m_pLanguageCVar)->GetValue().GetData()).IgnoreResult();
  }
  else
  {
    m_pLanguageCVar = nullptr;
  }

  return PL_SUCCESS;
}

void plPlasmaAudioMiddleware::Update(plTime delta)
{
  PL_PROFILE_SCOPE("PlasmaAudio.Update");

  const plTime tStart = plTime::Now();
  PL_SCOPE_EXIT(m_fLastUpdateMs.store(static_cast<float>((plTime::Now() - tStart).GetMilliseconds()), std::memory_order_relaxed));

  ApplyPendingLanguage();
  if (!m_LanguageReloads.IsEmpty())
  {
    RetryLanguageReloads();
  }
  UpdatePendingBanks();
  UpdateBankReadiness();

  if (m_pBackend != nullptr)
  {
    if (m_pBackend->IsDeviceReinitRequired())
    {
      RecoverDevice();
    }

    m_pBackend->UpdateSound(delta);

    // an in-place rebuild of the mixer happens once the output has faded out
    if (m_bRebuildPending && m_pBackend->GetEngine() != nullptr && ma_engine_get_time_in_pcm_frames(m_pBackend->GetEngine()) >= m_uiRebuildFrame)
    {
      FinishMixerRebuild();
    }

    {
      PL_LOCK(m_GlobalParameterMutex);
      if (m_bGlobalParametersDirty)
      {
        m_EventManager.SetGlobalParameters(m_GlobalParameters);
        m_bGlobalParametersDirty = false;
      }
    }

    // voices are spatialized against their closest listener, so audibility and distance use the closest one as well
    float fMasterPeak = 0.0f;
    float fMasterRms = 0.0f;
    GetBusMeter(0, fMasterPeak, fMasterRms);
    m_Audition.Update(m_EventManager, m_BusGraph, m_pBackend->GetEngine(), fMasterPeak);

    plVec3 listenerPositions[plMiniAudioSingleton::MaxListeners];
    const plUInt32 uiListeners = m_pBackend->GetActiveListenerPositions(plArrayPtr<plVec3>(listenerPositions));
    m_EventManager.Update(delta, plArrayPtr<const plVec3>(listenerPositions, uiListeners));

    UpdateProfilingSnapshot();

    // the Audio view mode draws from this while a view shows it
    if (plAudioDebugDraw::IsWanted())
    {
      plAudioDebugSnapshot snapshot;
      m_EventManager.GetDebugSnapshot(snapshot);
      plAudioDebugDraw::Publish(std::move(snapshot));
    }
  }
}

void plPlasmaAudioMiddleware::UpdateProfilingSnapshot()
{
  const plInt32 iHz = cvar_PlasmaAudioProfilerSnapshotHz.GetValue();
  if (iHz <= 0)
    return;

  const plTime tNow = plTime::Now();
  if (tNow - m_LastProfilingSnapshot < plTime::MakeFromSeconds(1.0 / plMath::Min(iHz, 1000)))
    return;

  m_LastProfilingSnapshot = tNow;

  PL_PROFILE_SCOPE("PlasmaAudio.ProfilingSnapshot");

  m_EventManager.FillSnapshot(m_ProfilingSnapshot);
  m_ProfilingSnapshot.m_uiStreams = m_StreamService.GetActiveStreamCount();
  m_ProfilingSnapshot.m_fDspLoad = m_pBackend->GetDspLoad();
  m_ProfilingSnapshot.m_fUpdateMs = GetLastUpdateMilliseconds();
  m_ProfilingSnapshot.m_uiMemoryBytes = plAudioClipResource::GetTotalResidentBytes() + m_StreamService.GetRingBytesInUse();

  FillBusSnapshot();
  FillMemorySnapshot();

  // the oldest cursor of the ring is where the entries of this snapshot start; it is replaced by the current end
  {
    const plAudioEventLog& log = m_EventManager.GetEventLog();
    plUInt64& uiCursor = m_ProfilingLogCursors[m_uiProfilingLogCursorSlot];
    m_ProfilingSnapshot.m_uiEventLogFirstSequence = log.Collect(uiCursor, m_ProfilingSnapshot.m_EventLog);
    uiCursor = m_ProfilingSnapshot.m_uiEventLogFirstSequence + m_ProfilingSnapshot.m_EventLog.GetCount();
    m_uiProfilingLogCursorSlot = (m_uiProfilingLogCursorSlot + 1) % ProfilingLogOverlap;
  }

  m_ProfilingScratch.Clear();
  {
    plMemoryStreamContainerWrapperStorage<plDataBuffer> storage(&m_ProfilingScratch);
    plMemoryStreamWriter writer(&storage);
    m_ProfilingSnapshot.Write(writer);
  }

  // only the swap happens under the lock; the old published buffer becomes the next scratch buffer
  PL_LOCK(m_ProfilingMutex);
  m_ProfilingPublished.Swap(m_ProfilingScratch);
  m_uiProfilingPublishedReal = m_ProfilingSnapshot.m_uiRealVoices;
  m_uiProfilingPublishedVirtual = m_ProfilingSnapshot.m_uiVirtualVoices;
  m_fProfilingPublishedDspLoad = m_ProfilingSnapshot.m_fDspLoad;
  m_fProfilingPublishedUpdateMs = m_ProfilingSnapshot.m_fUpdateMs;
  ++m_uiProfilingGeneration;
}

void plPlasmaAudioMiddleware::FillBusSnapshot()
{
  m_ProfilingSnapshot.m_sMixer.Clear();
  m_ProfilingSnapshot.m_Buses.Clear();
  m_ProfilingSnapshot.m_ActiveSnapshots.Clear();

  // without a mixer there is no bus graph and no meter; the profiler reports that no mixer is loaded
  if (!m_BusGraph.IsBuilt())
    return;

  const plAudioMixerDesc& desc = m_BusGraph.GetDesc();

  // the mixer resource id and the snapshot intensities live in the engine's mixer service (this runs on the audio thread)
  if (const plAudioSystem* pAudioSystem = plAudioSystem::GetSingleton())
  {
    const plAudioMixerService& mixerService = pAudioSystem->GetMixerService();
    m_ProfilingSnapshot.m_sMixer = mixerService.GetMixerResourceId();

    const plAudioMixerDesc& serviceDesc = mixerService.GetDesc();
    for (plUInt32 uiSnapshot = 0; uiSnapshot < serviceDesc.m_Snapshots.GetCount(); ++uiSnapshot)
    {
      const float fIntensity = mixerService.GetSnapshotIntensity(uiSnapshot);
      if (fIntensity <= 0.0f)
        continue;

      plAudioActiveSnapshotEntry& entry = m_ProfilingSnapshot.m_ActiveSnapshots.ExpandAndGetRef();
      entry.m_sName = serviceDesc.m_Snapshots[uiSnapshot].m_sName;
      entry.m_fIntensity = fIntensity;
    }
  }

  m_ProfilingSnapshot.m_Buses.SetCount(desc.m_Buses.GetCount());
  for (plUInt32 uiBus = 0; uiBus < desc.m_Buses.GetCount(); ++uiBus)
  {
    const plAudioBusDesc& busDesc = desc.m_Buses[uiBus];
    plAudioBusSnapshotEntry& bus = m_ProfilingSnapshot.m_Buses[uiBus];
    bus.m_sName = busDesc.m_sName;
    bus.m_uiParent = busDesc.m_uiParent < desc.m_Buses.GetCount() ? busDesc.m_uiParent : plAudioBusSnapshotEntry::NoParent;
    bus.m_bIsAux = busDesc.m_bIsAux;
    bus.m_fGainDb = plAudioMixerUtils::LinearToDb(m_BusGraph.GetBusGain(uiBus));
    m_BusGraph.GetBusMeter(uiBus, bus.m_fPeak, bus.m_fRms);
  }
}

void plPlasmaAudioMiddleware::FillMemorySnapshot()
{
  plAudioClipResource::GatherLoadedClips(m_ProfilingSnapshot.m_Clips, &m_StreamService);
  plAudioEventResource::GatherLoadedEvents(m_ProfilingSnapshot.m_Events);

  m_ProfilingSnapshot.m_Banks.Clear();
  for (auto it = m_Banks.GetIterator(); it.IsValid(); ++it)
  {
    const LoadedBank& loadedBank = it.Value();

    plAudioBankMemorySnapshotEntry& bank = m_ProfilingSnapshot.m_Banks.ExpandAndGetRef();
    bank.m_sName = loadedBank.m_hBank.GetResourceID();
    bank.m_uiEvents = loadedBank.m_Events.GetCount();
    bank.m_uiClips = loadedBank.m_Clips.GetCount();
    bank.m_uiState = plAudioBankMemorySnapshotEntry::Loading;

    if (loadedBank.m_bContentRequested)
    {
      plResourceLock<plAudioBankResource> pBank(loadedBank.m_hBank, plResourceAcquireMode::PointerOnly);

      if (pBank->GetLoadingState() == plResourceState::LoadedResourceMissing)
      {
        bank.m_uiState = plAudioBankMemorySnapshotEntry::Missing;
      }
      else if (IsBankContentLoaded(loadedBank))
      {
        bank.m_uiState = plAudioBankMemorySnapshotEntry::Loaded;
      }
    }
  }

  m_ProfilingSnapshot.m_uiClipResidentBytes = plAudioClipResource::GetTotalResidentBytes();
  m_ProfilingSnapshot.m_uiStreamRingBytes = m_StreamService.GetRingBytesInUse();
  m_ProfilingSnapshot.m_uiPooledStreams = m_StreamService.GetPooledStreamCount();
}

void plPlasmaAudioMiddleware::PublishStats()
{
  if (cvar_PlasmaAudioProfilerSnapshotHz.GetValue() <= 0)
  {
    RemoveProfilingStats();
    return;
  }

  plUInt32 uiReal = 0;
  plUInt32 uiVirtual = 0;
  float fDspLoad = 0.0f;
  float fUpdateMs = 0.0f;

  {
    PL_LOCK(m_ProfilingMutex);

    if (m_uiProfilingGeneration == m_uiProfilingGenerationSeen)
      return;

    m_uiProfilingGenerationSeen = m_uiProfilingGeneration;
    m_ProfilingStatBuffer = m_ProfilingPublished;
    uiReal = m_uiProfilingPublishedReal;
    uiVirtual = m_uiProfilingPublishedVirtual;
    fDspLoad = m_fProfilingPublishedDspLoad;
    fUpdateMs = m_fProfilingPublishedUpdateMs;
  }

  plStats::SetStat("PlasmaAudio/VoiceSnapshot", plVariant(m_ProfilingStatBuffer));
  plStats::SetStat("PlasmaAudio/RealVoices", uiReal);
  plStats::SetStat("PlasmaAudio/VirtualVoices", uiVirtual);
  plStats::SetStat("PlasmaAudio/DspLoad", fDspLoad);
  plStats::SetStat("PlasmaAudio/UpdateMs", fUpdateMs);
  m_bProfilingStatsSet = true;
}

void plPlasmaAudioMiddleware::RemoveProfilingStats()
{
  if (!m_bProfilingStatsSet)
    return;

  plStats::RemoveStat("PlasmaAudio/VoiceSnapshot");
  plStats::RemoveStat("PlasmaAudio/RealVoices");
  plStats::RemoveStat("PlasmaAudio/VirtualVoices");
  plStats::RemoveStat("PlasmaAudio/DspLoad");
  plStats::RemoveStat("PlasmaAudio/UpdateMs");
  m_bProfilingStatsSet = false;
}

plResult plPlasmaAudioMiddleware::Shutdown()
{
  if (m_pLanguageCVar != nullptr)
  {
    m_pLanguageCVar->m_CVarEvents.RemoveEventHandler(m_LanguageCVarSubscription);
    m_pLanguageCVar = nullptr;
  }

  for (auto it = m_Banks.GetIterator(); it.IsValid(); ++it)
  {
    plAudioPackedContent::Unregister(it.Key());
  }

  m_Banks.Clear();
  m_PendingBanks.Clear();
  m_LanguageReloads.Clear();

  plAudioAudition::s_pSingleton = nullptr;
  plAudioDebugDraw::Shutdown();
  m_Audition.StopAll(m_EventManager);

  m_EventManager.StopAll(true);
  DestroyAllPcmStreams();
  UnloadMixer().IgnoreResult();

  // after the voices, which release their streams
  m_EventManager.SetStreamService(nullptr);
  m_StreamService.Shutdown();

  if (m_pBackend != nullptr)
  {
    m_pBackend->Shutdown();
  }

  for (bool& bTaken : m_bListenerSlotTaken)
  {
    bTaken = false;
  }
  return PL_SUCCESS;
}

void plPlasmaAudioMiddleware::RecoverDevice()
{
  // Re-initializing the engine invalidates every ma_sound and ma_node created on the old one.
  // Event instances playing at this point are stopped, not resumed on the new device. PCM streams keep
  // their ids and settings but lose buffered frames. The mixer graph is rebuilt from its desc with the values the
  // mixer service had pushed, so runtime VCA / snapshot values carry over.
  plLog::Warning("PlasmaAudio: recreating the audio engine for an output device change; playing sounds are stopped.");
  m_EventManager.GetEventLog().Add(plAudioLogEventType::DeviceLost, "output device", "recreating the audio engine");

  // held throughout, so no PCM stream node is created, fed or destroyed against a half-rebuilt engine
  PL_LOCK(m_PcmStreamMutex);

  // after a failed attempt the graph is gone and the mixer waits in m_PendingMixerDesc; an in-place rebuild that was
  // waiting for its fade-out is built on the new device instead
  const bool bHadMixer = m_BusGraph.IsBuilt() || m_bMixerPendingRecovery;
  const plAudioMixerDesc mixerDesc = m_bRebuildPending ? m_RebuildDesc : (m_BusGraph.IsBuilt() ? m_BusGraph.GetDesc() : m_PendingMixerDesc);
  const plDynamicArray<plMiniAudioBusGraph::BusValues> mixerValues = m_bRebuildPending ? m_RebuildValues : (m_BusGraph.IsBuilt() ? m_BusGraph.GetBusValues() : m_PendingMixerValues);
  m_bMixerPendingRecovery = false;
  m_bRebuildPending = false;

  // voices first (they feed the buses), then the bus graph, which also moves the stream nodes to the endpoint.
  // Stopping the voices releases their disk streams; the stream service itself does not depend on the device.
  m_EventManager.StopAll(true);
  UnloadMixer().IgnoreResult();

  // stream nodes are recreated because the new device may have another channel count or sample rate
  struct PcmState
  {
    bool m_bValid = false;
    float m_fVolume = 1.0f;
    float m_fPitch = 1.0f;
    bool m_bPaused = false;
    plUInt64 m_uiFramesRendered = 0;
  };

  plHybridArray<PcmState, 4> pcmStates;
  pcmStates.SetCount(m_PcmStreams.GetCount());

  for (plUInt32 i = 0; i < m_PcmStreams.GetCount(); ++i)
  {
    PcmStream& stream = m_PcmStreams[i];
    if (stream.m_pNode == nullptr)
      continue;

    PcmState& state = pcmStates[i];
    state.m_bValid = true;
    state.m_fVolume = stream.m_pNode->m_fVolume.load(std::memory_order_relaxed);
    state.m_fPitch = stream.m_pNode->m_fPitch.load(std::memory_order_relaxed);
    state.m_bPaused = stream.m_pNode->m_bPaused.load(std::memory_order_relaxed);
    state.m_uiFramesRendered = stream.m_pNode->m_uiFramesRendered.load(std::memory_order_relaxed);

    plMiniAudioDsp::UninitPcmStreamNode(stream.m_pNode.Borrow());
    stream.m_pNode.Clear();
  }

  if (m_pBackend->ReinitDevice().Failed())
  {
    plLog::Error("PlasmaAudio: re-initializing the audio engine failed; retrying every few seconds.");
    m_EventManager.GetEventLog().Add(plAudioLogEventType::DeviceLost, "output device", "re-init failed, retrying");

    // the mixer is rebuilt from this desc once a retry succeeds
    if (bHadMixer)
    {
      m_PendingMixerDesc = mixerDesc;
      m_PendingMixerValues = mixerValues;
      m_bMixerPendingRecovery = true;
    }
    return;
  }

  m_EventManager.GetEventLog().Add(plAudioLogEventType::DeviceRecovered, "output device");

  // streams without a node (lost in a failed attempt) are recreated with the default settings
  for (plUInt32 i = 0; i < m_PcmStreams.GetCount(); ++i)
  {
    PcmStream& stream = m_PcmStreams[i];
    const PcmState& state = pcmStates[i];

    plUniquePtr<plMaPcmStreamNode> pNode = PL_DEFAULT_NEW(plMaPcmStreamNode);
    if (plMiniAudioDsp::InitPcmStreamNode(m_pBackend->GetEngine(), pNode.Borrow(), stream.m_uiChannels, stream.m_uiSampleRate, stream.m_uiRingFrames).Failed())
    {
      plLog::Error("PlasmaAudio: PCM stream {0} could not be recreated on the new device and stays silent.", stream.m_uiId);
      continue;
    }

    pNode->m_fVolume.store(state.m_fVolume, std::memory_order_relaxed);
    pNode->m_fPitch.store(state.m_fPitch, std::memory_order_relaxed);
    pNode->m_bPaused.store(state.m_bPaused, std::memory_order_relaxed);
    pNode->m_uiFramesRendered.store(state.m_uiFramesRendered, std::memory_order_relaxed);

    stream.m_pNode = std::move(pNode);
    AttachPcmStream(stream);
  }

  // rebuilding the bus graph moves the streams from the endpoint onto their buses
  if (bHadMixer && LoadMixer(mixerDesc).Succeeded())
  {
    m_BusGraph.ApplyBusValues(mixerValues);
  }
}

plResult plPlasmaAudioMiddleware::Release()
{
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::StopAllSounds()
{
  m_EventManager.StopAll(false);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::AddEntity(plAudioSystemEntityData* pEntityData, const char* szEntityName)
{
  PL_IGNORE_UNUSED(pEntityData);
  PL_IGNORE_UNUSED(szEntityName);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::ResetEntity(plAudioSystemEntityData* pEntityData)
{
  PL_IGNORE_UNUSED(pEntityData);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::UpdateEntity(plAudioSystemEntityData* pEntityData)
{
  PL_IGNORE_UNUSED(pEntityData);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::RemoveEntity(plAudioSystemEntityData* pEntityData)
{
  if (const auto* pEntity = plDynamicCast<const plPlasmaAudioEntityData*>(pEntityData))
  {
    // the entity data is going away — stop its instances immediately and sever links
    m_EventManager.StopAllOnEntity(pEntity, true);
  }
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::SetEntityTransform(plAudioSystemEntityData* pEntityData, const plAudioSystemTransform& Transform)
{
  auto* pEntity = plDynamicCast<plPlasmaAudioEntityData*>(pEntityData);
  if (pEntity == nullptr)
    return PL_FAILURE;

  pEntity->m_LastTransform = Transform;
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::LoadTrigger(plAudioSystemEntityData* pEntityData, const plAudioSystemTriggerData* pTriggerData, plAudioSystemEventData* pEventData)
{
  PL_IGNORE_UNUSED(pEntityData);
  PL_IGNORE_UNUSED(pTriggerData);
  PL_IGNORE_UNUSED(pEventData);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::ActivateTrigger(plAudioSystemEntityData* pEntityData, const plAudioSystemTriggerData* pTriggerData, plAudioSystemEventData* pEventData)
{
  if (m_pBackend == nullptr)
    return PL_FAILURE;

  ma_engine* pEngine = m_pBackend->GetEngine();
  if (pEngine == nullptr)
    return PL_FAILURE;

  auto* pEntity = plDynamicCast<plPlasmaAudioEntityData*>(pEntityData);
  auto* pEvent = plDynamicCast<plPlasmaAudioEventData*>(pEventData);

  // legacy trigger data, registered by plMiniAudioSoundResource on load: played as a generated audio event
  if (const auto* pTrigger = plDynamicCast<const plPlasmaAudioTriggerData*>(pTriggerData))
  {
    const plMiniAudioSoundResourceHandle hSound = plResourceManager::LoadResource<plMiniAudioSoundResource>(pTrigger->m_sResourceId);
    return m_EventManager.ActivateLegacySound(hSound, pEntity, pEvent, &m_BusGraph, pEngine);
  }

  if (const auto* pStringTrigger = plDynamicCast<const plAudioSystemStringTriggerData*>(pTriggerData))
  {
    const plStringView sName = pStringTrigger->m_sTriggerName;

    // a legacy sound resource known under this id is played as a generated audio event
    if (const plMiniAudioSoundResourceHandle hSound = plResourceManager::GetExistingResource<plMiniAudioSoundResource>(sName); hSound.IsValid())
    {
      return m_EventManager.ActivateLegacySound(hSound, pEntity, pEvent, &m_BusGraph, pEngine);
    }

    // audio event path (also covers events not yet loaded — the manager loads the resource)
    return m_EventManager.ActivateEvent(sName, pEntity, pEvent, &m_BusGraph, pEngine);
  }

  return PL_FAILURE;
}

plResult plPlasmaAudioMiddleware::UnloadTrigger(plAudioSystemEntityData* pEntityData, const plAudioSystemTriggerData* pTriggerData)
{
  PL_IGNORE_UNUSED(pEntityData);
  PL_IGNORE_UNUSED(pTriggerData);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::StopEvent(plAudioSystemEntityData* pEntityData, const plAudioSystemEventData* pEventData)
{
  PL_IGNORE_UNUSED(pEntityData);

  if (const auto* pEvent = plDynamicCast<const plPlasmaAudioEventData*>(pEventData))
  {
    m_EventManager.StopEvent(pEvent, false).IgnoreResult();
  }
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::StopAllEvents(plAudioSystemEntityData* pEntityData)
{
  if (const auto* pEntity = plDynamicCast<const plPlasmaAudioEntityData*>(pEntityData))
  {
    m_EventManager.StopAllOnEntity(pEntity, false);
  }
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::SetRtpc(plAudioSystemEntityData* pEntityData, const plAudioSystemRtpcData* pRtpcData, float fValue)
{
  const auto* pEntity = plDynamicCast<const plPlasmaAudioEntityData*>(pEntityData);
  const auto* pRtpc = plDynamicCast<const plAudioSystemStringRtpcData*>(pRtpcData);

  if (pEntity == nullptr || pRtpc == nullptr)
    return PL_FAILURE;

  // RTPCs map onto the audio event parameter of the same name
  m_EventManager.SetEventParameter(pEntity, pRtpc->m_sRtpcName, fValue);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::SetEventParameter(plAudioSystemEntityData* pEntityData, plStringView sParamName, float fValue)
{
  auto* pEntity = plDynamicCast<plPlasmaAudioEntityData*>(pEntityData);
  if (pEntity == nullptr)
    return PL_FAILURE;

  // reserved names set entity multipliers that every instance on the entity applies in its next update
  if (sParamName == plPlasmaAudioReservedParameter::Volume)
  {
    pEntity->m_fVolumeMultiplier = plMath::Max(fValue, 0.0f);
    return PL_SUCCESS;
  }

  if (sParamName == plPlasmaAudioReservedParameter::Pitch)
  {
    pEntity->m_fPitchMultiplier = plMath::Clamp(fValue, 0.01f, 10.0f);
    return PL_SUCCESS;
  }

  m_EventManager.SetEventParameter(pEntity, sParamName, fValue);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::ResetRtpc(plAudioSystemEntityData* pEntityData, const plAudioSystemRtpcData* pRtpcData)
{
  const auto* pEntity = plDynamicCast<const plPlasmaAudioEntityData*>(pEntityData);
  const auto* pRtpc = plDynamicCast<const plAudioSystemStringRtpcData*>(pRtpcData);

  if (pEntity == nullptr || pRtpc == nullptr)
    return PL_FAILURE;

  // the event parameter of the same name goes back to the global value, or to its default without one
  m_EventManager.ResetEventParameter(pEntity, pRtpc->m_sRtpcName);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::SetSwitchState(plAudioSystemEntityData* pEntityData, const plAudioSystemSwitchStateData* pSwitchStateData)
{
  auto* pEntity = plDynamicCast<plPlasmaAudioEntityData*>(pEntityData);
  const auto* pSwitchState = plDynamicCast<const plAudioSystemStringSwitchStateData*>(pSwitchStateData);

  if (pEntity == nullptr || pSwitchState == nullptr)
    return PL_FAILURE;

  // stored per group, so instruments filtered on this switch pick it up on their next trigger
  pEntity->SetSwitchState(pSwitchState->GetGroup(), pSwitchState->m_sSwitchStateName);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::SetObstructionAndOcclusion(plAudioSystemEntityData* pEntityData, float fObstruction, float fOcclusion)
{
  auto* pEntity = plDynamicCast<plPlasmaAudioEntityData*>(pEntityData);
  if (pEntity == nullptr)
    return PL_FAILURE;

  // stored on the entity; playing event instances smooth toward these targets each update
  // (per-voice low-pass for occlusion, direct-gain reduction for obstruction)
  pEntity->m_fTargetObstruction = plMath::Clamp(fObstruction, 0.0f, 1.0f);
  pEntity->m_fTargetOcclusion = plMath::Clamp(fOcclusion, 0.0f, 1.0f);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::SetEnvironmentAmount(plAudioSystemEntityData* pEntityData, const plAudioSystemEnvironmentData* pEnvironmentData, float fAmount)
{
  auto* pEntity = plDynamicCast<plPlasmaAudioEntityData*>(pEntityData);
  const auto* pEnvironment = plDynamicCast<const plAudioSystemStringEnvironmentData*>(pEnvironmentData);

  if (pEntity == nullptr || pEnvironment == nullptr)
    return PL_FAILURE;

  for (auto& env : pEntity->m_Environments)
  {
    if (env.m_sName == pEnvironment->m_sEnvironmentName)
    {
      env.m_fAmount = plMath::Clamp(fAmount, 0.0f, 1.0f);
      return PL_SUCCESS;
    }
  }

  auto& env = pEntity->m_Environments.ExpandAndGetRef();
  env.m_sName = pEnvironment->m_sEnvironmentName;
  env.m_fAmount = plMath::Clamp(fAmount, 0.0f, 1.0f);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::AddListener(plAudioSystemListenerData* pListenerData, const char* szListenerName, bool bIsDefault)
{
  auto* pListener = plDynamicCast<plPlasmaAudioListenerData*>(pListenerData);
  if (pListener == nullptr)
    return PL_FAILURE;

  // Default listeners share slot 0: the editor listener plAudioSystem registers in development builds
  // (which also receives every transform in editor override mode) and the game's default listener.
  // Only the one that is updated moves slot 0, so the game listener wins in a running game.
  if (bIsDefault)
  {
    pListener->m_uiListenerSlot = 0;
    return PL_SUCCESS;
  }

  // other listeners take the lowest free split-screen slot; it is heard once SetNumListeners covers it
  static_assert(PL_ARRAY_SIZE(m_bListenerSlotTaken) == plMiniAudioSingleton::MaxListeners);
  for (plUInt8 uiSlot = 1; uiSlot < plMiniAudioSingleton::MaxListeners; ++uiSlot)
  {
    if (!m_bListenerSlotTaken[uiSlot])
    {
      m_bListenerSlotTaken[uiSlot] = true;
      pListener->m_uiListenerSlot = uiSlot;
      return PL_SUCCESS;
    }
  }

  pListener->m_uiListenerSlot = 0xFF;
  plLog::Warning("PlasmaAudio: no free listener slot for listener '{0}', it is not heard.", szListenerName);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::ResetListener(plAudioSystemListenerData* pListenerData)
{
  PL_IGNORE_UNUSED(pListenerData);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::RemoveListener(plAudioSystemListenerData* pListenerData)
{
  auto* pListener = plDynamicCast<plPlasmaAudioListenerData*>(pListenerData);
  if (pListener == nullptr)
    return PL_FAILURE;

  const plUInt8 uiSlot = pListener->m_uiListenerSlot;
  pListener->m_uiListenerSlot = 0xFF;

  // slot 0 is shared and stays enabled
  if (uiSlot > 0 && uiSlot < plMiniAudioSingleton::MaxListeners)
  {
    m_bListenerSlotTaken[uiSlot] = false;

    if (m_pBackend != nullptr)
      m_pBackend->ReleaseListenerSlot(uiSlot);
  }
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::SetListenerTransform(plAudioSystemListenerData* pListenerData, const plAudioSystemTransform& Transform)
{
  const auto* pListener = plDynamicCast<const plPlasmaAudioListenerData*>(pListenerData);
  if (pListener == nullptr || m_pBackend == nullptr)
    return PL_FAILURE;

  // a listener without a slot is not heard
  if (pListener->m_uiListenerSlot == 0xFF)
    return PL_SUCCESS;

  m_pBackend->SetListener(pListener->m_uiListenerSlot, Transform.m_vPosition, Transform.m_vForward, Transform.m_vUp, Transform.m_vVelocity);
  return PL_SUCCESS;
}

namespace
{
  /// Bank id and resource id of the bank data types the ATL hands to the middleware.
  bool GetBankIdentity(const plAudioSystemBankData* pBankData, plAudioSystemDataID& out_uiBankId, plStringView& out_sBankName)
  {
    if (const auto* pStringBank = plDynamicCast<const plAudioSystemStringBankData*>(pBankData))
    {
      out_uiBankId = pStringBank->m_uiBankId;
      out_sBankName = pStringBank->m_sBankName;
      return true;
    }

    if (const auto* pPlasmaBank = plDynamicCast<const plPlasmaAudioBankData*>(pBankData))
    {
      out_uiBankId = pPlasmaBank->m_uiBankId;
      out_sBankName = pPlasmaBank->m_sBankName;
      return true;
    }

    return false;
  }
} // namespace

plResult plPlasmaAudioMiddleware::LoadBank(plAudioSystemBankData* pBankData)
{
  plAudioSystemDataID uiBankId = 0;
  plStringView sBankName;
  if (!GetBankIdentity(pBankData, uiBankId, sBankName) || sBankName.IsEmpty())
    return PL_FAILURE;

  if (m_Banks.Contains(uiBankId))
    return PL_SUCCESS;

  LoadedBank& bank = m_Banks[uiBankId];
  bank.m_hBank = plResourceManager::LoadResource<plAudioBankResource>(sBankName);
  plResourceManager::PreloadResource(bank.m_hBank);

  if (!RequestBankContent(uiBankId, bank))
  {
    m_PendingBanks.PushBack(uiBankId);
  }

  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::UnloadBank(plAudioSystemBankData* pBankData)
{
  plAudioSystemDataID uiBankId = 0;
  plStringView sBankName;
  if (!GetBankIdentity(pBankData, uiBankId, sBankName))
    return PL_FAILURE;

  LoadedBank* pBank = nullptr;
  if (m_Banks.TryGetValue(uiBankId, pBank))
  {
    m_EventManager.GetEventLog().Add(plAudioLogEventType::BankUnloaded, pBank->m_hBank.GetResourceID());
  }

  // releasing the handles lets the resources be freed once nothing else uses them; resources loaded from the bank
  // file stay loaded and are read from their own files when they load again
  m_Banks.Remove(uiBankId);
  m_PendingBanks.RemoveAndSwap(uiBankId);
  plAudioPackedContent::Unregister(uiBankId);
  return PL_SUCCESS;
}

bool plPlasmaAudioMiddleware::RequestBankContent(plAudioSystemDataID uiBankId, LoadedBank& ref_bank)
{
  if (ref_bank.m_bContentRequested)
    return true;

  plResourceLock<plAudioBankResource> pBank(ref_bank.m_hBank, plResourceAcquireMode::PointerOnly);
  const plResourceState state = pBank->GetLoadingState();

  if (state == plResourceState::LoadedResourceMissing)
  {
    plLog::Warning("PlasmaAudio: audio bank '{}' could not be loaded", pBank->GetResourceIdOrDescription());
    m_EventManager.GetEventLog().Add(plAudioLogEventType::BankLoaded, ref_bank.m_hBank.GetResourceID(), "missing");
    ref_bank.m_bContentRequested = true;
    return true;
  }

  if (state != plResourceState::Loaded)
    return false;

  const plAudioBankDesc& desc = pBank->GetDescriptor();

  // the loaders read what the bank packs from the bank file; the bank's budget is that of its platform
  plAudioPackedContent::Register(uiBankId, *pBank.GetPointer());

  if (desc.m_Budget.IsSet())
    m_MemoryBudget = desc.m_Budget;

  ref_bank.m_Events.Reserve(desc.m_Events.GetCount());
  for (const plString& sEvent : desc.m_Events)
  {
    if (sEvent.IsEmpty())
      continue;

    plAudioEventResourceHandle hEvent = plResourceManager::LoadResource<plAudioEventResource>(sEvent);
    plResourceManager::PreloadResource(hEvent);
    ref_bank.m_Events.PushBack(hEvent);
  }

  ref_bank.m_Clips.Reserve(desc.m_Clips.GetCount());
  for (const plString& sClip : desc.m_Clips)
  {
    if (sClip.IsEmpty())
      continue;

    plAudioClipResourceHandle hClip = plResourceManager::LoadResource<plAudioClipResource>(sClip);
    plResourceManager::PreloadResource(hClip);
    ref_bank.m_Clips.PushBack(hClip);
  }

  {
    plStringBuilder sDetail;
    sDetail.SetFormat("{} events, {} clips", ref_bank.m_Events.GetCount(), ref_bank.m_Clips.GetCount());
    if (!desc.m_Packed.IsEmpty())
      sDetail.AppendFormat(", {} packed", desc.m_Packed.GetCount());

    m_EventManager.GetEventLog().Add(plAudioLogEventType::BankLoaded, ref_bank.m_hBank.GetResourceID(), sDetail);
  }

  ref_bank.m_bContentRequested = true;
  return true;
}

void plPlasmaAudioMiddleware::UpdatePendingBanks()
{
  for (plUInt32 i = m_PendingBanks.GetCount(); i > 0; --i)
  {
    const plAudioSystemDataID uiBankId = m_PendingBanks[i - 1];

    LoadedBank* pBank = nullptr;
    if (!m_Banks.TryGetValue(uiBankId, pBank) || RequestBankContent(uiBankId, *pBank))
    {
      m_PendingBanks.RemoveAtAndSwap(i - 1);
    }
  }
}

void plPlasmaAudioMiddleware::UpdateBankReadiness()
{
  for (auto it = m_Banks.GetIterator(); it.IsValid(); ++it)
  {
    LoadedBank& bank = it.Value();
    if (bank.m_bReady || !bank.m_bContentRequested || !IsBankContentLoaded(bank))
      continue;

    bank.m_bReady = true;
    CheckMemoryBudget(bank);
  }

  // the editor's bank window, ten times a second
  if (!m_Audition.IsBankMonitorWatched())
    return;

  const plTime tNow = plTime::Now();
  if (tNow - m_LastBankMonitorRefresh < plTime::MakeFromMilliseconds(100))
    return;

  m_LastBankMonitorRefresh = tNow;

  plAudioBankMonitorStatus status;
  status.m_Usage = GetMemoryUsage();
  status.m_Budget = m_MemoryBudget;

  for (auto it = m_Banks.GetIterator(); it.IsValid(); ++it)
  {
    const LoadedBank& bank = it.Value();

    plAudioBankMonitorEntry& entry = status.m_Banks.ExpandAndGetRef();
    entry.m_sBank = bank.m_hBank.GetResourceID();
    entry.m_uiItems = bank.m_Events.GetCount() + bank.m_Clips.GetCount();
    entry.m_uiReadyItems = bank.m_bReady ? entry.m_uiItems : CountReadyItems(bank);
    entry.m_bOverBudget = bank.m_bOverBudget;
    entry.m_uiState = plAudioBankMemorySnapshotEntry::Loading;

    if (bank.m_bContentRequested)
    {
      plResourceLock<plAudioBankResource> pBank(bank.m_hBank, plResourceAcquireMode::PointerOnly);
      entry.m_uiState = pBank->GetLoadingState() == plResourceState::LoadedResourceMissing ? plAudioBankMemorySnapshotEntry::Missing : plAudioBankMemorySnapshotEntry::Loaded;
    }
  }

  m_Audition.SetBankMonitorStatus(std::move(status));
}

void plPlasmaAudioMiddleware::CheckMemoryBudget(LoadedBank& ref_bank)
{
  if (!m_MemoryBudget.IsSet())
    return;

  plStringBuilder sExcess;
  GetMemoryUsage().DescribeExcess(m_MemoryBudget, sExcess);
  if (sExcess.IsEmpty())
    return;

  ref_bank.m_bOverBudget = true;
  plLog::Warning("PlasmaAudio: with the audio bank '{}' loaded, the audio content is over its memory budget: {}", ref_bank.m_hBank.GetResourceID(), sExcess);
  m_EventManager.GetEventLog().Add(plAudioLogEventType::BudgetExceeded, ref_bank.m_hBank.GetResourceID(), sExcess);
}

plAudioMemoryUsage plPlasmaAudioMiddleware::GetMemoryUsage() const
{
  plAudioMemoryUsage usage;
  plAudioClipResource::GatherMemoryUsage(usage);
  usage.m_uiRingBytes = m_StreamService.GetRingBytesInUse();
  return usage;
}

bool plPlasmaAudioMiddleware::IsBankOverBudget(plAudioSystemDataID uiBankId) const
{
  const LoadedBank* pBank = nullptr;
  return m_Banks.TryGetValue(uiBankId, pBank) && pBank->m_bOverBudget;
}

bool plPlasmaAudioMiddleware::IsBankContentRequested(plAudioSystemDataID uiBankId) const
{
  const LoadedBank* pBank = nullptr;
  return m_Banks.TryGetValue(uiBankId, pBank) && pBank->m_bContentRequested;
}

bool plPlasmaAudioMiddleware::IsBankReady(plAudioSystemDataID uiBankId) const
{
  const LoadedBank* pBank = nullptr;
  return m_Banks.TryGetValue(uiBankId, pBank) && pBank->m_bContentRequested && IsBankContentLoaded(*pBank);
}

namespace
{
  /// A missing resource counts as finished: it will not become playable by waiting.
  bool IsFinished(const plResource* pResource)
  {
    const plResourceState state = pResource->GetLoadingState();
    return state == plResourceState::Loaded || state == plResourceState::LoadedResourceMissing;
  }
} // namespace

bool plPlasmaAudioMiddleware::IsBankContentLoaded(const LoadedBank& bank)
{
  for (const plAudioEventResourceHandle& hEvent : bank.m_Events)
  {
    plResourceLock<plAudioEventResource> pEvent(hEvent, plResourceAcquireMode::PointerOnly);
    if (!IsFinished(pEvent.GetPointer()))
      return false;
  }

  for (const plAudioClipResourceHandle& hClip : bank.m_Clips)
  {
    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::PointerOnly);
    if (!IsFinished(pClip.GetPointer()))
      return false;
  }

  return true;
}

plUInt32 plPlasmaAudioMiddleware::CountReadyItems(const LoadedBank& bank)
{
  plUInt32 uiReady = 0;

  for (const plAudioEventResourceHandle& hEvent : bank.m_Events)
  {
    plResourceLock<plAudioEventResource> pEvent(hEvent, plResourceAcquireMode::PointerOnly);
    uiReady += IsFinished(pEvent.GetPointer()) ? 1 : 0;
  }

  for (const plAudioClipResourceHandle& hClip : bank.m_Clips)
  {
    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::PointerOnly);
    uiReady += IsFinished(pClip.GetPointer()) ? 1 : 0;
  }

  return uiReady;
}

plAudioSystemEntityData* plPlasmaAudioMiddleware::CreateWorldEntity(plAudioSystemDataID uiEntityId)
{
  return PL_AUDIOSYSTEM_NEW(plPlasmaAudioEntityData, uiEntityId, true);
}

plAudioSystemEntityData* plPlasmaAudioMiddleware::CreateEntityData(plAudioSystemDataID uiEntityId)
{
  return PL_AUDIOSYSTEM_NEW(plPlasmaAudioEntityData, uiEntityId, false);
}

plResult plPlasmaAudioMiddleware::DestroyEntityData(plAudioSystemEntityData* pEntityData)
{
  // instances still on the entity (the ATL does not always remove it first) must not reference it once it is deleted
  if (const auto* pEntity = plDynamicCast<const plPlasmaAudioEntityData*>(pEntityData))
  {
    m_EventManager.StopAllOnEntity(pEntity, true);
  }

  PL_AUDIOSYSTEM_DELETE(pEntityData);
  return PL_SUCCESS;
}

plAudioSystemListenerData* plPlasmaAudioMiddleware::CreateListenerData(plAudioSystemDataID uiListenerId)
{
  return PL_AUDIOSYSTEM_NEW(plPlasmaAudioListenerData, uiListenerId);
}

plResult plPlasmaAudioMiddleware::DestroyListenerData(plAudioSystemListenerData* pListenerData)
{
  PL_AUDIOSYSTEM_DELETE(pListenerData);
  return PL_SUCCESS;
}

plAudioSystemEventData* plPlasmaAudioMiddleware::CreateEventData(plAudioSystemDataID uiEventId)
{
  return PL_AUDIOSYSTEM_NEW(plPlasmaAudioEventData, uiEventId);
}

plResult plPlasmaAudioMiddleware::ResetEventData(plAudioSystemEventData* pEventData)
{
  // the event data may be reused for another activation: instances still bound to it are let go
  if (auto* pEvent = plDynamicCast<plPlasmaAudioEventData*>(pEventData))
  {
    m_EventManager.DetachEventData(pEvent);
  }
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::DestroyEventData(plAudioSystemEventData* pEventData)
{
  // instances still playing (e.g. fading out after a stop) must not reference the data once it is deleted
  if (auto* pEvent = plDynamicCast<plPlasmaAudioEventData*>(pEventData))
  {
    m_EventManager.DetachEventData(pEvent);
  }

  PL_AUDIOSYSTEM_DELETE(pEventData);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::DestroyBank(plAudioSystemBankData* pBankData)
{
  // the ATL destroys banks at shutdown without unloading them first
  UnloadBank(pBankData).IgnoreResult();

  PL_AUDIOSYSTEM_DELETE(pBankData);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::DestroyTriggerData(plAudioSystemTriggerData* pTriggerData)
{
  PL_AUDIOSYSTEM_DELETE(pTriggerData);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::DestroyRtpcData(plAudioSystemRtpcData* pRtpcData)
{
  PL_AUDIOSYSTEM_DELETE(pRtpcData);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::DestroySwitchStateData(plAudioSystemSwitchStateData* pSwitchStateData)
{
  PL_AUDIOSYSTEM_DELETE(pSwitchStateData);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::DestroyEnvironmentData(plAudioSystemEnvironmentData* pEnvironmentData)
{
  PL_AUDIOSYSTEM_DELETE(pEnvironmentData);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::SetLanguage(const char* szLanguage)
{
  PL_LOCK(m_LanguageMutex);
  m_sPendingLanguage = szLanguage != nullptr ? szLanguage : "";
  m_bLanguagePending = true;
  return PL_SUCCESS;
}

plString plPlasmaAudioMiddleware::GetLanguage() const
{
  return plAudioClipResourceLoader::GetLanguage();
}

void plPlasmaAudioMiddleware::OnLanguageCVarEvent(const plCVarEvent& e)
{
  if (e.m_EventType != plCVarEvent::ValueChanged)
    return;

  SetLanguage(static_cast<plCVarString*>(e.m_pCVar)->GetValue().GetData()).IgnoreResult();
}

void plPlasmaAudioMiddleware::ApplyPendingLanguage()
{
  plString sLanguage;
  {
    PL_LOCK(m_LanguageMutex);
    if (!m_bLanguagePending)
      return;

    m_bLanguagePending = false;
    sLanguage = m_sPendingLanguage;
  }

  if (plAudioClipResourceLoader::GetLanguage() == sLanguage)
    return;

  {
    const plString sPrevious = plAudioClipResourceLoader::GetLanguage();
    plStringBuilder sDetail;
    sDetail.SetFormat("from {}", sPrevious.IsEmpty() ? plStringView("<default>") : sPrevious.GetView());
    m_EventManager.GetEventLog().Add(plAudioLogEventType::LanguageChanged, sLanguage.IsEmpty() ? plStringView("<default>") : sLanguage.GetView(), sDetail);
  }

  plAudioClipResourceLoader::SetLanguage(sLanguage);

  // Only clips with more than one language are reloaded; everything else keeps playing. Reloading a clip stops the
  // voices that read it, and looping instruments start again once it is loaded in the new language. Unloaded clips pick
  // the language up whenever they are loaded. A clip whose load is in flight finishes in the old language; a voice
  // starting it reloads it first (plAudioClipResource::GetLanguageGeneration()).
  plDynamicArray<plString> localizedClips;
  plAudioClipResource::GatherLoadedLocalizedClips(localizedClips);

  m_LanguageReloads.Clear();
  m_LanguageReloadDeadline = plTime::Now() + plTime::MakeFromSeconds(5);
  for (const plString& sClip : localizedClips)
  {
    const plAudioClipResourceHandle hClip = plResourceManager::GetExistingResource<plAudioClipResource>(sClip);
    if (hClip.IsValid())
    {
      m_LanguageReloads.PushBack(hClip);
    }
  }

  RetryLanguageReloads();

  // bank clips stay resident: queue every bank clip that is not loaded right now, including the ones just reloaded
  for (auto it = m_Banks.GetIterator(); it.IsValid(); ++it)
  {
    for (const plAudioClipResourceHandle& hClip : it.Value().m_Clips)
    {
      plResourceManager::PreloadResource(hClip);
    }
  }
}

void plPlasmaAudioMiddleware::RetryLanguageReloads()
{
  // The resource manager skips a reload while a load task still finishes the clip (e.g. right after a blocking load
  // returned), so a clip that is still in the old language is tried again on the next updates, for a few seconds. A
  // clip that is still stale then is reloaded by the next voice that plays it (plAudioClipResource::GetLanguageGeneration()).
  if (plTime::Now() > m_LanguageReloadDeadline)
  {
    m_LanguageReloads.Clear();
    return;
  }

  for (plUInt32 i = m_LanguageReloads.GetCount(); i > 0; --i)
  {
    const plAudioClipResourceHandle& hClip = m_LanguageReloads[i - 1];

    plAudioClipResource* pClip = plResourceManager::BeginAcquireResource(hClip, plResourceAcquireMode::PointerOnly);
    const bool bLoaded = pClip->GetLoadingState() == plResourceState::Loaded;
    const bool bStale = bLoaded && pClip->GetLanguageGeneration() != plAudioClipResourceLoader::GetLanguageGeneration();
    plResourceManager::EndAcquireResource(pClip);

    // unloaded clips load in the current language whenever they are used; up-to-date ones are done
    if (bLoaded && !bStale)
    {
      m_LanguageReloads.RemoveAtAndSwap(i - 1);
      continue;
    }

    if (!bLoaded)
      continue;

    if (plResourceManager::ReloadResource(hClip, true))
    {
      plResourceManager::PreloadResource(hClip);
      m_LanguageReloads.RemoveAtAndSwap(i - 1);
    }
  }
}

const char* plPlasmaAudioMiddleware::GetMiddlewareName() const
{
  return "PlasmaAudio";
}

float plPlasmaAudioMiddleware::GetMasterGain() const
{
  return m_pBackend != nullptr ? m_pBackend->GetMasterChannelVolume() : 1.0f;
}

bool plPlasmaAudioMiddleware::GetMute() const
{
  return m_pBackend != nullptr ? m_pBackend->GetMasterChannelMute() : false;
}

void plPlasmaAudioMiddleware::OnMasterGainChange(float fGain)
{
  if (m_pBackend != nullptr)
    m_pBackend->SetMasterChannelVolume(fGain);
}

void plPlasmaAudioMiddleware::OnMuteChange(bool bMute)
{
  if (m_pBackend != nullptr)
    m_pBackend->SetMasterChannelMute(bMute);
}

void plPlasmaAudioMiddleware::OnMasterPauseChange(bool bPaused)
{
  if (m_pBackend != nullptr)
    m_pBackend->SetMasterChannelPaused(bPaused);
}

bool plPlasmaAudioMiddleware::GetMasterPaused() const
{
  return m_pBackend != nullptr ? m_pBackend->GetMasterChannelPaused() : false;
}

void plPlasmaAudioMiddleware::SetSoundGroupVolume(plStringView sGroupGuid, float fVolume)
{
  // the ATL maps the name onto a bus or VCA of the loaded mixer first and only calls this for the other names
  if (m_pBackend != nullptr)
    m_pBackend->SetSoundGroupVolume(sGroupGuid, fVolume);
}

float plPlasmaAudioMiddleware::GetSoundGroupVolume(plStringView sGroupGuid) const
{
  return m_pBackend != nullptr ? m_pBackend->GetSoundGroupVolume(sGroupGuid) : 1.0f;
}

void plPlasmaAudioMiddleware::SetNumListeners(plUInt8 uiNumListeners)
{
  if (m_pBackend != nullptr)
    m_pBackend->SetNumListeners(uiNumListeners);
}

plUInt8 plPlasmaAudioMiddleware::GetNumListeners() const
{
  return m_pBackend != nullptr ? m_pBackend->GetNumListeners() : 1;
}

void plPlasmaAudioMiddleware::SetGlobalParameter(plStringView sName, float fValue)
{
  const plUInt64 uiHash = plHashingUtils::StringHash(sName);

  PL_LOCK(m_GlobalParameterMutex);
  m_GlobalParameters[uiHash] = fValue;
  m_bGlobalParametersDirty = true;
}

float plPlasmaAudioMiddleware::GetGlobalParameter(plStringView sName) const
{
  const plUInt64 uiHash = plHashingUtils::StringHash(sName);

  PL_LOCK(m_GlobalParameterMutex);
  float fValue = 0.0f;
  m_GlobalParameters.TryGetValue(uiHash, fValue);
  return fValue;
}

void plPlasmaAudioMiddleware::OnLoseFocus()
{
  OnMasterPauseChange(true);
}

void plPlasmaAudioMiddleware::OnGainFocus()
{
  OnMasterPauseChange(false);
}

bool plPlasmaAudioMiddleware::IsEventDataPlaying(const plAudioSystemEventData* pEventData) const
{
  if (const auto* pEvent = plDynamicCast<const plPlasmaAudioEventData*>(pEventData))
  {
    return m_EventManager.IsEventPlaying(pEvent);
  }
  return false;
}

plResult plPlasmaAudioMiddleware::LoadMixer(const plAudioMixerDesc& desc)
{
  if (m_pBackend == nullptr)
    return PL_FAILURE;

  UnloadMixer().IgnoreResult();

  {
    // CreatePcmStream (video decoder thread) attaches to buses of the graph, so it must not see it half built
    PL_LOCK(m_PcmStreamMutex);

    ma_engine* pEngine = m_pBackend->GetEngine();
    if (pEngine == nullptr)
      return PL_FAILURE;

    PL_SUCCEED_OR_RETURN(m_BusGraph.Build(desc, pEngine));

    // streams created before the mixer existed sit on the endpoint; move them onto their buses
    ReattachPcmStreams();
  }

  RegisterMixerEnvironments(desc);

  // the mixer service pushes the authored base state right after this returns; it also logs the load with the
  // resource id, which the middleware does not get
  {
    plStringBuilder sDetail;
    sDetail.SetFormat("{} buses, {} VCAs, {} snapshots", desc.m_Buses.GetCount(), desc.m_Vcas.GetCount(), desc.m_Snapshots.GetCount());
    m_EventManager.GetEventLog().Add(plAudioLogEventType::MixerLoaded, "mixer", sDetail);
  }

  return PL_SUCCESS;
}

void plPlasmaAudioMiddleware::RegisterMixerEnvironments(const plAudioMixerDesc& desc)
{
  // register the mixer's environment aux buses as ATL environments so the generic
  // Box/Sphere environment components resolve their names
  auto* pAudioSystem = plAudioSystem::GetSingleton();
  if (pAudioSystem == nullptr)
    return;

  for (const auto& busDesc : desc.m_Buses)
  {
    if (!busDesc.m_bIsAux || busDesc.m_sEnvironmentName.IsEmpty())
      continue;

    const plAudioSystemDataID uiEnvironmentId = plHashHelper<plStringView>::Hash(busDesc.m_sEnvironmentName.GetView());
    plAudioSystemEnvironmentData* pData = PL_AUDIOSYSTEM_NEW(plAudioSystemStringEnvironmentData, uiEnvironmentId, busDesc.m_sEnvironmentName);

    // an environment someone else registered stays theirs: it is neither tracked nor unregistered by this mixer
    if (pAudioSystem->TryRegisterEnvironment(uiEnvironmentId, pData))
    {
      m_RegisteredEnvironmentIds.PushBack(uiEnvironmentId);
    }
    else
    {
      PL_AUDIOSYSTEM_DELETE(pData);
    }
  }
}

void plPlasmaAudioMiddleware::UnregisterMixerEnvironments()
{
  if (auto* pAudioSystem = plAudioSystem::GetSingleton())
  {
    for (const plAudioSystemDataID uiEnvironmentId : m_RegisteredEnvironmentIds)
    {
      pAudioSystem->UnregisterEnvironment(uiEnvironmentId);
    }
  }

  m_RegisteredEnvironmentIds.Clear();
}

plResult plPlasmaAudioMiddleware::UpdateMixer(const plAudioMixerDesc& desc, bool bLayoutChanged)
{
  if (!m_BusGraph.IsBuilt())
    return LoadMixer(desc);

  if (!bLayoutChanged && !m_bRebuildPending)
    return PL_SUCCESS;

  ma_engine* pEngine = m_pBackend != nullptr ? m_pBackend->GetEngine() : nullptr;
  if (pEngine == nullptr)
    return PL_FAILURE;

  // the values the service sends next refer to the new desc, so they wait with it for the rebuild
  m_RebuildDesc = desc;
  plMiniAudioBusGraph::GetAuthoredValues(desc, m_RebuildValues);

  if (!m_bRebuildPending)
  {
    m_bRebuildPending = true;

    // the output fades to silence first, so the rebuild does not click
    ma_sound_group_set_fade_in_milliseconds(m_BusGraph.GetBusInput(0), -1.0f, 0.0f, static_cast<ma_uint64>(MixerRebuildFade.GetMilliseconds()));

    const ma_uint64 uiFadeFrames = static_cast<ma_uint64>(MixerRebuildFade.GetSeconds() * ma_engine_get_sample_rate(pEngine));
    m_uiRebuildFrame = ma_engine_get_time_in_pcm_frames(pEngine) + uiFadeFrames;
  }

  return PL_SUCCESS;
}

void plPlasmaAudioMiddleware::FinishMixerRebuild()
{
  m_bRebuildPending = false;

  ma_engine* pEngine = m_pBackend != nullptr ? m_pBackend->GetEngine() : nullptr;
  if (pEngine == nullptr)
    return;

  // the voices keep playing, detached, while the graph is rebuilt underneath them
  m_EventManager.DetachFromBusGraph(&m_BusGraph);
  UnregisterMixerEnvironments();

  {
    PL_LOCK(m_PcmStreamMutex);

    DetachPcmStreamsToEndpoint();

    if (m_BusGraph.Build(m_RebuildDesc, pEngine).Failed())
    {
      plLog::Error("PlasmaAudio: the changed mixer could not be built; the output plays without a mixer.");
      m_EventManager.ReattachToBusGraph(&m_BusGraph);
      return;
    }

    // the new master starts silent and fades in once everything is attached again
    ma_sound_group_set_fade_in_milliseconds(m_BusGraph.GetBusInput(0), 0.0f, 1.0f, static_cast<ma_uint64>(MixerRebuildFade.GetMilliseconds()));

    m_BusGraph.ApplyBusValues(m_RebuildValues);
    ReattachPcmStreams();
  }

  m_EventManager.ReattachToBusGraph(&m_BusGraph);
  RegisterMixerEnvironments(m_RebuildDesc);

  plStringBuilder sDetail;
  sDetail.SetFormat("{} buses, rebuilt in place", m_RebuildDesc.m_Buses.GetCount());
  m_EventManager.GetEventLog().Add(plAudioLogEventType::MixerLoaded, "mixer", sDetail);
}

plMiniAudioBusGraph::BusValues* plPlasmaAudioMiddleware::GetPendingRebuildValues(plUInt32 uiBusIndex)
{
  if (!m_bRebuildPending || uiBusIndex >= m_RebuildValues.GetCount())
    return nullptr;

  return &m_RebuildValues[uiBusIndex];
}

plResult plPlasmaAudioMiddleware::UnloadMixer()
{
  m_bRebuildPending = false;

  if (!m_BusGraph.IsBuilt())
    return PL_SUCCESS;

  // voices attached to bus groups must not outlive the graph
  m_EventManager.StopAll(true);

  UnregisterMixerEnvironments();

  {
    // held across both steps, so CreatePcmStream cannot attach a new stream to a bus that is being torn down
    PL_LOCK(m_PcmStreamMutex);

    // stream nodes must not point at bus groups that are about to go away
    DetachPcmStreamsToEndpoint();

    m_BusGraph.Teardown();
  }
  return PL_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// PCM streams
// ---------------------------------------------------------------------------------------------

plPlasmaAudioMiddleware::PcmStream* plPlasmaAudioMiddleware::FindPcmStream(plUInt32 uiStreamId)
{
  for (PcmStream& stream : m_PcmStreams)
  {
    if (stream.m_uiId == uiStreamId)
      return &stream;
  }
  return nullptr;
}

const plPlasmaAudioMiddleware::PcmStream* plPlasmaAudioMiddleware::FindPcmStream(plUInt32 uiStreamId) const
{
  for (const PcmStream& stream : m_PcmStreams)
  {
    if (stream.m_uiId == uiStreamId)
      return &stream;
  }
  return nullptr;
}

void plPlasmaAudioMiddleware::AttachPcmStream(PcmStream& ref_stream)
{
  if (m_pBackend == nullptr || ref_stream.m_pNode == nullptr)
    return;

  ma_engine* pEngine = m_pBackend->GetEngine();
  if (pEngine == nullptr)
    return;

  ma_node* pTarget = nullptr;
  if (m_BusGraph.IsBuilt())
  {
    pTarget = reinterpret_cast<ma_node*>(m_BusGraph.GetBusInput(m_BusGraph.ResolveBus(ref_stream.m_sBus)));
  }
  if (pTarget == nullptr)
  {
    pTarget = ma_node_graph_get_endpoint(ma_engine_get_node_graph(pEngine));
  }

  ma_node_attach_output_bus(&ref_stream.m_pNode->m_Base, 0, pTarget, 0);
}

void plPlasmaAudioMiddleware::ReattachPcmStreams()
{
  PL_LOCK(m_PcmStreamMutex);
  for (PcmStream& stream : m_PcmStreams)
  {
    AttachPcmStream(stream);
  }
}

void plPlasmaAudioMiddleware::DetachPcmStreamsToEndpoint()
{
  if (m_pBackend == nullptr)
    return;

  PL_LOCK(m_PcmStreamMutex);

  ma_engine* pEngine = m_pBackend->GetEngine();
  if (pEngine == nullptr)
    return;

  ma_node* pEndpoint = ma_node_graph_get_endpoint(ma_engine_get_node_graph(pEngine));
  for (PcmStream& stream : m_PcmStreams)
  {
    if (stream.m_pNode != nullptr)
    {
      ma_node_attach_output_bus(&stream.m_pNode->m_Base, 0, pEndpoint, 0);
    }
  }
}

void plPlasmaAudioMiddleware::DestroyAllPcmStreams()
{
  PL_LOCK(m_PcmStreamMutex);
  for (PcmStream& stream : m_PcmStreams)
  {
    if (stream.m_pNode != nullptr)
    {
      plMiniAudioDsp::UninitPcmStreamNode(stream.m_pNode.Borrow());
    }
  }
  m_PcmStreams.Clear();
}

plResult plPlasmaAudioMiddleware::CreatePcmStream(const plAudioPcmStreamDesc& desc, plUInt32& out_uiStreamId)
{
  out_uiStreamId = 0;
  if (m_pBackend == nullptr)
    return PL_FAILURE;

  // locked before the node is created, so it cannot land on an engine that RecoverDevice() is replacing
  PL_LOCK(m_PcmStreamMutex);

  ma_engine* pEngine = m_pBackend->GetEngine();
  if (pEngine == nullptr)
    return PL_FAILURE;

  plUniquePtr<plMaPcmStreamNode> pNode = PL_DEFAULT_NEW(plMaPcmStreamNode);
  if (plMiniAudioDsp::InitPcmStreamNode(pEngine, pNode.Borrow(), desc.m_uiChannels, desc.m_uiSampleRate, desc.m_uiRingFrames).Failed())
    return PL_FAILURE;

  PcmStream& stream = m_PcmStreams.ExpandAndGetRef();
  stream.m_uiId = m_uiNextPcmStreamId++;
  stream.m_sBus = desc.m_sOutputBus;
  stream.m_uiChannels = desc.m_uiChannels;
  stream.m_uiSampleRate = desc.m_uiSampleRate;
  stream.m_uiRingFrames = desc.m_uiRingFrames;
  stream.m_pNode = std::move(pNode);
  AttachPcmStream(stream);

  out_uiStreamId = stream.m_uiId;
  return PL_SUCCESS;
}

void plPlasmaAudioMiddleware::DestroyPcmStream(plUInt32 uiStreamId)
{
  PL_LOCK(m_PcmStreamMutex);

  for (plUInt32 i = 0; i < m_PcmStreams.GetCount(); ++i)
  {
    if (m_PcmStreams[i].m_uiId != uiStreamId)
      continue;

    if (m_PcmStreams[i].m_pNode != nullptr)
    {
      plMiniAudioDsp::UninitPcmStreamNode(m_PcmStreams[i].m_pNode.Borrow());
    }
    m_PcmStreams.RemoveAtAndCopy(i);
    return;
  }
}

plUInt32 plPlasmaAudioMiddleware::WritePcmFrames(plUInt32 uiStreamId, plArrayPtr<const float> interleaved)
{
  PL_LOCK(m_PcmStreamMutex);
  PcmStream* pStream = FindPcmStream(uiStreamId);
  if (pStream == nullptr || pStream->m_pNode == nullptr)
    return 0;

  const plUInt32 uiFrames = interleaved.GetCount() / plMath::Max(1u, pStream->m_pNode->m_uiSourceChannels);
  return plMiniAudioDsp::WritePcmStreamFrames(pStream->m_pNode.Borrow(), interleaved.GetPtr(), uiFrames);
}

plUInt32 plPlasmaAudioMiddleware::GetPcmStreamFreeFrames(plUInt32 uiStreamId) const
{
  PL_LOCK(m_PcmStreamMutex);
  const PcmStream* pStream = FindPcmStream(uiStreamId);
  if (pStream == nullptr || pStream->m_pNode == nullptr)
    return 0;
  return plMiniAudioDsp::GetPcmStreamFreeFrames(pStream->m_pNode.Borrow());
}

plUInt64 plPlasmaAudioMiddleware::GetPcmStreamFramesRendered(plUInt32 uiStreamId) const
{
  PL_LOCK(m_PcmStreamMutex);
  const PcmStream* pStream = FindPcmStream(uiStreamId);
  if (pStream == nullptr || pStream->m_pNode == nullptr)
    return 0;
  return pStream->m_pNode->m_uiFramesRendered.load(std::memory_order_relaxed);
}

void plPlasmaAudioMiddleware::SetPcmStreamVolume(plUInt32 uiStreamId, float fLinearGain)
{
  PL_LOCK(m_PcmStreamMutex);
  if (PcmStream* pStream = FindPcmStream(uiStreamId); pStream != nullptr && pStream->m_pNode != nullptr)
  {
    pStream->m_pNode->m_fVolume.store(plMath::Max(0.0f, fLinearGain), std::memory_order_relaxed);
  }
}

void plPlasmaAudioMiddleware::SetPcmStreamPitch(plUInt32 uiStreamId, float fPitch)
{
  PL_LOCK(m_PcmStreamMutex);
  if (PcmStream* pStream = FindPcmStream(uiStreamId); pStream != nullptr && pStream->m_pNode != nullptr)
  {
    pStream->m_pNode->m_fPitch.store(plMath::Clamp(fPitch, 0.05f, 8.0f), std::memory_order_relaxed);
  }
}

void plPlasmaAudioMiddleware::SetPcmStreamPaused(plUInt32 uiStreamId, bool bPaused)
{
  PL_LOCK(m_PcmStreamMutex);
  if (PcmStream* pStream = FindPcmStream(uiStreamId); pStream != nullptr && pStream->m_pNode != nullptr)
  {
    pStream->m_pNode->m_bPaused.store(bPaused, std::memory_order_relaxed);
  }
}

void plPlasmaAudioMiddleware::FlushPcmStream(plUInt32 uiStreamId)
{
  PL_LOCK(m_PcmStreamMutex);
  if (PcmStream* pStream = FindPcmStream(uiStreamId); pStream != nullptr && pStream->m_pNode != nullptr)
  {
    pStream->m_pNode->m_uiFlushRequested.fetch_add(1, std::memory_order_release);
  }
}

plResult plPlasmaAudioMiddleware::SetBusVolume(plUInt32 uiBusIndex, float fLinearGain)
{
  if (plMiniAudioBusGraph::BusValues* pPending = GetPendingRebuildValues(uiBusIndex))
  {
    pPending->m_fGain = fLinearGain;
    return PL_SUCCESS;
  }

  if (!m_BusGraph.IsBuilt())
    return PL_FAILURE;

  m_BusGraph.SetBusVolume(uiBusIndex, fLinearGain);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::SetBusPitch(plUInt32 uiBusIndex, float fPitch)
{
  if (plMiniAudioBusGraph::BusValues* pPending = GetPendingRebuildValues(uiBusIndex))
  {
    pPending->m_fPitch = fPitch;
    return PL_SUCCESS;
  }

  if (!m_BusGraph.IsBuilt())
    return PL_FAILURE;

  m_BusGraph.SetBusPitch(uiBusIndex, fPitch);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::SetBusSendLevel(plUInt32 uiBusIndex, plUInt32 uiSendIndex, float fLinearLevel)
{
  if (plMiniAudioBusGraph::BusValues* pPending = GetPendingRebuildValues(uiBusIndex))
  {
    if (uiSendIndex < pPending->m_SendLevels.GetCount())
      pPending->m_SendLevels[uiSendIndex] = fLinearLevel;
    return PL_SUCCESS;
  }

  if (!m_BusGraph.IsBuilt())
    return PL_FAILURE;

  m_BusGraph.SetBusSendLevel(uiBusIndex, uiSendIndex, fLinearLevel);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::SetBusEffectParameter(plUInt32 uiBusIndex, plUInt32 uiEffectIndex, plUInt32 uiParamIndex, float fValue)
{
  if (plMiniAudioBusGraph::BusValues* pPending = GetPendingRebuildValues(uiBusIndex))
  {
    const plUInt32 uiSlot = uiEffectIndex * plAudioEffectMaxParams + uiParamIndex;
    if (uiParamIndex < plAudioEffectMaxParams && uiSlot < pPending->m_EffectParams.GetCount())
      pPending->m_EffectParams[uiSlot] = fValue;
    return PL_SUCCESS;
  }

  if (!m_BusGraph.IsBuilt())
    return PL_FAILURE;

  m_BusGraph.SetBusEffectParameter(uiBusIndex, uiEffectIndex, uiParamIndex, fValue);
  return PL_SUCCESS;
}

plResult plPlasmaAudioMiddleware::SetBusEffectBypass(plUInt32 uiBusIndex, plUInt32 uiEffectIndex, bool bBypass)
{
  if (plMiniAudioBusGraph::BusValues* pPending = GetPendingRebuildValues(uiBusIndex))
  {
    if (uiEffectIndex < pPending->m_EffectBypass.GetCount())
      pPending->m_EffectBypass[uiEffectIndex] = bBypass;
    return PL_SUCCESS;
  }

  if (!m_BusGraph.IsBuilt())
    return PL_FAILURE;

  m_BusGraph.SetBusEffectBypass(uiBusIndex, uiEffectIndex, bBypass);
  return PL_SUCCESS;
}

void plPlasmaAudioMiddleware::GetBusMeterInfo(plUInt32 uiBusIndex, plAudioBusMeterInfo& out_info) const
{
  GetBusMeter(uiBusIndex, out_info.m_fPeak, out_info.m_fRms);
}

void plPlasmaAudioMiddleware::GetProfilingInfo(plAudioSystemProfilingInfo& out_info) const
{
  out_info.m_uiActiveVoices = m_EventManager.GetRealVoiceCount();
  out_info.m_uiEventInstances = m_EventManager.GetInstanceCount();

  // sample data of the loaded clips plus the stream rings
  out_info.m_uiMemoryUsed = plAudioClipResource::GetTotalResidentBytes() + m_StreamService.GetRingBytesInUse();
  out_info.m_fDspLoad = m_pBackend != nullptr ? m_pBackend->GetDspLoad() : 0.0f;
}

void plPlasmaAudioMiddleware::GetBusMeter(plUInt32 uiBusIndex, float& out_fPeak, float& out_fRms) const
{
  out_fPeak = 0.0f;
  out_fRms = 0.0f;

  if (m_BusGraph.IsBuilt())
  {
    m_BusGraph.GetBusMeter(uiBusIndex, out_fPeak, out_fRms);
  }
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_MiniAudioMiddleware);
