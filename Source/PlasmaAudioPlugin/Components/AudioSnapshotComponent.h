#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <AudioSystem/Components/AudioSystemComponent.h>
#include <Core/World/ComponentManager.h>

using plAudioSnapshotComponentManager = plComponentManager<class plAudioSnapshotComponent, plBlockStorageType::Compact>;

/// \brief Activates a mixer snapshot (with its authored fade times) while the component is active.
///
/// Typical use: attach next to a trigger volume — entering activates the "Cave" snapshot,
/// leaving fades it back out. The snapshot must exist in the loaded audio mixer asset.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioSnapshotComponent : public plAudioSystemComponent
{
  PL_DECLARE_COMPONENT_TYPE(plAudioSnapshotComponent, plAudioSystemComponent, plAudioSnapshotComponentManager);

public:
  plAudioSnapshotComponent();
  ~plAudioSnapshotComponent() override;

  void OnSimulationStarted() override;
  void OnDeactivated() override;
  void SerializeComponent(plWorldWriter& stream) const override;
  void DeserializeComponent(plWorldReader& stream) override;

  /// \brief Blends the snapshot in to the configured intensity.
  void Activate();

  /// \brief Blends the snapshot out.
  void Deactivate();

  /// \brief Blends the snapshot to a specific intensity (0..1).
  void SetIntensity(float fIntensity);

protected:
  void plAudioSystemComponentIsAbstract() override {}

private:
  plString m_sSnapshotName;
  float m_fIntensity = 1.0f;
  bool m_bActivateOnSimulationStart = true;
  bool m_bDeactivateOnDeactivated = true;
};
