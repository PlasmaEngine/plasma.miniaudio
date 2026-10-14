#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <AudioSystem/Components/AudioSystemComponent.h>
#include <Core/World/ComponentManager.h>

using plAudioMixerLoaderComponentManager = plComponentManager<class plAudioMixerLoaderComponent, plBlockStorageType::Compact>;

/// \brief Loads the project audio mixer (bus hierarchy, VCAs, snapshots) into the audio system.
///
/// Place one of these in a scene (or in the project's startup scene) and point it at a
/// .plAudioMixerAsset. The mixer is loaded on the audio thread when the component initializes.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioMixerLoaderComponent : public plAudioSystemComponent
{
  PL_DECLARE_COMPONENT_TYPE(plAudioMixerLoaderComponent, plAudioSystemComponent, plAudioMixerLoaderComponentManager);

public:
  plAudioMixerLoaderComponent();
  ~plAudioMixerLoaderComponent() override;

  void Initialize() override;
  void Deinitialize() override;
  void SerializeComponent(plWorldWriter& stream) const override;
  void DeserializeComponent(plWorldReader& stream) override;

  void SetMixerAsset(const char* szAsset);
  const char* GetMixerAsset() const;

  /// \brief Loads the referenced mixer asset into the audio system.
  void LoadMixer();

  /// \brief Unloads the mixer from the audio system.
  void UnloadMixer();

protected:
  void plAudioSystemComponentIsAbstract() override {}

private:
  plString m_sMixerAsset;
  bool m_bLoadOnInit = true;
  bool m_bUnloadOnDeinit = false;
  bool m_bLoaded = false;
};
