#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Core/ResourceManager/Resource.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <Foundation/Threading/Mutex.h>
#include <PlasmaAudioPlugin/Clips/AudioClipDesc.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>

class plAudioStreamService;
struct plAudioClipVoiceSource;

using plAudioClipResourceHandle = plTypedResourceHandle<class plAudioClipResource>;

/// \brief Creates an audio clip resource from memory (tests, procedural audio).
///
/// m_Header.m_Languages may be empty, a single default language entry is then assumed. For DecompressOnLoad either
/// m_Pcm (interleaved s16) or an encoded m_Payload can be given, the payload is decoded on creation. For
/// CompressedInMemory m_Payload is required. Streamed clips cannot be created from memory.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioClipResourceDescriptor
{
  plAudioClipHeader m_Header;
  plDynamicArray<plInt16> m_Pcm;
  plDynamicArray<plUInt8> m_Payload;
};

/// \brief Runtime resource of an audio clip (.plBinAudioClip): the samples of one sound in one of three load modes.
///
/// - DecompressOnLoad: the payload is decoded to s16 PCM while loading (on the resource loader thread).
/// - CompressedInMemory: the encoded payload stays in memory, every voice decodes it.
/// - Streamed: only the start and loop priming data is in memory; the stream service reads the payload from the clip
///   file at GetPayloadFileOffset(), with plOSFile from GetOsFilePath() for a plain file on disk, otherwise through
///   the file system (plFileReader on the resource id, e.g. inside a package archive).
///
/// A clip file can hold several languages. plAudioClipResourceLoader picks the section of
/// plAudioClipResourceLoader::GetLanguage() (falling back to the default language) and only that section is loaded.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioClipResource : public plResource
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioClipResource, plResource);
  PL_RESOURCE_DECLARE_COMMON_CODE(plAudioClipResource);
  PL_RESOURCE_DECLARE_CREATEABLE(plAudioClipResource, plAudioClipResourceDescriptor);

public:
  plAudioClipResource();
  ~plAudioClipResource();

  /// \brief Sets up out_source to play this clip from the start.
  ///
  /// pStreams is needed for Streamed clips; without it (or while streaming is unavailable) Streamed clips fail.
  /// The resource must stay acquired while the voice source is in use.
  plResult CreateVoiceSource(plAudioClipVoiceSource& out_source, plAudioStreamService* pStreams) const;

  const plAudioClipHeader& GetHeader() const { return m_Header; }
  plAudioClipCodec::Enum GetCodec() const { return m_Header.m_eCodec; }
  plAudioClipLoadMode::Enum GetLoadMode() const { return m_Header.m_eLoadMode; }
  plUInt64 GetFrames() const { return m_Header.m_uiFrames; }
  plUInt32 GetSampleRate() const { return m_Header.m_uiSampleRate; }
  plUInt8 GetChannels() const { return m_Header.m_uiChannels; }
  plTime GetDuration() const;

  /// \brief Returns false when the clip has no loop; otherwise the loop frame range [out_uiStart, out_uiEnd).
  bool GetLoopPoints(plUInt64& out_uiStart, plUInt64& out_uiEnd) const;

  /// \brief Waveform overview: GetPeakBuckets() * GetChannels() (min, max) pairs, bucket major, scaled by 127.
  plArrayPtr<const plInt8> GetPeaks() const { return m_Header.m_Peaks; }
  plUInt32 GetPeakBuckets() const { return m_Header.m_uiPeakBuckets; }

  /// \brief Decoded PCM (DecompressOnLoad only).
  plArrayPtr<const plInt16> GetPcm() const { return m_Pcm; }

  /// \brief Encoded payload (CompressedInMemory only).
  plArrayPtr<const plUInt8> GetPayload() const { return m_Payload; }

  /// \brief First payload bytes of the active language (Streamed only).
  plArrayPtr<const plUInt8> GetPriming() const { return m_Priming; }

  /// \brief Payload bytes from GetLoopPrimingOffset() on (Streamed looping clips only).
  plArrayPtr<const plUInt8> GetLoopPriming() const { return m_LoopPriming; }
  plUInt64 GetLoopPrimingOffset() const { return m_Header.GetLoopPrimingOffset(); }

  /// \brief Absolute path of the clip file when it is a plain file on disk, empty otherwise (Streamed).
  const plString& GetOsFilePath() const { return m_sOsFilePath; }

  /// \brief Range of the clip file the payload of the active language is read from (Streamed).
  plUInt64 GetPayloadFileOffset() const { return m_uiPayloadFileOffset; }
  plUInt64 GetPayloadBytes() const { return m_uiPayloadBytes; }

  /// \brief The language whose section was loaded ("" = default).
  const plString& GetActiveLanguage() const { return m_sActiveLanguage; }

  /// \brief plAudioClipResourceLoader::GetLanguageGeneration() of the language the clip was loaded in (0 for clips
  /// created from memory).
  plUInt32 GetLanguageGeneration() const { return m_uiLanguageGeneration; }

  /// \brief Heap bytes of the sample data (decoded PCM, payload, priming data) of all loaded clip resources. Any thread.
  static plUInt64 GetTotalResidentBytes() { return static_cast<plUInt64>(static_cast<plInt64>(s_iResidentBytes)); }

  /// \brief Replaces out_clips with an entry per loaded clip resource: name, load mode, codec, resident bytes and, with
  /// pStreams, the open streams of streamed clips. Any thread.
  ///
  /// Clips are listed from their own registry of loaded clips, not through the resource manager: a clip enters it when
  /// its content finished loading and leaves it before its data is cleared or it is destroyed.
  static void GatherLoadedClips(plDynamicArray<plAudioClipMemorySnapshotEntry>& out_clips, const plAudioStreamService* pStreams);

private:
  /// Replaces this resource's share of s_iResidentBytes with the current size of its sample arrays and adds the clip to
  /// the registry of loaded clips once its content is loaded.
  void RecordResidentBytes();

  /// Removes the clip from the registry of loaded clips.
  void UnlinkLoaded();

  static plAtomicInteger64 s_iResidentBytes;
  plUInt64 m_uiResidentBytes = 0; ///< this resource's share of s_iResidentBytes

  /// registry of loaded clips: an intrusive list, guarded by s_LoadedMutex
  static plMutex s_LoadedMutex;
  static plAudioClipResource* s_pFirstLoaded;
  plAudioClipResource* m_pPrevLoaded = nullptr;
  plAudioClipResource* m_pNextLoaded = nullptr;
  bool m_bLinkedLoaded = false;
  bool m_bContentLoaded = false; ///< set by a successful load, cleared by ClearData

  virtual plResourceLoadDesc UnloadData(Unload WhatToUnload) override;
  virtual plResourceLoadDesc UpdateContent(plStreamReader* pStream) override;
  virtual void UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage) override;

  plResult DecodePayloadToPcm();
  void ClearData();

  plAudioClipHeader m_Header;
  plDynamicArray<plInt16> m_Pcm;
  plDynamicArray<plUInt8> m_Payload;
  plDynamicArray<plUInt8> m_Priming;
  plDynamicArray<plUInt8> m_LoopPriming;
  plUInt64 m_uiPayloadFileOffset = 0;
  plUInt64 m_uiPayloadBytes = 0;
  plString m_sOsFilePath;
  plString m_sActiveLanguage;
  plUInt32 m_uiLanguageGeneration = 0;
};
