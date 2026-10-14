#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Components/AudioSnapshotComponent.h>
#include <AudioSystem/Core/AudioSystem.h>
#include <AudioSystem/Core/AudioSystemRequests.h>

#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plAudioSnapshotComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Snapshot", m_sSnapshotName)->AddAttributes(new plDynamicStringEnumAttribute("AudioSystem.Snapshot")),
    PL_MEMBER_PROPERTY("Intensity", m_fIntensity)->AddAttributes(new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("ActivateOnSimulationStart", m_bActivateOnSimulationStart),
    PL_MEMBER_PROPERTY("DeactivateOnDeactivated", m_bDeactivateOnDeactivated),
  }
  PL_END_PROPERTIES;

  PL_BEGIN_FUNCTIONS
  {
    PL_SCRIPT_FUNCTION_PROPERTY(Activate),
    PL_SCRIPT_FUNCTION_PROPERTY(Deactivate),
    PL_SCRIPT_FUNCTION_PROPERTY(SetIntensity, In, "Intensity"),
  }
  PL_END_FUNCTIONS;
}
PL_END_COMPONENT_TYPE;
// clang-format on

plAudioSnapshotComponent::plAudioSnapshotComponent() = default;
plAudioSnapshotComponent::~plAudioSnapshotComponent() = default;

void plAudioSnapshotComponent::OnSimulationStarted()
{
  SUPER::OnSimulationStarted();

  if (m_bActivateOnSimulationStart)
  {
    Activate();
  }
}

void plAudioSnapshotComponent::OnDeactivated()
{
  if (m_bDeactivateOnDeactivated)
  {
    Deactivate();
  }

  SUPER::OnDeactivated();
}

void plAudioSnapshotComponent::SerializeComponent(plWorldWriter& stream) const
{
  SUPER::SerializeComponent(stream);

  auto& s = stream.GetStream();
  s << m_sSnapshotName;
  s << m_fIntensity;
  s << m_bActivateOnSimulationStart;
  s << m_bDeactivateOnDeactivated;
}

void plAudioSnapshotComponent::DeserializeComponent(plWorldReader& stream)
{
  SUPER::DeserializeComponent(stream);

  auto& s = stream.GetStream();
  s >> m_sSnapshotName;
  s >> m_fIntensity;
  s >> m_bActivateOnSimulationStart;
  s >> m_bDeactivateOnDeactivated;
}

void plAudioSnapshotComponent::Activate()
{
  SetIntensity(m_fIntensity);
}

void plAudioSnapshotComponent::Deactivate()
{
  SetIntensity(0.0f);
}

void plAudioSnapshotComponent::SetIntensity(float fIntensity)
{
  if (m_sSnapshotName.IsEmpty())
    return;

  plAudioSystem* pAudioSystem = plAudioSystem::GetSingleton();
  if (pAudioSystem == nullptr)
    return;

  plAudioSystemRequestSetMixerControl request;
  request.m_uiOp = plAudioMixerControlOp::SetSnapshotIntensity;
  request.m_sTarget = m_sSnapshotName;
  request.m_fValue = plMath::Clamp(fIntensity, 0.0f, 1.0f);

  pAudioSystem->SendRequest(request);
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Components_AudioSnapshotComponent);
