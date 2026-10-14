#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/Math/Random.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <AudioSystem/Core/AudioSystem.h>
#include <AudioSystem/Core/AudioSystemAllocator.h>
#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioPlugin/Resources/MiniAudioSoundResource.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioSoundResource, 1, plRTTIDefaultAllocator<plMiniAudioSoundResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plMiniAudioSoundResource);

plMiniAudioSoundResource::plMiniAudioSoundResource()
  : plResource(DoUpdate::OnAnyThread, 1)
{
  ModifyMemoryUsage().m_uiMemoryCPU = sizeof(plMiniAudioSoundResource);
}

plMiniAudioSoundResource::~plMiniAudioSoundResource() = default;

const plDataBuffer& plMiniAudioSoundResource::GetAudioData() const
{
  return m_AudioData[0];
}

const plDataBuffer& plMiniAudioSoundResource::GetAudioData(plRandom& ref_rng) const
{
  return m_AudioData[ref_rng.UInt32Index(m_AudioData.GetCount())];
}

float plMiniAudioSoundResource::GetVolume(plRandom& ref_rng) const
{
  if (m_fMinVolume < m_fMaxVolume)
    return ref_rng.FloatMinMax(m_fMinVolume, m_fMaxVolume);

  return m_fMinVolume;
}

float plMiniAudioSoundResource::GetPitch(plRandom& ref_rng) const
{
  if (m_fMinPitch < m_fMaxPitch)
    return ref_rng.FloatMinMax(m_fMinPitch, m_fMaxPitch);

  return m_fMinPitch;
}

plResourceLoadDesc plMiniAudioSoundResource::UnloadData(Unload WhatToUnload)
{
  m_AudioData.Clear();
  m_AudioData.Compact();

  ModifyMemoryUsage().m_uiMemoryCPU = sizeof(plMiniAudioSoundResource);

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Unloaded;

  return res;
}

plResourceLoadDesc plMiniAudioSoundResource::UpdateContent(plStreamReader* pStream)
{
  PL_LOG_BLOCK("plMiniAudioSoundResource::UpdateContent", GetResourceIdOrDescription());

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;

  if (pStream == nullptr)
  {
    res.m_State = plResourceState::LoadedResourceMissing;
    return res;
  }

  // the standard file reader writes the absolute file path into the stream
  plString sAbsFilePath;
  (*pStream) >> sAbsFilePath;

  // skip the asset file header at the start of the file
  plAssetFileHeader AssetHash;
  AssetHash.Read(*pStream).IgnoreResult();

  plUInt8 uiVersion = 0;
  *pStream >> uiVersion;

  *pStream >> m_bLoop;
  *pStream >> m_fMinVolume;
  *pStream >> m_fMaxVolume;
  *pStream >> m_fMinPitch;
  *pStream >> m_fMaxPitch;
  *pStream >> m_bSpatialize;
  *pStream >> m_fMinDistance;
  *pStream >> m_fMaxDistance;
  *pStream >> m_fRolloff;
  *pStream >> m_fDopplerFactor;

  if (m_fMinVolume > m_fMaxVolume)
    plMath::Swap(m_fMinVolume, m_fMaxVolume);

  if (m_fMinDistance > m_fMaxDistance)
    plMath::Swap(m_fMinDistance, m_fMaxDistance);

  if (m_fMinPitch > m_fMaxPitch)
    plMath::Swap(m_fMinPitch, m_fMaxPitch);

  plUInt32 uiNumFiles = 0;
  *pStream >> uiNumFiles;

  m_AudioData.SetCount(uiNumFiles);

  for (plUInt32 i = 0; i < uiNumFiles; ++i)
  {
    plUInt32 uiFileSize = 0;
    *pStream >> uiFileSize;

    m_AudioData[i].SetCountUninitialized(uiFileSize);
    if (pStream->ReadBytes(m_AudioData[i].GetData(), uiFileSize) != uiFileSize)
    {
      res.m_State = plResourceState::LoadedResourceMissing;
      return res;
    }
  }

  if (uiVersion >= 2)
  {
    *pStream >> m_sSoundGroup;
  }

  if (auto* pAudioSystem = plAudioSystem::GetSingleton())
  {
    const plAudioSystemDataID uiBankId = plHashHelper<plStringView>::Hash("MiniAudio.DefaultBank");
    const plAudioSystemDataID uiTriggerId = plHashHelper<plStringView>::Hash(GetResourceID());
    pAudioSystem->RegisterSoundBank(uiBankId, PL_AUDIOSYSTEM_NEW(plPlasmaAudioBankData, uiBankId, "MiniAudio.DefaultBank"));
    pAudioSystem->RegisterTrigger(uiTriggerId, PL_AUDIOSYSTEM_NEW(plPlasmaAudioTriggerData, uiTriggerId, GetResourceID()));
  }

  res.m_State = plResourceState::Loaded;

  return res;
}

void plMiniAudioSoundResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plMiniAudioSoundResource);
  out_NewMemoryUsage.m_uiMemoryCPU += m_AudioData.GetHeapMemoryUsage();

  for (const auto& data : m_AudioData)
  {
    out_NewMemoryUsage.m_uiMemoryCPU += data.GetHeapMemoryUsage();
  }

  out_NewMemoryUsage.m_uiMemoryGPU = 0;
}

PL_RESOURCE_IMPLEMENT_CREATEABLE(plMiniAudioSoundResource, plMiniAudioSoundResourceDescriptor)
{
  // have to create one 'missing' resource
  // PL_REPORT_FAILURE("This resource type does not support creating data.");

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Loaded;

  return res;
}
