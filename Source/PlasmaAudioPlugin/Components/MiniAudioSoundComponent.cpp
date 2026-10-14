#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <AudioSystem/Core/AudioSystem.h>
#include <AudioSystem/Core/AudioSystemRequests.h>
#include <Core/Messages/DeleteObjectMessage.h>
#include <Core/ResourceManager/Implementation/ResourceHandleReflection.h>
#include <Core/World/GameObject.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <PlasmaAudioPlugin/Components/MiniAudioSoundComponent.h>
#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioPlugin/Resources/MiniAudioSoundResource.h>

namespace
{
  /// Entity and event ids of legacy sound components live in their own range, far above the ids
  /// plAudioProxyComponent and plAudioTriggerComponent hand out.
  constexpr plAudioSystemDataID kLegacyIdBase = plAudioSystemDataID(1) << 62;
  plAtomicInteger64 s_iNextLegacyId;

  plAudioSystemDataID AllocateLegacyId()
  {
    return kLegacyIdBase + static_cast<plAudioSystemDataID>(s_iNextLegacyId.Increment());
  }

  /// Request callbacks can arrive after the component is gone: they carry the handle and resolve it here.
  plMutex s_ComponentRegistryMutex;
  plHashTable<plComponentHandle, plMiniAudioSoundComponent*> s_ComponentRegistry;

  /// Fire-and-forget sounds started with StartOneShot() own a transient audio entity until they end.
  struct LegacyOneShot
  {
    enum class State : plUInt8
    {
      Starting,
      Playing,
      Done,
    };

    plAudioSystemDataID m_uiEntityId = kInvalidAudioSystemId;
    plAudioSystemDataID m_uiTriggerId = kInvalidAudioSystemId;
    plAudioSystemDataID m_uiEventId = kInvalidAudioSystemId;
    State m_eState = State::Starting;
  };

  plMutex s_OneShotMutex;
  plDynamicArray<LegacyOneShot> s_OneShots;

  void SetOneShotState(plAudioSystemDataID uiEventId, LegacyOneShot::State eState)
  {
    PL_LOCK(s_OneShotMutex);
    for (LegacyOneShot& oneShot : s_OneShots)
    {
      if (oneShot.m_uiEventId == uiEventId)
      {
        oneShot.m_eState = eState;
        return;
      }
    }
  }

  /// Releases the transient entities of one-shots that have ended.
  void PollOneShots()
  {
    plAudioSystem* pAudioSystem = plAudioSystem::GetSingleton();
    if (pAudioSystem == nullptr)
      return;

    PL_LOCK(s_OneShotMutex);

    for (plUInt32 i = 0; i < s_OneShots.GetCount();)
    {
      LegacyOneShot& oneShot = s_OneShots[i];

      if (oneShot.m_eState == LegacyOneShot::State::Playing && !pAudioSystem->IsEventPlaying(oneShot.m_uiTriggerId, oneShot.m_uiEventId))
      {
        oneShot.m_eState = LegacyOneShot::State::Done;
      }

      if (oneShot.m_eState != LegacyOneShot::State::Done)
      {
        ++i;
        continue;
      }

      plAudioSystemRequestsQueue rq;

      plAudioSystemRequestUnloadTrigger unload;
      unload.m_uiEntityId = oneShot.m_uiEntityId;
      unload.m_uiObjectId = oneShot.m_uiTriggerId;
      unload.m_uiEventId = oneShot.m_uiEventId;
      rq.PushBack(unload);

      plAudioSystemRequestUnregisterEntity unregister;
      unregister.m_uiEntityId = oneShot.m_uiEntityId;
      rq.PushBack(unregister);

      pAudioSystem->SendRequests(rq);

      s_OneShots.RemoveAtAndSwap(i);
    }
  }
} // namespace

plMiniAudioSoundComponentManager::plMiniAudioSoundComponentManager(plWorld* pWorld)
  : plComponentManager(pWorld)
{
}

void plMiniAudioSoundComponentManager::Initialize()
{
  SUPER::Initialize();

  {
    auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plMiniAudioSoundComponentManager::UpdateEvents, this);
    desc.m_Phase = UpdateFunctionDesc::Phase::PostTransform;
    desc.m_bOnlyUpdateWhenSimulating = true;

    RegisterUpdateFunction(desc);
  }
}

void plMiniAudioSoundComponentManager::Deinitialize()
{
  SUPER::Deinitialize();
}

void plMiniAudioSoundComponentManager::UpdateEvents(const plWorldModule::UpdateContext& context)
{
  constexpr plUInt32 uiUpdatesPerSec = 20;
  constexpr plTime tUpdateRate = plTime::Milliseconds(1000 / uiUpdatesPerSec);

  const float fUpdateFraction = (GetWorld()->GetClock().GetTimeDiff() / tUpdateRate).AsFloatInSeconds();

  const plUInt32 uiNumComps = m_ComponentStorage.GetCount();

  if (m_uiFirstComponentIndex >= uiNumComps)
  {
    m_uiFirstComponentIndex = 0;
  }

  const plUInt32 uiNumUpdate = static_cast<plUInt32>(uiNumComps * fUpdateFraction) + 1;
  const plUInt32 uiLastCompP1 = plMath::Min(m_uiFirstComponentIndex + uiNumUpdate, uiNumComps);

  for (auto it = m_ComponentStorage.GetIterator(m_uiFirstComponentIndex, uiNumComps); it.IsValid(); ++it)
  {
    ComponentType* pComponent = it;

    // a lot of components will actually be inactive (waiting to be reused)
    if (pComponent->IsActiveAndInitialized())
    {
      pComponent->Update();
    }
  }

  m_uiFirstComponentIndex = uiLastCompP1;

  PollOneShots();
}

//////////////////////////////////////////////////////////////////////////

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plMiniAudioSoundComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_RESOURCE_MEMBER_PROPERTY("Audio", m_hSound)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_MiniAudio_Sound", plDependencyFlags::Package)),
    PL_ACCESSOR_PROPERTY("Paused", GetPaused, SetPaused),
    PL_ACCESSOR_PROPERTY("Volume", GetVolume, SetVolume)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_ACCESSOR_PROPERTY("Pitch", GetPitch, SetPitch)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.1f, 10.0f)),
    PL_ACCESSOR_PROPERTY("NoGlobalPitch", GetNoGlobalPitch, SetNoGlobalPitch),
    PL_ENUM_MEMBER_PROPERTY("OnFinishedAction", plOnComponentFinishedAction2, m_OnFinishedAction),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgDeleteGameObject, OnMsgDeleteGameObject),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_FUNCTIONS
  {
    PL_SCRIPT_FUNCTION_PROPERTY(Play),
    PL_SCRIPT_FUNCTION_PROPERTY(Pause),
    PL_SCRIPT_FUNCTION_PROPERTY(Stop),
    PL_SCRIPT_FUNCTION_PROPERTY(FadeOut, In, "Delay"),
    PL_SCRIPT_FUNCTION_PROPERTY(StartOneShot),
  }
  PL_END_FUNCTIONS;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Audio/MiniAudio"),
    new plHiddenAttribute(),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

enum
{
  NoGlobalPitch = 0,
};

plMiniAudioSoundComponent::plMiniAudioSoundComponent() = default;
plMiniAudioSoundComponent::~plMiniAudioSoundComponent() = default;

void plMiniAudioSoundComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);

  auto& s = inout_stream.GetStream();

  s << m_hSound;
  s << m_bPaused;
  s << m_fPitch;
  s << m_fComponentVolume;

  plOnComponentFinishedAction2::StorageType type = m_OnFinishedAction;
  s << type;
}

void plMiniAudioSoundComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  const plUInt32 uiVersion = inout_stream.GetComponentTypeVersion(GetStaticRTTI());

  auto& s = inout_stream.GetStream();

  s >> m_hSound;
  s >> m_bPaused;
  s >> m_fPitch;
  s >> m_fComponentVolume;

  plOnComponentFinishedAction2::StorageType type;
  s >> type;
  m_OnFinishedAction = (plOnComponentFinishedAction2::Enum)type;
}

void plMiniAudioSoundComponent::SetPaused(bool b)
{
  if (b == m_bPaused)
    return;

  m_bPaused = b;

  if (m_bPaused)
  {
    Pause();
  }
  else
  {
    Play();
  }
}

void plMiniAudioSoundComponent::SetPitch(float f)
{
  if (f == m_fPitch)
    return;

  m_fPitch = f;
}

void plMiniAudioSoundComponent::SetVolume(float f)
{
  if (f == m_fComponentVolume)
    return;

  m_fComponentVolume = f;
}

void plMiniAudioSoundComponent::SetNoGlobalPitch(bool bEnable)
{
  SetUserFlag(NoGlobalPitch, bEnable);
}

bool plMiniAudioSoundComponent::GetNoGlobalPitch() const
{
  return GetUserFlag(NoGlobalPitch);
}

void plMiniAudioSoundComponent::Initialize()
{
  SUPER::Initialize();

  PL_LOCK(s_ComponentRegistryMutex);
  s_ComponentRegistry[GetHandle()] = this;
}

void plMiniAudioSoundComponent::Deinitialize()
{
  {
    PL_LOCK(s_ComponentRegistryMutex);
    s_ComponentRegistry.Remove(GetHandle());
  }

  ReleaseAudioObjects();

  SUPER::Deinitialize();
}

void plMiniAudioSoundComponent::OnSimulationStarted()
{
  if (!m_bPaused)
  {
    Play();
  }
}

void plMiniAudioSoundComponent::OnDeactivated()
{
  Stop();
}

plAudioSystemDataID plMiniAudioSoundComponent::PrepareTrigger(bool& out_bLooping) const
{
  out_bLooping = false;

  if (!m_hSound.IsValid())
    return kInvalidAudioSystemId;

  // loading the resource registers its ATL trigger (see plMiniAudioSoundResource::UpdateContent)
  plResourceLock<plMiniAudioSoundResource> pResource(m_hSound, plResourceAcquireMode::BlockTillLoaded_NeverFail);
  if (pResource.GetAcquireResult() != plResourceAcquireResult::Final)
    return kInvalidAudioSystemId;

  out_bLooping = pResource->GetLoop();
  return plHashHelper<plStringView>::Hash(m_hSound.GetResourceID().GetView());
}

plAudioSystemTransform plMiniAudioSoundComponent::ComputeTransform() const
{
  const plQuat rotation = GetOwner()->GetGlobalRotation();

  plAudioSystemTransform transform;
  transform.m_vPosition = GetOwner()->GetGlobalPosition();
  transform.m_vForward = (rotation * plVec3::MakeAxisX()).GetNormalized();
  transform.m_vUp = (rotation * plVec3::MakeAxisZ()).GetNormalized();
  transform.m_vVelocity = GetOwner()->GetLinearVelocity();
  return transform;
}

float plMiniAudioSoundComponent::ComputeVolume() const
{
  return m_fComponentVolume;
}

float plMiniAudioSoundComponent::ComputePitch() const
{
  if (GetNoGlobalPitch())
    return m_fPitch;

  return m_fPitch * static_cast<float>(GetWorld()->GetClock().GetSpeed());
}

void plMiniAudioSoundComponent::EnsureEntityRegistered()
{
  if (m_bEntityRegistered)
    return;

  m_uiEntityId = AllocateLegacyId();
  m_uiEventId = AllocateLegacyId();

  plAudioSystemRequestRegisterEntity request;
  request.m_uiEntityId = m_uiEntityId;
  request.m_sName = GetOwner()->GetName();

  plAudioSystem::GetSingleton()->SendRequestSync(request);

  m_bEntityRegistered = true;
}

void plMiniAudioSoundComponent::EnsureTriggerLoaded(plAudioSystemDataID uiTriggerId)
{
  if (m_uiLoadedTriggerId == uiTriggerId)
    return;

  plAudioSystemRequestsQueue rq;

  // the sound resource changed: detach the event from the previous trigger first
  if (m_uiLoadedTriggerId != kInvalidAudioSystemId)
  {
    plAudioSystemRequestUnloadTrigger unload;
    unload.m_uiEntityId = m_uiEntityId;
    unload.m_uiObjectId = m_uiLoadedTriggerId;
    unload.m_uiEventId = m_uiEventId;
    rq.PushBack(unload);
  }

  plAudioSystemRequestLoadTrigger load;
  load.m_uiEntityId = m_uiEntityId;
  load.m_uiObjectId = uiTriggerId;
  load.m_uiEventId = m_uiEventId;
  rq.PushBack(load);

  plAudioSystem::GetSingleton()->SendRequests(rq);

  m_uiLoadedTriggerId = uiTriggerId;
}

void plMiniAudioSoundComponent::SendTransformAndMultipliers(bool bForce)
{
  if (!m_bEntityRegistered)
    return;

  plAudioSystemRequestsQueue rq;

  const plAudioSystemTransform transform = ComputeTransform();
  if (bForce || !(transform == m_LastTransform))
  {
    m_LastTransform = transform;

    plAudioSystemRequestSetEntityTransform request;
    request.m_uiEntityId = m_uiEntityId;
    request.m_Transform = transform;
    rq.PushBack(request);
  }

  const float fVolume = ComputeVolume();
  if (bForce || fVolume != m_fLastSentVolume)
  {
    m_fLastSentVolume = fVolume;

    plAudioSystemRequestSetEventParameter request;
    request.m_uiEntityId = m_uiEntityId;
    request.m_sName = plPlasmaAudioReservedParameter::Volume;
    request.m_fValue = fVolume;
    rq.PushBack(request);
  }

  const float fPitch = ComputePitch();
  if (bForce || fPitch != m_fLastSentPitch)
  {
    m_fLastSentPitch = fPitch;

    plAudioSystemRequestSetEventParameter request;
    request.m_uiEntityId = m_uiEntityId;
    request.m_sName = plPlasmaAudioReservedParameter::Pitch;
    request.m_fValue = fPitch;
    rq.PushBack(request);
  }

  if (!rq.IsEmpty())
  {
    plAudioSystem::GetSingleton()->SendRequests(rq);
  }
}

void plMiniAudioSoundComponent::ReleaseAudioObjects()
{
  m_bPlaying = false;
  m_bWaitingForFinish = false;

  plAudioSystem* pAudioSystem = plAudioSystem::GetSingleton();
  if (pAudioSystem == nullptr || !m_bEntityRegistered)
  {
    m_bEntityRegistered = false;
    m_uiLoadedTriggerId = kInvalidAudioSystemId;
    return;
  }

  plAudioSystemRequestsQueue rq;

  if (m_uiLoadedTriggerId != kInvalidAudioSystemId)
  {
    plAudioSystemRequestUnloadTrigger unload;
    unload.m_uiEntityId = m_uiEntityId;
    unload.m_uiObjectId = m_uiLoadedTriggerId;
    unload.m_uiEventId = m_uiEventId;
    rq.PushBack(unload);
  }

  // unregistering the entity stops whatever still plays on it
  plAudioSystemRequestUnregisterEntity unregister;
  unregister.m_uiEntityId = m_uiEntityId;
  rq.PushBack(unregister);

  pAudioSystem->SendRequests(rq);

  m_bEntityRegistered = false;
  m_uiLoadedTriggerId = kInvalidAudioSystemId;
}

void plMiniAudioSoundComponent::Play()
{
  m_bPaused = false;

  plAudioSystem* pAudioSystem = plAudioSystem::GetSingleton();
  if (pAudioSystem == nullptr || m_bPlaying)
    return;

  bool bLooping = false;
  const plAudioSystemDataID uiTriggerId = PrepareTrigger(bLooping);
  if (uiTriggerId == kInvalidAudioSystemId)
    return;

  EnsureEntityRegistered();
  SendTransformAndMultipliers(true);
  EnsureTriggerLoaded(uiTriggerId);

  plAudioSystemRequestActivateTrigger request;
  request.m_uiEntityId = m_uiEntityId;
  request.m_uiObjectId = uiTriggerId;
  request.m_uiEventId = m_uiEventId;
  request.m_bLooping = bLooping;

  const plComponentHandle hComponent = GetHandle();
  const plAudioSystemDataID uiEventId = m_uiEventId;
  request.m_Callback = [hComponent, uiEventId, bLooping](const plAudioSystemRequestActivateTrigger& e)
  {
    PL_LOCK(s_ComponentRegistryMutex);

    plMiniAudioSoundComponent* pComponent = nullptr;
    if (!s_ComponentRegistry.TryGetValue(hComponent, pComponent) || !pComponent->m_bPlaying || pComponent->m_uiEventId != uiEventId)
      return;

    if (e.m_eStatus.Succeeded())
    {
      pComponent->m_bWaitingForFinish = !bLooping;
    }
    else
    {
      pComponent->m_bPlaying = false;
    }
  };

  m_bPlaying = true;
  m_bWaitingForFinish = false;

  pAudioSystem->SendRequest(request);
}

void plMiniAudioSoundComponent::Pause()
{
  Stop();
}

void plMiniAudioSoundComponent::Stop()
{
  m_bWaitingForFinish = false;

  if (!m_bPlaying)
    return;

  m_bPlaying = false;

  plAudioSystem* pAudioSystem = plAudioSystem::GetSingleton();
  if (pAudioSystem == nullptr || !m_bEntityRegistered || m_uiLoadedTriggerId == kInvalidAudioSystemId)
    return;

  plAudioSystemRequestStopEvent request;
  request.m_uiEntityId = m_uiEntityId;
  request.m_uiTriggerId = m_uiLoadedTriggerId;
  request.m_uiObjectId = m_uiEventId;

  pAudioSystem->SendRequest(request);
}

void plMiniAudioSoundComponent::FadeOut(plTime fadeDuration)
{
  // the event stops with its own fixed fade
  PL_IGNORE_UNUSED(fadeDuration);
  Stop();
}

void plMiniAudioSoundComponent::StartOneShot()
{
  plAudioSystem* pAudioSystem = plAudioSystem::GetSingleton();
  if (pAudioSystem == nullptr)
    return;

  bool bLooping = false;
  const plAudioSystemDataID uiTriggerId = PrepareTrigger(bLooping);
  if (uiTriggerId == kInvalidAudioSystemId)
    return;

  // a transient entity at the current position; PollOneShots() releases it when the sound has ended
  LegacyOneShot oneShot;
  oneShot.m_uiEntityId = AllocateLegacyId();
  oneShot.m_uiEventId = AllocateLegacyId();
  oneShot.m_uiTriggerId = uiTriggerId;

  {
    PL_LOCK(s_OneShotMutex);
    s_OneShots.PushBack(oneShot);
  }

  plAudioSystemRequestsQueue rq;

  plAudioSystemRequestRegisterEntity registerEntity;
  registerEntity.m_uiEntityId = oneShot.m_uiEntityId;
  registerEntity.m_sName = GetOwner()->GetName();
  rq.PushBack(registerEntity);

  plAudioSystemRequestSetEntityTransform transform;
  transform.m_uiEntityId = oneShot.m_uiEntityId;
  transform.m_Transform = ComputeTransform();
  rq.PushBack(transform);

  plAudioSystemRequestSetEventParameter volume;
  volume.m_uiEntityId = oneShot.m_uiEntityId;
  volume.m_sName = plPlasmaAudioReservedParameter::Volume;
  volume.m_fValue = ComputeVolume();
  rq.PushBack(volume);

  plAudioSystemRequestSetEventParameter pitch;
  pitch.m_uiEntityId = oneShot.m_uiEntityId;
  pitch.m_sName = plPlasmaAudioReservedParameter::Pitch;
  pitch.m_fValue = ComputePitch();
  rq.PushBack(pitch);

  plAudioSystemRequestLoadTrigger load;
  load.m_uiEntityId = oneShot.m_uiEntityId;
  load.m_uiObjectId = uiTriggerId;
  load.m_uiEventId = oneShot.m_uiEventId;
  rq.PushBack(load);

  plAudioSystemRequestActivateTrigger activate;
  activate.m_uiEntityId = oneShot.m_uiEntityId;
  activate.m_uiObjectId = uiTriggerId;
  activate.m_uiEventId = oneShot.m_uiEventId;
  activate.m_bLooping = false;

  const plAudioSystemDataID uiEventId = oneShot.m_uiEventId;
  activate.m_Callback = [uiEventId](const plAudioSystemRequestActivateTrigger& e)
  { SetOneShotState(uiEventId, e.m_eStatus.Succeeded() ? LegacyOneShot::State::Playing : LegacyOneShot::State::Done); };
  rq.PushBack(activate);

  pAudioSystem->SendRequests(rq);
}

void plMiniAudioSoundComponent::OnMsgDeleteGameObject(plMsgDeleteGameObject& msg)
{
  plOnComponentFinishedAction2::HandleDeleteObjectMsg(msg, m_OnFinishedAction);
}

void plMiniAudioSoundComponent::Update()
{
  if (m_bWaitingForFinish)
  {
    plAudioSystem* pAudioSystem = plAudioSystem::GetSingleton();
    if (pAudioSystem == nullptr || !pAudioSystem->IsEventPlaying(m_uiLoadedTriggerId, m_uiEventId))
    {
      m_bWaitingForFinish = false;
      m_bPlaying = false;
      SoundFinished();
      return;
    }
  }

  if (m_bPlaying)
  {
    SendTransformAndMultipliers(false);
  }
}

void plMiniAudioSoundComponent::SoundFinished()
{
  plOnComponentFinishedAction2::HandleFinishedAction(this, m_OnFinishedAction);

  if (m_OnFinishedAction == plOnComponentFinishedAction2::Restart)
  {
    Play();
  }
}
