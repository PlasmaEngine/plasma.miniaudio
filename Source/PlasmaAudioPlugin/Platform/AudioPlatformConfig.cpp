#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/IO/ChunkStream.h>
#include <PlasmaAudioPlugin/Platform/AudioPlatformConfig.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioSpeakerLayout, 1)
  PL_ENUM_CONSTANTS(plAudioSpeakerLayout::Device, plAudioSpeakerLayout::Stereo, plAudioSpeakerLayout::Surround51, plAudioSpeakerLayout::Surround71)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioSuspendPolicy, 1)
  PL_ENUM_CONSTANTS(plAudioSuspendPolicy::Pause, plAudioSuspendPolicy::KeepPlaying, plAudioSuspendPolicy::StopAll)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plPlasmaAudioPlatformConfig, 1, plRTTIDefaultAllocator<plPlasmaAudioPlatformConfig>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Backend", m_sBackend),
    PL_ENUM_MEMBER_PROPERTY("Speakers", plAudioSpeakerLayout, m_eSpeakers),
    PL_MEMBER_PROPERTY("SampleRate", m_uiSampleRate)->AddAttributes(new plClampValueAttribute(0, 192000)),
    PL_MEMBER_PROPERTY("PeriodMs", m_uiPeriodMs)->AddAttributes(new plClampValueAttribute(0, 100), new plSuffixAttribute(" ms")),
    PL_MEMBER_PROPERTY("FollowDefaultDevice", m_bFollowDefaultDevice)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_ENUM_MEMBER_PROPERTY("SuspendPolicy", plAudioSuspendPolicy, m_eSuspendPolicy),
    PL_MEMBER_PROPERTY("SuspendFade", m_fSuspendFadeMs)->AddAttributes(new plDefaultValueAttribute(80.0f), new plClampValueAttribute(0.0f, 2000.0f), new plSuffixAttribute(" ms")),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plUInt32 plAudioSpeakerLayout::GetChannelCount(Enum layout)
{
  switch (layout)
  {
    case Stereo:
      return 2;
    case Surround51:
      return 6;
    case Surround71:
      return 8;
    default:
      return 0;
  }
}

void plPlasmaAudioPlatformConfig::SaveRuntimeData(plChunkStreamWriter& inout_stream) const
{
  inout_stream.BeginChunk("plPlasmaAudioPlatformConfig", 1);

  inout_stream << m_sBackend;
  inout_stream << m_eSpeakers;
  inout_stream << m_uiSampleRate;
  inout_stream << m_uiPeriodMs;
  inout_stream << m_bFollowDefaultDevice;
  inout_stream << m_eSuspendPolicy;
  inout_stream << m_fSuspendFadeMs;

  inout_stream.EndChunk();
}

void plPlasmaAudioPlatformConfig::LoadRuntimeData(plChunkStreamReader& inout_stream)
{
  const auto& chunk = inout_stream.GetCurrentChunk();

  if (chunk.m_sChunkName == "plPlasmaAudioPlatformConfig" && chunk.m_uiChunkVersion >= 1)
  {
    inout_stream >> m_sBackend;
    inout_stream >> m_eSpeakers;
    inout_stream >> m_uiSampleRate;
    inout_stream >> m_uiPeriodMs;
    inout_stream >> m_bFollowDefaultDevice;
    inout_stream >> m_eSuspendPolicy;
    inout_stream >> m_fSuspendFadeMs;
  }
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Platform_AudioPlatformConfig);
