#pragma once

#include <Core/ResourceManager/Resource.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

class plRandom;

using plMiniAudioSoundResourceHandle = plTypedResourceHandle<class plMiniAudioSoundResource>;

struct PL_PLASMAAUDIOPLUGIN_DLL plMiniAudioSoundResourceDescriptor{
  // empty, these types of resources must be loaded from file
};

class PL_PLASMAAUDIOPLUGIN_DLL plMiniAudioSoundResource : public plResource
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioSoundResource, plResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plMiniAudioSoundResource);
  PL_RESOURCE_DECLARE_CREATEABLE(plMiniAudioSoundResource, plMiniAudioSoundResourceDescriptor);

public:
  plMiniAudioSoundResource();
  ~plMiniAudioSoundResource();

  const plDataBuffer& GetAudioData() const;
  const plDataBuffer& GetAudioData(plRandom& ref_rng) const;

  bool GetLoop() const { return m_bLoop; }
  float GetVolume(plRandom& ref_rng) const;
  float GetPitch(plRandom& ref_rng) const;
  bool GetSpatialize() const { return m_bSpatialize; }
  float GetDopplerFactor() const { return m_fDopplerFactor; }
  float GetMinDistance() const { return m_fMinDistance; }
  float GetMaxDistance() const { return m_fMaxDistance; }
  float GetRolloff() const { return m_fRolloff; }

  float GetMinVolume() const { return m_fMinVolume; }
  float GetMaxVolume() const { return m_fMaxVolume; }
  float GetMinPitch() const { return m_fMinPitch; }
  float GetMaxPitch() const { return m_fMaxPitch; }

  /// \brief Name of the sound group the sound plays in. Empty means the master output.
  const plString& GetSoundGroup() const { return m_sSoundGroup; }

  /// \brief Number of encoded audio files; playback picks one at random.
  plUInt32 GetNumVariants() const { return m_AudioData.GetCount(); }
  const plDataBuffer& GetVariantData(plUInt32 uiIndex) const { return m_AudioData[uiIndex]; }

private:
  virtual plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  virtual plResourceLoadDesc UpdateContent(plStreamReader* pStream) override;
  virtual void UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage) override;

private:
  plHybridArray<plDataBuffer, 1> m_AudioData;

  plString m_sSoundGroup;
  bool m_bLoop = false;
  float m_fMinVolume = 1.0f;
  float m_fMaxVolume = 1.0f;
  float m_fMinDistance = 1.0f;
  float m_fMaxDistance = 10.0f;
  float m_fMinPitch = 1.0f;
  float m_fMaxPitch = 1.0f;
  bool m_bSpatialize = true;
  float m_fDopplerFactor = 1.0f;
  float m_fRolloff = 1.0f;
};
