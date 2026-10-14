#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Mixer/AudioMixerResource.h>

#include <Foundation/Utilities/AssetFileHeader.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerResource, 1, plRTTIDefaultAllocator<plAudioMixerResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plAudioMixerResource);

plAudioMixerResource::plAudioMixerResource()
  : plResource(DoUpdate::OnAnyThread, 1)
{
  ModifyMemoryUsage().m_uiMemoryCPU = sizeof(plAudioMixerResource);
}

plAudioMixerResource::~plAudioMixerResource() = default;

plResourceLoadDesc plAudioMixerResource::UnloadData(Unload WhatToUnload)
{
  PL_IGNORE_UNUSED(WhatToUnload);

  m_Desc.m_Buses.Clear();
  m_Desc.m_Vcas.Clear();
  m_Desc.m_Snapshots.Clear();

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Unloaded;

  return res;
}

plResourceLoadDesc plAudioMixerResource::UpdateContent(plStreamReader* pStream)
{
  PL_LOG_BLOCK("plAudioMixerResource::UpdateContent", GetResourceIdOrDescription());

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

  if (m_Desc.Load(*pStream).Failed())
  {
    res.m_State = plResourceState::LoadedResourceMissing;
    return res;
  }

  res.m_State = plResourceState::Loaded;
  return res;
}

void plAudioMixerResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plAudioMixerResource);
  out_NewMemoryUsage.m_uiMemoryCPU += m_Desc.m_Buses.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryCPU += m_Desc.m_Vcas.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryCPU += m_Desc.m_Snapshots.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryGPU = 0;
}

PL_RESOURCE_IMPLEMENT_CREATEABLE(plAudioMixerResource, plAudioMixerResourceDescriptor)
{
  m_Desc = descriptor.m_Desc;

  if (m_Desc.m_Buses.IsEmpty())
    m_Desc.MakeDefault();

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Loaded;

  return res;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Mixer_AudioMixerResource);
