#pragma once

#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <MiniAudioPlugin/MiniAudioPluginDLL.h>

class plMiniAudioListenerComponentManager : public plComponentManager<class plMiniAudioListenerComponent, plBlockStorageType::Compact>
{
public:
  plMiniAudioListenerComponentManager(plWorld* pWorld);

  virtual void Initialize() override;

private:
  void UpdateListeners(const plWorldModule::UpdateContext& context);
};

//////////////////////////////////////////////////////////////////////////

/// \brief Represents the position of the sound listener
class PL_MINIAUDIOPLUGIN_DLL plMiniAudioListenerComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plMiniAudioListenerComponent, plComponent, plMiniAudioListenerComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

  //////////////////////////////////////////////////////////////////////////
  // plMiniAudioListenerComponent

public:
  plMiniAudioListenerComponent();
  ~plMiniAudioListenerComponent();

protected:
  void Update();
};