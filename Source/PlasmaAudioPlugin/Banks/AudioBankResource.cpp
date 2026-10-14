#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/Utilities/AssetFileHeader.h>
#include <PlasmaAudioPlugin/Banks/AudioBankResource.h>

static constexpr plTypeVersion kAudioBankDescVersion = 1;

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioBankResource, 1, plRTTIDefaultAllocator<plAudioBankResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plAudioBankResource);

plResult plAudioBankDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream.WriteVersion(kAudioBankDescVersion);

  inout_stream << m_Events.GetCount();
  for (const plString& sEvent : m_Events)
    inout_stream << sEvent;

  inout_stream << m_Clips.GetCount();
  for (const plString& sClip : m_Clips)
    inout_stream << sClip;

  return PL_SUCCESS;
}

plResult plAudioBankDesc::Load(plStreamReader& inout_stream)
{
  const plTypeVersion uiVersion = inout_stream.ReadVersion(kAudioBankDescVersion);
  PL_IGNORE_UNUSED(uiVersion);

  plUInt32 uiCount = 0;

  inout_stream >> uiCount;
  m_Events.SetCount(uiCount);
  for (plString& sEvent : m_Events)
    inout_stream >> sEvent;

  inout_stream >> uiCount;
  m_Clips.SetCount(uiCount);
  for (plString& sClip : m_Clips)
    inout_stream >> sClip;

  return PL_SUCCESS;
}

plAudioBankResource::plAudioBankResource()
  : plResource(DoUpdate::OnAnyThread, 1)
{
  ModifyMemoryUsage().m_uiMemoryCPU = sizeof(plAudioBankResource);
}

plAudioBankResource::~plAudioBankResource() = default;

plResourceLoadDesc plAudioBankResource::UnloadData(Unload WhatToUnload)
{
  PL_IGNORE_UNUSED(WhatToUnload);

  m_Desc = plAudioBankDesc();

  ModifyMemoryUsage().m_uiMemoryCPU = sizeof(plAudioBankResource);

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Unloaded;
  return res;
}

plResourceLoadDesc plAudioBankResource::UpdateContent(plStreamReader* pStream)
{
  PL_LOG_BLOCK("plAudioBankResource::UpdateContent", GetResourceIdOrDescription());

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::LoadedResourceMissing;

  if (pStream == nullptr)
    return res;

  // the standard file reader writes the absolute file path into the stream
  plString sAbsFilePath;
  (*pStream) >> sAbsFilePath;

  plAssetFileHeader assetHeader;
  if (assetHeader.Read(*pStream).Failed())
    return res;

  m_Desc = plAudioBankDesc();
  if (m_Desc.Load(*pStream).Failed())
    return res;

  res.m_State = plResourceState::Loaded;
  return res;
}

void plAudioBankResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plAudioBankResource);
  out_NewMemoryUsage.m_uiMemoryCPU += m_Desc.m_Events.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryCPU += m_Desc.m_Clips.GetHeapMemoryUsage();
  out_NewMemoryUsage.m_uiMemoryGPU = 0;
}

PL_RESOURCE_IMPLEMENT_CREATEABLE(plAudioBankResource, plAudioBankResourceDescriptor)
{
  m_Desc = std::move(descriptor.m_Desc);

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Loaded;
  return res;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Banks_AudioBankResource);
