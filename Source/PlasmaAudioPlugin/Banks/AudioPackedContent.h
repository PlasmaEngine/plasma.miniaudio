#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Core/ResourceManager/ResourceTypeLoader.h>
#include <Foundation/Strings/String.h>

class plAudioBankResource;

/// \brief The events and clips that loaded packed banks carry: where in which bank file the built file of each one lies.
///
/// The middleware registers a packed bank's entries when it loads the bank and unregisters them when it unloads it. The
/// clip and event loaders read a registered resource from its bank file instead of its own file. When several loaded
/// banks pack the same resource, the one registered first is used until it is unregistered. Thread-safe.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioPackedContent
{
public:
  struct Location
  {
    plString m_sFile;        ///< the bank file, as the file system opens it
    plUInt64 m_uiOffset = 0; ///< file offset of the built file
    plUInt64 m_uiBytes = 0;
  };

  /// \brief Registers every packed entry of the loaded bank, owned by uiOwner (the bank id).
  static void Register(plUInt64 uiOwner, const plAudioBankResource& bank);

  /// \brief Removes the entries uiOwner registered.
  static void Unregister(plUInt64 uiOwner);

  /// \brief Where the resource's built file lies, if a loaded packed bank carries it.
  static bool Find(plStringView sResourceId, Location& out_location);

  /// \brief The number of registered entries.
  static plUInt32 GetCount();
};

/// \brief Loads audio events from a packed bank when one carries them, else from their own file.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioEventResourceLoader : public plResourceLoaderFromFile
{
public:
  virtual plResourceLoadData OpenDataStream(const plResource* pResource) override;
  virtual void CloseDataStream(const plResource* pResource, const plResourceLoadData& loaderData) override;
};
