#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <PlasmaAudioPlugin/Mixer/AudioMixerData.h>
#include <Core/ResourceManager/Resource.h>

using plAudioMixerResourceHandle = plTypedResourceHandle<class plAudioMixerResource>;

struct PL_PLASMAAUDIOPLUGIN_DLL plAudioMixerResourceDescriptor
{
  plAudioMixerDesc m_Desc;
};

/// \brief Runtime resource for the project mixer (compiled from a .plAudioMixerAsset).
///
/// Holds the serialized plAudioMixerDesc. Loaded by plAudioMixerService on the audio thread
/// and handed to the active plAudioMiddleware via LoadMixer().
class PL_PLASMAAUDIOPLUGIN_DLL plAudioMixerResource : public plResource
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerResource, plResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plAudioMixerResource);
  PL_RESOURCE_DECLARE_CREATEABLE(plAudioMixerResource, plAudioMixerResourceDescriptor);

public:
  plAudioMixerResource();
  ~plAudioMixerResource();

  const plAudioMixerDesc& GetDescriptor() const { return m_Desc; }

private:
  virtual plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  virtual plResourceLoadDesc UpdateContent(plStreamReader* pStream) override;
  virtual void UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage) override;

  plAudioMixerDesc m_Desc;
};
