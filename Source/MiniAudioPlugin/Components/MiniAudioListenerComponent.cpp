#include <MiniAudioPlugin/MiniAudioPluginPCH.h>

#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <MiniAudioPlugin/Components/MiniAudioListenerComponent.h>
#include <MiniAudioPlugin/MiniAudioSingleton.h>

plMiniAudioListenerComponentManager::plMiniAudioListenerComponentManager(plWorld* pWorld)
  : plComponentManager(pWorld)
{
}

void plMiniAudioListenerComponentManager::Initialize()
{
  SUPER::Initialize();

  {
    auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plMiniAudioListenerComponentManager::UpdateListeners, this);
    desc.m_Phase = UpdateFunctionDesc::Phase::PostTransform;
    desc.m_bOnlyUpdateWhenSimulating = true;

    this->RegisterUpdateFunction(desc);
  }
}

void plMiniAudioListenerComponentManager::UpdateListeners(const plWorldModule::UpdateContext& context)
{
  for (auto it = this->m_ComponentStorage.GetIterator(context.m_uiFirstComponentIndex, context.m_uiComponentCount); it.IsValid(); ++it)
  {
    ComponentType* pComponent = it;
    if (pComponent->IsActiveAndInitialized())
    {
      pComponent->Update();
    }
  }
}

//////////////////////////////////////////////////////////////////////////

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plMiniAudioListenerComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Audio/MiniAudio"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plMiniAudioListenerComponent::plMiniAudioListenerComponent() = default;
plMiniAudioListenerComponent::~plMiniAudioListenerComponent() = default;

void plMiniAudioListenerComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);

  auto& s = inout_stream.GetStream();
}

void plMiniAudioListenerComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  // const plUInt32 uiVersion = stream.GetComponentTypeVersion(GetStaticRTTI());

  auto& s = inout_stream.GetStream();
}

void plMiniAudioListenerComponent::Update()
{
  const auto pos = GetOwner()->GetGlobalPosition();
  const auto vel = GetOwner()->GetLinearVelocity();
  const auto fwd = (GetOwner()->GetGlobalRotation() * plVec3::MakeAxisX()).GetNormalized();
  const auto up = (GetOwner()->GetGlobalRotation() * plVec3::MakeAxisZ()).GetNormalized();

  plMiniAudioSingleton::GetSingleton()->SetListener(0, pos, fwd, up, vel);
}
