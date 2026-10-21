#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Core/ResourceManager/ResourceTypeLoader.h>
#include <Foundation/Strings/String.h>

/// \brief Loads the language section of a .plBinAudioClip that the resource needs, instead of the whole file.
///
/// The clip is read from its own file, or from a loaded packed bank that carries it (plAudioPackedContent).
///
/// Reads the asset header and the clip header, picks the section of GetLanguage() (the default language when the
/// clip has no such section), skips the other sections and reads the priming data and, unless the clip is
/// streamed, the payload. The stream handed to plAudioClipResource::UpdateContent is:
///   string osFilePath, string language, u32 languageGeneration, u64 payloadFileOffset, u64 payloadBytes,
///   u32 headerBytes, [clip header], [start priming], [loop priming], [payload (not for Streamed)]
///
/// osFilePath is the absolute path of the clip file for streamed clips that are plain files on disk, and empty
/// otherwise (resident clips, clips inside a package archive).
class PL_PLASMAAUDIOPLUGIN_DLL plAudioClipResourceLoader : public plResourceTypeLoader
{
public:
  /// \brief Sets the language that clips are loaded in. "" selects the default language. Affects clips loaded afterwards.
  /// Thread-safe: the language is read by resource loader threads.
  static void SetLanguage(plStringView sLanguage);

  /// \brief Returns a copy of the language that clips are loaded in. Thread-safe.
  static plString GetLanguage();

  /// \brief A number that changes whenever SetLanguage() changes the language. Every loaded clip records the number
  /// of the language it was loaded in (plAudioClipResource::GetLanguageGeneration()). Thread-safe, lock-free.
  static plUInt32 GetLanguageGeneration();

  virtual plResourceLoadData OpenDataStream(const plResource* pResource) override;
  virtual void CloseDataStream(const plResource* pResource, const plResourceLoadData& loaderData) override;
  virtual bool IsResourceOutdated(const plResource* pResource) const override;
};
