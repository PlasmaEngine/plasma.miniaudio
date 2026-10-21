#include <EnginePluginPlasmaAudio/EnginePluginPlasmaAudioPCH.h>

#include <EnginePluginPlasmaAudio/Mixer/AudioMixerConsoleMessages.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioMixerConsoleCommand, 1)
  PL_ENUM_CONSTANT(plAudioMixerConsoleCommand::Apply),
  PL_ENUM_CONSTANT(plAudioMixerConsoleCommand::SetBusMute),
  PL_ENUM_CONSTANT(plAudioMixerConsoleCommand::SetBusSolo),
  PL_ENUM_CONSTANT(plAudioMixerConsoleCommand::ClearMuteAndSolo),
  PL_ENUM_CONSTANT(plAudioMixerConsoleCommand::SetSnapshot),
  PL_ENUM_CONSTANT(plAudioMixerConsoleCommand::SetTestSignal),
  PL_ENUM_CONSTANT(plAudioMixerConsoleCommand::Poll),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerConsoleMsgToEngine, 1, plRTTIDefaultAllocator<plAudioMixerConsoleMsgToEngine>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Command", plAudioMixerConsoleCommand, m_Command),
    PL_MEMBER_PROPERTY("MixerData", m_MixerData),
    PL_MEMBER_PROPERTY("Load", m_bLoad),
    PL_MEMBER_PROPERTY("Target", m_sTarget),
    PL_MEMBER_PROPERTY("Value", m_fValue),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerConsoleStatusMsgToEditor, 1, plRTTIDefaultAllocator<plAudioMixerConsoleStatusMsgToEditor>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Available", m_bAvailable),
    PL_MEMBER_PROPERTY("HasMixer", m_bHasMixer),
    PL_MEMBER_PROPERTY("IsThisMixer", m_bIsThisMixer),
    PL_MEMBER_PROPERTY("LoadedMixer", m_sLoadedMixer),
    PL_MEMBER_PROPERTY("TestSignal", m_bTestSignal),
    PL_MEMBER_PROPERTY("LayoutRebuilds", m_uiLayoutRebuilds),
    PL_MEMBER_PROPERTY("RealVoices", m_uiRealVoices),
    PL_ARRAY_MEMBER_PROPERTY("BusNames", m_BusNames),
    PL_ARRAY_MEMBER_PROPERTY("BusPeaks", m_BusPeaks),
    PL_ARRAY_MEMBER_PROPERTY("BusRms", m_BusRms),
    PL_ARRAY_MEMBER_PROPERTY("BusMuted", m_BusMuted),
    PL_ARRAY_MEMBER_PROPERTY("BusSoloed", m_BusSoloed),
    PL_ARRAY_MEMBER_PROPERTY("EffectGainReductions", m_EffectGainReductions),
    PL_ARRAY_MEMBER_PROPERTY("SnapshotNames", m_SnapshotNames),
    PL_ARRAY_MEMBER_PROPERTY("SnapshotIntensities", m_SnapshotIntensities),
    PL_MEMBER_PROPERTY("Error", m_sError),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on
