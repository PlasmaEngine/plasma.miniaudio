#include <EnginePluginPlasmaAudio/EnginePluginPlasmaAudioPCH.h>

#include <EnginePluginPlasmaAudio/Audition/AudioAuditionMessages.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioAuditionCommand, 1)
  PL_ENUM_CONSTANT(plAudioAuditionCommand::Play),
  PL_ENUM_CONSTANT(plAudioAuditionCommand::Stop),
  PL_ENUM_CONSTANT(plAudioAuditionCommand::SetParameters),
  PL_ENUM_CONSTANT(plAudioAuditionCommand::Poll),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioAuditionMsgToEngine, 1, plRTTIDefaultAllocator<plAudioAuditionMsgToEngine>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Command", plAudioAuditionCommand, m_Command),
    PL_MEMBER_PROPERTY("EventData", m_EventData),
    PL_MEMBER_PROPERTY("Mixer", m_sMixer),
    PL_MEMBER_PROPERTY("StartOffset", m_fStartOffset),
    PL_MEMBER_PROPERTY("Seeded", m_bSeeded),
    PL_MEMBER_PROPERTY("Seed", m_uiSeed),
    PL_ARRAY_MEMBER_PROPERTY("SwitchStates", m_SwitchStates),
    PL_ARRAY_MEMBER_PROPERTY("ParameterNames", m_ParameterNames),
    PL_ARRAY_MEMBER_PROPERTY("ParameterValues", m_ParameterValues),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioAuditionStatusMsgToEditor, 1, plRTTIDefaultAllocator<plAudioAuditionStatusMsgToEditor>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Available", m_bAvailable),
    PL_MEMBER_PROPERTY("Playing", m_bPlaying),
    PL_MEMBER_PROPERTY("Waiting", m_bWaiting),
    PL_MEMBER_PROPERTY("HasMixer", m_bHasMixer),
    PL_MEMBER_PROPERTY("Position", m_fPosition),
    PL_MEMBER_PROPERTY("Peak", m_fPeak),
    PL_ARRAY_MEMBER_PROPERTY("InstrumentSounding", m_InstrumentSounding),
    PL_ARRAY_MEMBER_PROPERTY("InstrumentRangeGains", m_InstrumentRangeGains),
    PL_MEMBER_PROPERTY("Error", m_sError),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on
