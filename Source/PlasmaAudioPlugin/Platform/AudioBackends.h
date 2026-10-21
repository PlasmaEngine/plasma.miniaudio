#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Strings/String.h>
#include <MiniAudio/miniaudio.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

/// \brief An output backend a platform plugin brings: the callbacks of miniaudio's custom backend.
///
/// A plugin registers its backend with plAudioBackendRegistry (before the audio system starts, or followed by a change of
/// the PlasmaAudio.Backend cvar) and keeps the object alive until it unregisters it. The context and its devices call the
/// callbacks; see miniaudio's custom backend documentation for what each one does.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioCustomBackend
{
public:
  virtual ~plAudioCustomBackend() = default;

  /// \brief The name the backend is chosen by (PlasmaAudio.Backend, the platform config).
  virtual plStringView GetName() const = 0;

  /// \brief Fills in the callbacks miniaudio's custom backend calls.
  virtual void GetCallbacks(ma_backend_callbacks& out_callbacks) const = 0;
};

/// \brief The output backends of this platform: miniaudio's built-in ones and those plugins register. Any thread.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioBackendRegistry
{
public:
  static void RegisterCustomBackend(const plAudioCustomBackend* pBackend);
  static void UnregisterCustomBackend(const plAudioCustomBackend* pBackend);

  /// \brief The names of the backends that can be chosen: the built-in ones compiled for this platform in their usual
  /// order ("Null" last), then the registered custom ones.
  static void GetBackendNames(plDynamicArray<plString>& out_names);

  /// \brief What a backend name picks.
  struct Choice
  {
    bool m_bFound = false;                       ///< false: unknown or empty name, the usual order applies
    ma_backend m_Backend = ma_backend_null;      ///< the built-in backend, or ma_backend_custom
    const plAudioCustomBackend* m_pCustom = nullptr;
  };

  static Choice Resolve(plStringView sName);
};
