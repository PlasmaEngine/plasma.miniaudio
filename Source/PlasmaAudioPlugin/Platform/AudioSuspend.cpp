#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Platform/AudioSuspend.h>

plStringView plAudioOutputState::GetName(Enum state)
{
  switch (state)
  {
    case FadingOut:
      return "fading out";
    case Suspended:
      return "suspended";
    case Resuming:
      return "resuming";
    case Background:
      return "running in the background";
    default:
      return "running";
  }
}

plAudioSuspendController::Result plAudioSuspendController::Update(bool bSuspendRequested, plTime delta, plAudioSuspendPolicy::Enum policy, plTime fade)
{
  Result result;
  const plAudioOutputState::Enum previous = m_State;

  // the share of the fade this update covers; no fade steps in one update
  const float fStep = fade.IsPositive() ? static_cast<float>(delta.GetSeconds() / fade.GetSeconds()) : 1.0f;

  if (bSuspendRequested)
  {
    switch (m_State)
    {
      case plAudioOutputState::Running:
      case plAudioOutputState::Resuming:
        m_State = (policy == plAudioSuspendPolicy::KeepPlaying) ? plAudioOutputState::Background : plAudioOutputState::FadingOut;
        break;

      default:
        break;
    }

    if (m_State == plAudioOutputState::FadingOut)
    {
      m_fGain = plMath::Max(m_fGain - fStep, 0.0f);

      if (m_fGain <= 0.0f)
      {
        m_State = plAudioOutputState::Suspended;
        result.m_bStopAllEvents = policy == plAudioSuspendPolicy::StopAll;
      }
    }
  }
  else
  {
    switch (m_State)
    {
      case plAudioOutputState::Background:
        m_State = plAudioOutputState::Running;
        break;

      case plAudioOutputState::FadingOut:
      case plAudioOutputState::Suspended:
        m_State = plAudioOutputState::Resuming;
        break;

      default:
        break;
    }

    if (m_State == plAudioOutputState::Resuming)
    {
      m_fGain = plMath::Min(m_fGain + fStep, 1.0f);

      if (m_fGain >= 1.0f)
        m_State = plAudioOutputState::Running;
    }
  }

  // the gain of a state without a fade
  if (m_State == plAudioOutputState::Running || m_State == plAudioOutputState::Background)
    m_fGain = 1.0f;
  else if (m_State == plAudioOutputState::Suspended)
    m_fGain = 0.0f;

  result.m_fGain = m_fGain;
  result.m_bDeviceRunning = m_State != plAudioOutputState::Suspended;
  result.m_bStateChanged = m_State != previous;
  return result;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Platform_AudioSuspend);
