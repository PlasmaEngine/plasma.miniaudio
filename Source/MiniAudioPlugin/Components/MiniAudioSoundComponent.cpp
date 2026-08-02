#include <MiniAudioPlugin/MiniAudioPluginPCH.h>

#include <Core/Messages/DeleteObjectMessage.h>
#include <Core/ResourceManager/Implementation/ResourceHandleReflection.h>
#include <Core/World/GameObject.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <MiniAudioPlugin/Components/MiniAudioSoundComponent.h>
#include <MiniAudioPlugin/MiniAudioSingleton.h>
#include <MiniAudioPlugin/Resources/MiniAudioSoundResource.h>

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

  plMiniAudioSingleton::GetSingleton()->StopWorldSounds(GetWorld());
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

void plMiniAudioSoundComponent::OnSimulationStarted()
{
  if (!m_bPaused)
  {
    Play();
  }
}

void plMiniAudioSoundComponent::OnDeactivated()
{
  if (m_pInstance)
  {
    plMiniAudioSingleton* pMA = plMiniAudioSingleton::GetSingleton();

    // fade it out over a short period
    pMA->DetachAndFadeOutSoundInstance(m_pInstance, plTime::Milliseconds(500));
  }
}

void plMiniAudioSoundComponent::Play()
{
  if (!m_hSound.IsValid())
    return;

  if (m_pInstance == nullptr)
  {
    plResourceLock<plMiniAudioSoundResource> pResource(m_hSound, plResourceAcquireMode::BlockTillLoaded_NeverFail);

    if (pResource.GetAcquireResult() != plResourceAcquireResult::Final)
      return;

    plRandom& rng = GetWorld()->GetRandomNumberGenerator();
    m_pInstance = pResource->InstantiateSound(&rng, GetWorld(), GetHandle());

    m_fResourceVolume = pResource->GetVolume(rng);
    m_fResourcePitch = pResource->GetPitch(rng);

    Update();
  }

  PL_MA_CHECK(ma_sound_start(&m_pInstance->m_Sound));
  m_bPaused = false;
}

void plMiniAudioSoundComponent::Pause()
{
  if (m_pInstance)
  {
    PL_MA_CHECK(ma_sound_stop(&m_pInstance->m_Sound));
  }
}

void plMiniAudioSoundComponent::Stop()
{
  if (m_pInstance == nullptr)
    return;

  // just free the sound, this will stop it right away
  plMiniAudioSingleton* pMA = plMiniAudioSingleton::GetSingleton();
  pMA->FreeSoundInstance(m_pInstance);
}

void plMiniAudioSoundComponent::FadeOut(plTime fadeDuration)
{
  if (m_pInstance == nullptr)
    return;

  plMiniAudioSingleton* pMA = plMiniAudioSingleton::GetSingleton();
  pMA->DetachAndFadeOutSoundInstance(m_pInstance, fadeDuration);
}

void plMiniAudioSoundComponent::StartOneShot()
{
  if (!m_hSound.IsValid())
    return;

  plResourceLock<plMiniAudioSoundResource> pResource(m_hSound, plResourceAcquireMode::BlockTillLoaded_NeverFail);

  if (pResource.GetAcquireResult() != plResourceAcquireResult::Final)
    return;

  plRandom& rng = GetWorld()->GetRandomNumberGenerator();
  auto pInstance = pResource->InstantiateSound(&rng, GetWorld(), {});

  const float fResourceVolume = pResource->GetVolume(rng);
  const float fResourcePitch = pResource->GetPitch(rng);

  UpdateParameters(pInstance, m_fComponentVolume * fResourceVolume, m_fPitch * fResourcePitch);

  // the sound will play until it ends and then get cleaned up automatically
  PL_MA_CHECK(ma_sound_start(&pInstance->m_Sound));
}

void plMiniAudioSoundComponent::OnMsgDeleteGameObject(plMsgDeleteGameObject& msg)
{
  plOnComponentFinishedAction2::HandleDeleteObjectMsg(msg, m_OnFinishedAction);
}

void plMiniAudioSoundComponent::Update()
{
  if (m_pInstance)
  {
    UpdateParameters(m_pInstance, m_fComponentVolume * m_fResourceVolume, m_fPitch * m_fResourcePitch);
  }
}

void plMiniAudioSoundComponent::UpdateParameters(plMiniAudioSoundInstance* pInstance, float fVolume, float fPitch) const
{
  const plVec3 pos = GetOwner()->GetGlobalPosition();

  ma_sound_set_position(&pInstance->m_Sound, pos.x, pos.y, pos.z);

  if (GetNoGlobalPitch())
  {
    ma_sound_set_pitch(&pInstance->m_Sound, fPitch);
  }
  else
  {
    ma_sound_set_pitch(&pInstance->m_Sound, fPitch * (float)GetWorld()->GetClock().GetSpeed());
  }

  ma_sound_set_volume(&pInstance->m_Sound, fVolume);

  // no need to set the direction, we currently don't support directional sounds
  // const plVec3 dir = GetOwner()->GetGlobalDirForwards();
  // ma_sound_set_direction(&m_pInstance->m_Sound, dir.x, dir.y, dir.z);
}

void plMiniAudioSoundComponent::SoundFinished()
{
  // reset used sound
  m_pInstance = nullptr;

  // TODO MiniAudio: send event
  // plMsgFmodSoundFinished msg;
  // m_SoundFinishedEventSender.SendEventMessage(msg, this, GetOwner());

  plOnComponentFinishedAction2::HandleFinishedAction(this, m_OnFinishedAction);

  if (m_OnFinishedAction == plOnComponentFinishedAction2::Restart)
  {
    Play();
  }
}
