#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Core/ResourceManager/Resource.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Strings/String.h>

using plAudioBankResourceHandle = plTypedResourceHandle<class plAudioBankResource>;

/// \brief The content list of an audio bank: resource ids (asset GUID strings) of the events and clips it keeps loaded.
///
/// File layout of a .plBinAudioBank: plAssetFileHeader, then Save() output:
///   version (plTypeVersion, currently 1), u32 event count, event ids (plString), u32 clip count, clip ids (plString)
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioBankDesc
{
  plDynamicArray<plString> m_Events;
  plDynamicArray<plString> m_Clips;

  plResult Save(plStreamWriter& inout_stream) const;
  plResult Load(plStreamReader& inout_stream);
};

struct PL_PLASMAAUDIOPLUGIN_DLL plAudioBankResourceDescriptor
{
  plAudioBankDesc m_Desc;
};

/// \brief Runtime resource of an audio bank (.plBinAudioBank).
///
/// Only holds the content list. The middleware loads a bank by its resource id (plAudioSystemStringBankData::m_sBankName)
/// and, once this resource is loaded, loads every listed event and clip and holds their handles until the bank is unloaded.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioBankResource : public plResource
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioBankResource, plResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plAudioBankResource);
  PL_RESOURCE_DECLARE_CREATEABLE(plAudioBankResource, plAudioBankResourceDescriptor);

public:
  plAudioBankResource();
  ~plAudioBankResource();

  const plAudioBankDesc& GetDescriptor() const { return m_Desc; }

private:
  virtual plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  virtual plResourceLoadDesc UpdateContent(plStreamReader* pStream) override;
  virtual void UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage) override;

  plAudioBankDesc m_Desc;
};
