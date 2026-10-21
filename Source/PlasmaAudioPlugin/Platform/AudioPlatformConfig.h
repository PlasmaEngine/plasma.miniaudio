#pragma once

#include <Core/Configuration/PlatformProfile.h>
#include <Foundation/Reflection/Reflection.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

/// \brief The speaker layout the output mixes for.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioSpeakerLayout
{
  using StorageType = plUInt8;

  enum Enum : plUInt8
  {
    Device,     ///< whatever the device reports
    Stereo,     ///< 2 channels
    Surround51, ///< 6 channels
    Surround71, ///< 8 channels

    Default = Device
  };

  /// \brief The channel count of the layout; 0 for Device.
  static plUInt32 GetChannelCount(Enum layout);
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PLASMAAUDIOPLUGIN_DLL, plAudioSpeakerLayout);

/// \brief What the output does while the application is suspended (sent to the background, a console's system menu).
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioSuspendPolicy
{
  using StorageType = plUInt8;

  enum Enum : plUInt8
  {
    Pause,       ///< fades out and stops the device; everything resumes where it was
    KeepPlaying, ///< keeps playing (background audio)
    StopAll,     ///< fades out, stops every event and the device

    Default = Pause
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PLASMAAUDIOPLUGIN_DLL, plAudioSuspendPolicy);

/// \brief The PlasmaAudio output settings of a platform profile.
///
/// Edited per profile (the editor's Audio Platforms page and the asset profiles dialog) and saved with the profile's
/// runtime data, so a game started with that profile opens its output this way. At startup the middleware copies the
/// settings into the PlasmaAudio output cvars, which can change them while the game runs.
class PL_PLASMAAUDIOPLUGIN_DLL plPlasmaAudioPlatformConfig : public plProfileConfigData
{
  PL_ADD_DYNAMIC_REFLECTION(plPlasmaAudioPlatformConfig, plProfileConfigData);

public:
  /// \brief The output backend by name (see plAudioBackendRegistry); empty tries the platform's backends in their
  /// usual order.
  plString m_sBackend;

  plEnum<plAudioSpeakerLayout> m_eSpeakers;
  plUInt32 m_uiSampleRate = 0;  ///< Hz; 0 = the device's
  plUInt32 m_uiPeriodMs = 0;    ///< update period of the output; 0 = the backend's default
  bool m_bFollowDefaultDevice = true; ///< moves to the new default device when the system's default changes

  plEnum<plAudioSuspendPolicy> m_eSuspendPolicy;
  float m_fSuspendFadeMs = 80.0f; ///< fade out before a suspend and in after the resume

  virtual void SaveRuntimeData(plChunkStreamWriter& inout_stream) const override;
  virtual void LoadRuntimeData(plChunkStreamReader& inout_stream) override;
};
