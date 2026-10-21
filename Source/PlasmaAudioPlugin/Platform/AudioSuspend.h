#pragma once

#include <Foundation/Time/Time.h>
#include <PlasmaAudioPlugin/Platform/AudioPlatformConfig.h>

/// \brief The output's way through a suspend and the resume after it.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioOutputState
{
  using StorageType = plUInt8;

  enum Enum : plUInt8
  {
    Running,
    FadingOut,  ///< a suspend was asked for: the output fades out before the device stops
    Suspended,  ///< the device is stopped
    Resuming,   ///< the device runs again and the output fades in
    Background, ///< suspended with plAudioSuspendPolicy::KeepPlaying: the output plays on

    Default = Running
  };

  static plStringView GetName(Enum state);
};

/// \brief Takes the output through a suspend and back, as the platform config's policy and fade say.
///
/// The middleware updates it every audio update with whether a suspend is asked for (PlasmaAudio.Suspend) and applies
/// what it returns: the gain the output plays at, whether the device runs, and whether every event is to stop. A resume
/// while fading out fades back in from where the fade got to. Audio thread only.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioSuspendController
{
public:
  struct Result
  {
    float m_fGain = 1.0f;          ///< linear, on top of the master volume
    bool m_bDeviceRunning = true;
    bool m_bStopAllEvents = false; ///< set for the one update in which a StopAll suspend stops the device
    bool m_bStateChanged = false;
  };

  Result Update(bool bSuspendRequested, plTime delta, plAudioSuspendPolicy::Enum policy, plTime fade);

  plAudioOutputState::Enum GetState() const { return m_State; }
  float GetGain() const { return m_fGain; }

private:
  plAudioOutputState::Enum m_State = plAudioOutputState::Running;
  float m_fGain = 1.0f;
};
