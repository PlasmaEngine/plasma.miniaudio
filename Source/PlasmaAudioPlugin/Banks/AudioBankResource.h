#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Core/ResourceManager/Resource.h>
#include <Core/ResourceManager/ResourceTypeLoader.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Strings/String.h>

using plAudioBankResourceHandle = plTypedResourceHandle<class plAudioBankResource>;

/// \brief Memory limits of the loaded audio content per category, in bytes. 0 is no limit.
///
/// The categories follow the clips' load modes: decoded (DecompressOnLoad), compressed (CompressedInMemory) and
/// streaming (the priming data of Streamed clips plus the stream rings).
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioMemoryBudget
{
  plUInt64 m_uiDecodedBytes = 0;
  plUInt64 m_uiCompressedBytes = 0;
  plUInt64 m_uiStreamingBytes = 0;
  plUInt64 m_uiTotalBytes = 0;

  bool IsSet() const { return m_uiDecodedBytes != 0 || m_uiCompressedBytes != 0 || m_uiStreamingBytes != 0 || m_uiTotalBytes != 0; }

  bool operator==(const plAudioMemoryBudget& rhs) const
  {
    return m_uiDecodedBytes == rhs.m_uiDecodedBytes && m_uiCompressedBytes == rhs.m_uiCompressedBytes && m_uiStreamingBytes == rhs.m_uiStreamingBytes && m_uiTotalBytes == rhs.m_uiTotalBytes;
  }
};

/// \brief Memory the loaded audio content takes, in the categories of plAudioMemoryBudget.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioMemoryUsage
{
  plUInt64 m_uiDecodedBytes = 0;
  plUInt64 m_uiCompressedBytes = 0;
  plUInt64 m_uiPrimingBytes = 0; ///< priming data of streamed clips
  plUInt64 m_uiRingBytes = 0;    ///< rings of the open and pooled streams

  plUInt64 GetStreamingBytes() const { return m_uiPrimingBytes + m_uiRingBytes; }
  plUInt64 GetTotalBytes() const { return m_uiDecodedBytes + m_uiCompressedBytes + GetStreamingBytes(); }

  /// \brief The categories over their limit, as text ("decoded 12.0 MB of 8.0 MB, total ..."). Empty when none is.
  void DescribeExcess(const plAudioMemoryBudget& budget, plStringBuilder& out_sText) const;
};

/// \brief An event or clip whose built file a packed bank carries.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioBankPackedEntry
{
  plString m_sResourceId;
  bool m_bClip = false;
  plUInt64 m_uiOffset = 0; ///< from the start of the packed data
  plUInt64 m_uiBytes = 0;

  bool operator==(const plAudioBankPackedEntry& rhs) const
  {
    return m_sResourceId == rhs.m_sResourceId && m_bClip == rhs.m_bClip && m_uiOffset == rhs.m_uiOffset && m_uiBytes == rhs.m_uiBytes;
  }
};

/// \brief The content list of an audio bank: resource ids (asset GUID strings) of the events and clips it keeps loaded,
/// the memory budget of the platform it was built for and, for a packed bank, where its packed files lie.
///
/// File layout of a .plBinAudioBank: plAssetFileHeader, then Save() output, then the packed data: the built files of
/// the packed entries one after the other.
///
/// Save() output: version (plTypeVersion, currently 2), u32 event count, event ids (plString), u32 clip count, clip ids
/// (plString), the budget (4 x u64), u32 packed count, per packed entry: id (plString), u8 clip, u64 offset, u64 bytes.
/// Version 1 ends after the clip ids.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioBankDesc
{
  plDynamicArray<plString> m_Events;
  plDynamicArray<plString> m_Clips;
  plAudioMemoryBudget m_Budget;
  plDynamicArray<plAudioBankPackedEntry> m_Packed;

  plResult Save(plStreamWriter& inout_stream) const;
  plResult Load(plStreamReader& inout_stream);
};

struct PL_PLASMAAUDIOPLUGIN_DLL plAudioBankResourceDescriptor
{
  plAudioBankDesc m_Desc;
};

/// \brief Runtime resource of an audio bank (.plBinAudioBank).
///
/// Holds the content list. The middleware loads a bank by its resource id (plAudioSystemStringBankData::m_sBankName)
/// and, once this resource is loaded, loads every listed event and clip and holds their handles until the bank is unloaded.
/// The events and clips of a packed bank are read from the bank file (see plAudioPackedContent).
class PL_PLASMAAUDIOPLUGIN_DLL plAudioBankResource : public plResource
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioBankResource, plResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plAudioBankResource);
  PL_RESOURCE_DECLARE_CREATEABLE(plAudioBankResource, plAudioBankResourceDescriptor);

public:
  plAudioBankResource();
  ~plAudioBankResource();

  const plAudioBankDesc& GetDescriptor() const { return m_Desc; }

  /// \brief The bank file the packed entries are read from, empty for a bank created from a descriptor.
  const plString& GetFilePath() const { return m_sFilePath; }

  /// \brief File offset of the packed data.
  plUInt64 GetPackedDataOffset() const { return m_uiPackedDataOffset; }

private:
  virtual plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  virtual plResourceLoadDesc UpdateContent(plStreamReader* pStream) override;
  virtual void UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage) override;

  plAudioBankDesc m_Desc;
  plString m_sFilePath;
  plUInt64 m_uiPackedDataOffset = 0;
};

/// \brief Reads a .plBinAudioBank up to its packed data, which stays on disk.
///
/// The stream handed to plAudioBankResource::UpdateContent is: string filePath, u64 packedDataOffset, plAssetFileHeader,
/// the desc. filePath is the resource id the file was opened with, so the packed files are read through the file system.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioBankResourceLoader : public plResourceLoaderFromFile
{
public:
  virtual plResourceLoadData OpenDataStream(const plResource* pResource) override;
  virtual void CloseDataStream(const plResource* pResource, const plResourceLoadData& loaderData) override;
};
