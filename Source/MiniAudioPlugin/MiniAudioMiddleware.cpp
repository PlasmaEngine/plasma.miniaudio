#include <MiniAudioPlugin/MiniAudioPluginPCH.h>

#include <AudioSystem/Core/AudioSystem.h>
#include <AudioSystem/Core/AudioSystemAllocator.h>
#include <AudioSystem/Core/AudioSystemData.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <MiniAudioPlugin/MiniAudioAtlData.h>
#include <MiniAudioPlugin/MiniAudioMiddleware.h>
#include <MiniAudioPlugin/MiniAudioSingleton.h>
#include <MiniAudioPlugin/Resources/MiniAudioSoundResource.h>

PL_IMPLEMENT_SINGLETON(plMiniAudioMiddleware);

plMiniAudioEntityData::plMiniAudioEntityData(plAudioSystemDataID uiEntityId, bool bIsGlobal)
  : m_uiEntityId(uiEntityId)
  , m_bGlobalEntity(bIsGlobal)
{
}

plMiniAudioListenerData::plMiniAudioListenerData(plAudioSystemDataID uiListenerId)
  : m_uiListenerId(uiListenerId)
{
}

plMiniAudioEventData::plMiniAudioEventData(plAudioSystemDataID uiEventId)
  : m_uiEventId(uiEventId)
{
}

plMiniAudioTriggerData::plMiniAudioTriggerData(plAudioSystemDataID uiTriggerId, plStringView sResourceId)
  : m_uiTriggerId(uiTriggerId)
  , m_sResourceId(sResourceId)
{
}

plMiniAudioBankData::plMiniAudioBankData(plAudioSystemDataID uiBankId, plStringView sBankName)
  : m_uiBankId(uiBankId)
  , m_sBankName(sBankName)
{
}

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioEntityData, 1, plRTTIDefaultAllocator<plMiniAudioEntityData>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioListenerData, 1, plRTTIDefaultAllocator<plMiniAudioListenerData>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioEventData, 1, plRTTIDefaultAllocator<plMiniAudioEventData>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioTriggerData, 1, plRTTIDefaultAllocator<plMiniAudioTriggerData>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioBankData, 1, plRTTIDefaultAllocator<plMiniAudioBankData>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioRtpcData, 1, plRTTIDefaultAllocator<plMiniAudioRtpcData>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioSwitchStateData, 1, plRTTIDefaultAllocator<plMiniAudioSwitchStateData>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioEnvironmentData, 1, plRTTIDefaultAllocator<plMiniAudioEnvironmentData>)
PL_END_DYNAMIC_REFLECTED_TYPE;

plMiniAudioMiddleware::plMiniAudioMiddleware()
  : m_SingletonRegistrar(this)
{
  // the PCM stream interface is registered on its own, so consumers find it only when this middleware has it
  plSingletonRegistry::Register<plAudioPcmStreamProvider>(this);
}

plMiniAudioMiddleware::~plMiniAudioMiddleware()
{
  plSingletonRegistry::Unregister<plAudioPcmStreamProvider>();
}

plResult plMiniAudioMiddleware::LoadConfiguration(const plOpenDdlReaderElement& reader)
{
  PL_IGNORE_UNUSED(reader);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::Startup()
{
  m_pBackend = plMiniAudioSingleton::GetSingleton();
  if (m_pBackend == nullptr)
    return PL_FAILURE;

  m_pBackend->Startup();
  return PL_SUCCESS;
}

void plMiniAudioMiddleware::Update(plTime delta)
{
  if (m_pBackend != nullptr)
  {
    m_pBackend->UpdateSound();
    m_EventManager.Update(delta, m_pBackend->GetListenerPosition());
  }
}

plResult plMiniAudioMiddleware::Shutdown()
{
  m_EventManager.StopAll(true);
  DestroyAllPcmStreams();
  UnloadMixer().IgnoreResult();

  if (m_pBackend != nullptr)
  {
    m_pBackend->Shutdown();
  }
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::Release()
{
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::StopAllSounds()
{
  m_EventManager.StopAll(false);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::AddEntity(plAudioSystemEntityData* pEntityData, const char* szEntityName)
{
  PL_IGNORE_UNUSED(pEntityData);
  PL_IGNORE_UNUSED(szEntityName);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::ResetEntity(plAudioSystemEntityData* pEntityData)
{
  PL_IGNORE_UNUSED(pEntityData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::UpdateEntity(plAudioSystemEntityData* pEntityData)
{
  PL_IGNORE_UNUSED(pEntityData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::RemoveEntity(plAudioSystemEntityData* pEntityData)
{
  if (const auto* pEntity = plDynamicCast<const plMiniAudioEntityData*>(pEntityData))
  {
    // the entity data is going away — stop its instances immediately and sever links
    m_EventManager.StopAllOnEntity(pEntity, true);
  }
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::SetEntityTransform(plAudioSystemEntityData* pEntityData, const plAudioSystemTransform& Transform)
{
  auto* pEntity = plDynamicCast<plMiniAudioEntityData*>(pEntityData);
  if (pEntity == nullptr)
    return PL_FAILURE;

  pEntity->m_LastTransform = Transform;
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::LoadTrigger(plAudioSystemEntityData* pEntityData, const plAudioSystemTriggerData* pTriggerData, plAudioSystemEventData* pEventData)
{
  PL_IGNORE_UNUSED(pEntityData);
  PL_IGNORE_UNUSED(pTriggerData);
  PL_IGNORE_UNUSED(pEventData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::ActivateTrigger(plAudioSystemEntityData* pEntityData, const plAudioSystemTriggerData* pTriggerData, plAudioSystemEventData* pEventData)
{
  if (m_pBackend == nullptr)
    return PL_FAILURE;

  auto* pEntity = plDynamicCast<plMiniAudioEntityData*>(pEntityData);

  plTransform t = plTransform::MakeIdentity();
  if (pEntity != nullptr)
  {
    t.m_vPosition = pEntity->m_LastTransform.m_vPosition;
  }

  // legacy trigger data — registered by plMiniAudioSoundResource on load, always a simple sound
  if (const auto* pTrigger = plDynamicCast<const plMiniAudioTriggerData*>(pTriggerData))
  {
    return m_pBackend->OneShotSound(nullptr, pTrigger->m_sResourceId, t);
  }

  if (const auto* pStringTrigger = plDynamicCast<const plAudioSystemStringTriggerData*>(pTriggerData))
  {
    const plStringView sName = pStringTrigger->m_sTriggerName;

    // if a legacy sound resource is already known under this id, keep the legacy behavior
    if (plResourceManager::GetExistingResource<plMiniAudioSoundResource>(sName).IsValid())
    {
      return m_pBackend->OneShotSound(nullptr, sName, t);
    }

    // audio event path (also covers events not yet loaded — the manager loads the resource)
    auto* pEvent = plDynamicCast<plMiniAudioEventData*>(pEventData);
    return m_EventManager.ActivateEvent(sName, pEntity, pEvent, &m_BusGraph, m_pBackend->GetEngine());
  }

  return PL_FAILURE;
}

plResult plMiniAudioMiddleware::UnloadTrigger(plAudioSystemEntityData* pEntityData, const plAudioSystemTriggerData* pTriggerData)
{
  PL_IGNORE_UNUSED(pEntityData);
  PL_IGNORE_UNUSED(pTriggerData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::StopEvent(plAudioSystemEntityData* pEntityData, const plAudioSystemEventData* pEventData)
{
  PL_IGNORE_UNUSED(pEntityData);

  if (const auto* pEvent = plDynamicCast<const plMiniAudioEventData*>(pEventData))
  {
    m_EventManager.StopEvent(pEvent, false).IgnoreResult();
  }
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::StopAllEvents(plAudioSystemEntityData* pEntityData)
{
  if (const auto* pEntity = plDynamicCast<const plMiniAudioEntityData*>(pEntityData))
  {
    m_EventManager.StopAllOnEntity(pEntity, false);
  }
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::SetRtpc(plAudioSystemEntityData* pEntityData, const plAudioSystemRtpcData* pRtpcData, float fValue)
{
  const auto* pEntity = plDynamicCast<const plMiniAudioEntityData*>(pEntityData);
  const auto* pRtpc = plDynamicCast<const plAudioSystemStringRtpcData*>(pRtpcData);

  if (pEntity == nullptr || pRtpc == nullptr)
    return PL_FAILURE;

  // RTPCs map onto the audio event parameter of the same name
  m_EventManager.SetEventParameter(pEntity, pRtpc->m_sRtpcName, fValue);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::SetEventParameter(plAudioSystemEntityData* pEntityData, plStringView sParamName, float fValue)
{
  const auto* pEntity = plDynamicCast<const plMiniAudioEntityData*>(pEntityData);
  if (pEntity == nullptr)
    return PL_FAILURE;

  m_EventManager.SetEventParameter(pEntity, sParamName, fValue);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::ResetRtpc(plAudioSystemEntityData* pEntityData, const plAudioSystemRtpcData* pRtpcData)
{
  PL_IGNORE_UNUSED(pEntityData);
  PL_IGNORE_UNUSED(pRtpcData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::SetSwitchState(plAudioSystemEntityData* pEntityData, const plAudioSystemSwitchStateData* pSwitchStateData)
{
  auto* pEntity = plDynamicCast<plMiniAudioEntityData*>(pEntityData);
  const auto* pSwitchState = plDynamicCast<const plAudioSystemStringSwitchStateData*>(pSwitchStateData);

  if (pEntity == nullptr || pSwitchState == nullptr)
    return PL_FAILURE;

  // stored per group, so instruments filtered on this switch pick it up on their next trigger
  pEntity->SetSwitchState(pSwitchState->GetGroup(), pSwitchState->m_sSwitchStateName);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::SetObstructionAndOcclusion(plAudioSystemEntityData* pEntityData, float fObstruction, float fOcclusion)
{
  auto* pEntity = plDynamicCast<plMiniAudioEntityData*>(pEntityData);
  if (pEntity == nullptr)
    return PL_FAILURE;

  // stored on the entity; playing event instances smooth toward these targets each update
  // (per-voice low-pass for occlusion, direct-gain reduction for obstruction)
  pEntity->m_fTargetObstruction = plMath::Clamp(fObstruction, 0.0f, 1.0f);
  pEntity->m_fTargetOcclusion = plMath::Clamp(fOcclusion, 0.0f, 1.0f);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::SetEnvironmentAmount(plAudioSystemEntityData* pEntityData, const plAudioSystemEnvironmentData* pEnvironmentData, float fAmount)
{
  auto* pEntity = plDynamicCast<plMiniAudioEntityData*>(pEntityData);
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

plResult plMiniAudioMiddleware::AddListener(plAudioSystemListenerData* pListenerData, const char* szListenerName, bool bIsDefault)
{
  PL_IGNORE_UNUSED(pListenerData);
  PL_IGNORE_UNUSED(szListenerName);
  PL_IGNORE_UNUSED(bIsDefault);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::ResetListener(plAudioSystemListenerData* pListenerData)
{
  PL_IGNORE_UNUSED(pListenerData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::RemoveListener(plAudioSystemListenerData* pListenerData)
{
  PL_IGNORE_UNUSED(pListenerData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::SetListenerTransform(plAudioSystemListenerData* pListenerData, const plAudioSystemTransform& Transform)
{
  const auto* pListener = plDynamicCast<const plMiniAudioListenerData*>(pListenerData);
  if (pListener == nullptr || m_pBackend == nullptr)
    return PL_FAILURE;

  m_pBackend->SetListener(static_cast<plInt32>(pListener->m_uiListenerId), Transform.m_vPosition, Transform.m_vForward, Transform.m_vUp, Transform.m_vVelocity);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::LoadBank(plAudioSystemBankData* pBankData)
{
  PL_IGNORE_UNUSED(pBankData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::UnloadBank(plAudioSystemBankData* pBankData)
{
  PL_IGNORE_UNUSED(pBankData);
  return PL_SUCCESS;
}

plAudioSystemEntityData* plMiniAudioMiddleware::CreateWorldEntity(plAudioSystemDataID uiEntityId)
{
  return PL_AUDIOSYSTEM_NEW(plMiniAudioEntityData, uiEntityId, true);
}

plAudioSystemEntityData* plMiniAudioMiddleware::CreateEntityData(plAudioSystemDataID uiEntityId)
{
  return PL_AUDIOSYSTEM_NEW(plMiniAudioEntityData, uiEntityId, false);
}

plResult plMiniAudioMiddleware::DestroyEntityData(plAudioSystemEntityData* pEntityData)
{
  PL_AUDIOSYSTEM_DELETE(pEntityData);
  return PL_SUCCESS;
}

plAudioSystemListenerData* plMiniAudioMiddleware::CreateListenerData(plAudioSystemDataID uiListenerId)
{
  return PL_AUDIOSYSTEM_NEW(plMiniAudioListenerData, uiListenerId);
}

plResult plMiniAudioMiddleware::DestroyListenerData(plAudioSystemListenerData* pListenerData)
{
  PL_AUDIOSYSTEM_DELETE(pListenerData);
  return PL_SUCCESS;
}

plAudioSystemEventData* plMiniAudioMiddleware::CreateEventData(plAudioSystemDataID uiEventId)
{
  return PL_AUDIOSYSTEM_NEW(plMiniAudioEventData, uiEventId);
}

plResult plMiniAudioMiddleware::ResetEventData(plAudioSystemEventData* pEventData)
{
  PL_IGNORE_UNUSED(pEventData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::DestroyEventData(plAudioSystemEventData* pEventData)
{
  PL_AUDIOSYSTEM_DELETE(pEventData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::DestroyBank(plAudioSystemBankData* pBankData)
{
  PL_AUDIOSYSTEM_DELETE(pBankData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::DestroyTriggerData(plAudioSystemTriggerData* pTriggerData)
{
  PL_AUDIOSYSTEM_DELETE(pTriggerData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::DestroyRtpcData(plAudioSystemRtpcData* pRtpcData)
{
  PL_AUDIOSYSTEM_DELETE(pRtpcData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::DestroySwitchStateData(plAudioSystemSwitchStateData* pSwitchStateData)
{
  PL_AUDIOSYSTEM_DELETE(pSwitchStateData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::DestroyEnvironmentData(plAudioSystemEnvironmentData* pEnvironmentData)
{
  PL_AUDIOSYSTEM_DELETE(pEnvironmentData);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::SetLanguage(const char* szLanguage)
{
  PL_IGNORE_UNUSED(szLanguage);
  return PL_SUCCESS;
}

const char* plMiniAudioMiddleware::GetMiddlewareName() const
{
  return "MiniAudio";
}

float plMiniAudioMiddleware::GetMasterGain() const
{
  return m_pBackend != nullptr ? m_pBackend->GetMasterChannelVolume() : 1.0f;
}

bool plMiniAudioMiddleware::GetMute() const
{
  return m_pBackend != nullptr ? m_pBackend->GetMasterChannelMute() : false;
}

void plMiniAudioMiddleware::OnMasterGainChange(float fGain)
{
  if (m_pBackend != nullptr)
    m_pBackend->SetMasterChannelVolume(fGain);
}

void plMiniAudioMiddleware::OnMuteChange(bool bMute)
{
  if (m_pBackend != nullptr)
    m_pBackend->SetMasterChannelMute(bMute);
}

void plMiniAudioMiddleware::OnMasterPauseChange(bool bPaused)
{
  if (m_pBackend != nullptr)
    m_pBackend->SetMasterChannelPaused(bPaused);
}

bool plMiniAudioMiddleware::GetMasterPaused() const
{
  return m_pBackend != nullptr ? m_pBackend->GetMasterChannelPaused() : false;
}

void plMiniAudioMiddleware::SetSoundGroupVolume(plStringView sGroupGuid, float fVolume)
{
  if (m_pBackend != nullptr)
    m_pBackend->SetSoundGroupVolume(sGroupGuid, fVolume);
}

float plMiniAudioMiddleware::GetSoundGroupVolume(plStringView sGroupGuid) const
{
  return m_pBackend != nullptr ? m_pBackend->GetSoundGroupVolume(sGroupGuid) : 1.0f;
}

void plMiniAudioMiddleware::SetNumListeners(plUInt8 uiNumListeners)
{
  if (m_pBackend != nullptr)
    m_pBackend->SetNumListeners(uiNumListeners);
}

plUInt8 plMiniAudioMiddleware::GetNumListeners() const
{
  return m_pBackend != nullptr ? m_pBackend->GetNumListeners() : 1;
}

void plMiniAudioMiddleware::OnLoseFocus()
{
  OnMasterPauseChange(true);
}

void plMiniAudioMiddleware::OnGainFocus()
{
  OnMasterPauseChange(false);
}

bool plMiniAudioMiddleware::IsEventDataPlaying(const plAudioSystemEventData* pEventData) const
{
  if (const auto* pEvent = plDynamicCast<const plMiniAudioEventData*>(pEventData))
  {
    return m_EventManager.IsEventPlaying(pEvent);
  }
  return false;
}

plResult plMiniAudioMiddleware::LoadMixer(const plAudioMixerDesc& desc)
{
  if (m_pBackend == nullptr)
    return PL_FAILURE;

  UnloadMixer().IgnoreResult();

  PL_SUCCEED_OR_RETURN(m_BusGraph.Build(desc, m_pBackend->GetEngine()));

  // streams created before the mixer existed sit on the endpoint; move them onto their buses
  ReattachPcmStreams();

  // register the mixer's environment aux buses as ATL environments so the generic
  // Box/Sphere environment components resolve their names
  if (auto* pAudioSystem = plAudioSystem::GetSingleton())
  {
    for (const auto& busDesc : desc.m_Buses)
    {
      if (!busDesc.m_bIsAux || busDesc.m_sEnvironmentName.IsEmpty())
        continue;

      const plAudioSystemDataID uiEnvironmentId = plHashHelper<plStringView>::Hash(busDesc.m_sEnvironmentName.GetView());
      pAudioSystem->RegisterEnvironment(uiEnvironmentId, PL_AUDIOSYSTEM_NEW(plAudioSystemStringEnvironmentData, uiEnvironmentId, busDesc.m_sEnvironmentName));
      m_RegisteredEnvironmentIds.PushBack(uiEnvironmentId);
    }
  }

  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::UnloadMixer()
{
  if (!m_BusGraph.IsBuilt())
    return PL_SUCCESS;

  // voices attached to bus groups must not outlive the graph
  m_EventManager.StopAll(true);

  if (auto* pAudioSystem = plAudioSystem::GetSingleton())
  {
    for (const plAudioSystemDataID uiEnvironmentId : m_RegisteredEnvironmentIds)
    {
      pAudioSystem->UnregisterEnvironment(uiEnvironmentId);
    }
  }
  m_RegisteredEnvironmentIds.Clear();

  // stream nodes must not point at bus groups that are about to go away
  DetachPcmStreamsToEndpoint();

  m_BusGraph.Teardown();
  return PL_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// PCM streams
// ---------------------------------------------------------------------------------------------

plMiniAudioMiddleware::PcmStream* plMiniAudioMiddleware::FindPcmStream(plUInt32 uiStreamId)
{
  for (PcmStream& stream : m_PcmStreams)
  {
    if (stream.m_uiId == uiStreamId)
      return &stream;
  }
  return nullptr;
}

const plMiniAudioMiddleware::PcmStream* plMiniAudioMiddleware::FindPcmStream(plUInt32 uiStreamId) const
{
  for (const PcmStream& stream : m_PcmStreams)
  {
    if (stream.m_uiId == uiStreamId)
      return &stream;
  }
  return nullptr;
}

void plMiniAudioMiddleware::AttachPcmStream(PcmStream& ref_stream)
{
  if (m_pBackend == nullptr || ref_stream.m_pNode == nullptr)
    return;

  ma_node* pTarget = nullptr;
  if (m_BusGraph.IsBuilt())
  {
    pTarget = reinterpret_cast<ma_node*>(m_BusGraph.GetBusInput(m_BusGraph.ResolveBus(ref_stream.m_sBus)));
  }
  if (pTarget == nullptr)
  {
    pTarget = ma_node_graph_get_endpoint(ma_engine_get_node_graph(m_pBackend->GetEngine()));
  }

  ma_node_attach_output_bus(&ref_stream.m_pNode->m_Base, 0, pTarget, 0);
}

void plMiniAudioMiddleware::ReattachPcmStreams()
{
  PL_LOCK(m_PcmStreamMutex);
  for (PcmStream& stream : m_PcmStreams)
  {
    AttachPcmStream(stream);
  }
}

void plMiniAudioMiddleware::DetachPcmStreamsToEndpoint()
{
  if (m_pBackend == nullptr)
    return;

  PL_LOCK(m_PcmStreamMutex);
  ma_node* pEndpoint = ma_node_graph_get_endpoint(ma_engine_get_node_graph(m_pBackend->GetEngine()));
  for (PcmStream& stream : m_PcmStreams)
  {
    if (stream.m_pNode != nullptr)
    {
      ma_node_attach_output_bus(&stream.m_pNode->m_Base, 0, pEndpoint, 0);
    }
  }
}

void plMiniAudioMiddleware::DestroyAllPcmStreams()
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

plResult plMiniAudioMiddleware::CreatePcmStream(const plAudioPcmStreamDesc& desc, plUInt32& out_uiStreamId)
{
  out_uiStreamId = 0;
  if (m_pBackend == nullptr)
    return PL_FAILURE;

  plUniquePtr<plMaPcmStreamNode> pNode = PL_DEFAULT_NEW(plMaPcmStreamNode);
  if (plMiniAudioDsp::InitPcmStreamNode(m_pBackend->GetEngine(), pNode.Borrow(), desc.m_uiChannels, desc.m_uiSampleRate, desc.m_uiRingFrames).Failed())
    return PL_FAILURE;

  PL_LOCK(m_PcmStreamMutex);

  PcmStream& stream = m_PcmStreams.ExpandAndGetRef();
  stream.m_uiId = m_uiNextPcmStreamId++;
  stream.m_sBus = desc.m_sOutputBus;
  stream.m_pNode = std::move(pNode);
  AttachPcmStream(stream);

  out_uiStreamId = stream.m_uiId;
  return PL_SUCCESS;
}

void plMiniAudioMiddleware::DestroyPcmStream(plUInt32 uiStreamId)
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

plUInt32 plMiniAudioMiddleware::WritePcmFrames(plUInt32 uiStreamId, plArrayPtr<const float> interleaved)
{
  PL_LOCK(m_PcmStreamMutex);
  PcmStream* pStream = FindPcmStream(uiStreamId);
  if (pStream == nullptr || pStream->m_pNode == nullptr)
    return 0;

  const plUInt32 uiFrames = interleaved.GetCount() / plMath::Max(1u, pStream->m_pNode->m_uiSourceChannels);
  return plMiniAudioDsp::WritePcmStreamFrames(pStream->m_pNode.Borrow(), interleaved.GetPtr(), uiFrames);
}

plUInt32 plMiniAudioMiddleware::GetPcmStreamFreeFrames(plUInt32 uiStreamId) const
{
  PL_LOCK(m_PcmStreamMutex);
  const PcmStream* pStream = FindPcmStream(uiStreamId);
  if (pStream == nullptr || pStream->m_pNode == nullptr)
    return 0;
  return plMiniAudioDsp::GetPcmStreamFreeFrames(pStream->m_pNode.Borrow());
}

plUInt64 plMiniAudioMiddleware::GetPcmStreamFramesRendered(plUInt32 uiStreamId) const
{
  PL_LOCK(m_PcmStreamMutex);
  const PcmStream* pStream = FindPcmStream(uiStreamId);
  if (pStream == nullptr || pStream->m_pNode == nullptr)
    return 0;
  return pStream->m_pNode->m_uiFramesRendered.load(std::memory_order_relaxed);
}

void plMiniAudioMiddleware::SetPcmStreamVolume(plUInt32 uiStreamId, float fLinearGain)
{
  PL_LOCK(m_PcmStreamMutex);
  if (PcmStream* pStream = FindPcmStream(uiStreamId); pStream != nullptr && pStream->m_pNode != nullptr)
  {
    pStream->m_pNode->m_fVolume.store(plMath::Max(0.0f, fLinearGain), std::memory_order_relaxed);
  }
}

void plMiniAudioMiddleware::SetPcmStreamPitch(plUInt32 uiStreamId, float fPitch)
{
  PL_LOCK(m_PcmStreamMutex);
  if (PcmStream* pStream = FindPcmStream(uiStreamId); pStream != nullptr && pStream->m_pNode != nullptr)
  {
    pStream->m_pNode->m_fPitch.store(plMath::Clamp(fPitch, 0.05f, 8.0f), std::memory_order_relaxed);
  }
}

void plMiniAudioMiddleware::SetPcmStreamPaused(plUInt32 uiStreamId, bool bPaused)
{
  PL_LOCK(m_PcmStreamMutex);
  if (PcmStream* pStream = FindPcmStream(uiStreamId); pStream != nullptr && pStream->m_pNode != nullptr)
  {
    pStream->m_pNode->m_bPaused.store(bPaused, std::memory_order_relaxed);
  }
}

void plMiniAudioMiddleware::FlushPcmStream(plUInt32 uiStreamId)
{
  PL_LOCK(m_PcmStreamMutex);
  if (PcmStream* pStream = FindPcmStream(uiStreamId); pStream != nullptr && pStream->m_pNode != nullptr)
  {
    pStream->m_pNode->m_uiFlushRequested.fetch_add(1, std::memory_order_release);
  }
}

plResult plMiniAudioMiddleware::SetBusVolume(plUInt32 uiBusIndex, float fLinearGain)
{
  if (!m_BusGraph.IsBuilt())
    return PL_FAILURE;

  m_BusGraph.SetBusVolume(uiBusIndex, fLinearGain);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::SetBusPitch(plUInt32 uiBusIndex, float fPitch)
{
  if (!m_BusGraph.IsBuilt())
    return PL_FAILURE;

  m_BusGraph.SetBusPitch(uiBusIndex, fPitch);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::SetBusSendLevel(plUInt32 uiBusIndex, plUInt32 uiSendIndex, float fLinearLevel)
{
  if (!m_BusGraph.IsBuilt())
    return PL_FAILURE;

  m_BusGraph.SetBusSendLevel(uiBusIndex, uiSendIndex, fLinearLevel);
  return PL_SUCCESS;
}

plResult plMiniAudioMiddleware::SetBusEffectParameter(plUInt32 uiBusIndex, plUInt32 uiEffectIndex, plUInt32 uiParamIndex, float fValue)
{
  if (!m_BusGraph.IsBuilt())
    return PL_FAILURE;

  m_BusGraph.SetBusEffectParameter(uiBusIndex, uiEffectIndex, uiParamIndex, fValue);
  return PL_SUCCESS;
}

void plMiniAudioMiddleware::GetProfilingInfo(plAudioSystemProfilingInfo& out_info) const
{
  out_info.m_uiActiveVoices = m_EventManager.GetActiveVoiceCount();
  out_info.m_uiEventInstances = m_EventManager.GetInstanceCount();
  out_info.m_uiMemoryUsed = 0;
  out_info.m_fDspLoad = 0.0f;
}

void plMiniAudioMiddleware::GetBusMeterInfo(plUInt32 uiBusIndex, plAudioBusMeterInfo& out_info) const
{
  m_BusGraph.GetBusMeter(uiBusIndex, out_info.m_fPeak, out_info.m_fRms);
}

PL_STATICLINK_FILE(MiniAudioPlugin, MiniAudioPlugin_MiniAudioMiddleware);
