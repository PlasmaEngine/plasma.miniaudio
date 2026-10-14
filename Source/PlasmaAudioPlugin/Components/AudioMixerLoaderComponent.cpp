#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Components/AudioMixerLoaderComponent.h>
#include <AudioSystem/Core/AudioSystem.h>
#include <AudioSystem/Core/AudioSystemRequests.h>

#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plAudioMixerLoaderComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ACCESSOR_PROPERTY("Mixer", GetMixerAsset, SetMixerAsset)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Audio_Mixer")),
    PL_MEMBER_PROPERTY("LoadOnInit", m_bLoadOnInit),
    PL_MEMBER_PROPERTY("UnloadOnDeinit", m_bUnloadOnDeinit),
  }
  PL_END_PROPERTIES;

  PL_BEGIN_FUNCTIONS
  {
    PL_SCRIPT_FUNCTION_PROPERTY(LoadMixer),
    PL_SCRIPT_FUNCTION_PROPERTY(UnloadMixer),
  }
  PL_END_FUNCTIONS;
}
PL_END_COMPONENT_TYPE;
// clang-format on

plAudioMixerLoaderComponent::plAudioMixerLoaderComponent() = default;
plAudioMixerLoaderComponent::~plAudioMixerLoaderComponent() = default;

void plAudioMixerLoaderComponent::Initialize()
{
  SUPER::Initialize();

  if (m_bLoadOnInit)
  {
    LoadMixer();
  }
}

void plAudioMixerLoaderComponent::Deinitialize()
{
  if (m_bUnloadOnDeinit)
  {
    UnloadMixer();
  }

  SUPER::Deinitialize();
}

void plAudioMixerLoaderComponent::SerializeComponent(plWorldWriter& stream) const
{
  SUPER::SerializeComponent(stream);

  auto& s = stream.GetStream();
  s << m_sMixerAsset;
  s << m_bLoadOnInit;
  s << m_bUnloadOnDeinit;
}

void plAudioMixerLoaderComponent::DeserializeComponent(plWorldReader& stream)
{
  SUPER::DeserializeComponent(stream);

  auto& s = stream.GetStream();
  s >> m_sMixerAsset;
  s >> m_bLoadOnInit;
  s >> m_bUnloadOnDeinit;
}

void plAudioMixerLoaderComponent::SetMixerAsset(const char* szAsset)
{
  m_sMixerAsset = szAsset;
}

const char* plAudioMixerLoaderComponent::GetMixerAsset() const
{
  return m_sMixerAsset;
}

void plAudioMixerLoaderComponent::LoadMixer()
{
  if (m_sMixerAsset.IsEmpty())
    return;

  plAudioSystem* pAudioSystem = plAudioSystem::GetSingleton();
  if (pAudioSystem == nullptr)
    return;

  plAudioSystemRequestSetMixerControl request;
  request.m_uiOp = plAudioMixerControlOp::LoadMixer;
  request.m_sTarget = m_sMixerAsset;

  pAudioSystem->SendRequest(request);
  m_bLoaded = true;
}

void plAudioMixerLoaderComponent::UnloadMixer()
{
  if (!m_bLoaded)
    return;

  m_bLoaded = false;

  plAudioSystem* pAudioSystem = plAudioSystem::GetSingleton();
  if (pAudioSystem == nullptr)
    return;

  plAudioSystemRequestSetMixerControl request;
  request.m_uiOp = plAudioMixerControlOp::UnloadMixer;

  pAudioSystem->SendRequest(request);
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Components_AudioMixerLoaderComponent);
