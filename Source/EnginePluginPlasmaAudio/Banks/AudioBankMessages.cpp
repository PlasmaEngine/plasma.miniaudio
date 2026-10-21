#include <EnginePluginPlasmaAudio/EnginePluginPlasmaAudioPCH.h>

#include <EnginePluginPlasmaAudio/Banks/AudioBankMessages.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioBankCommand, 1)
  PL_ENUM_CONSTANT(plAudioBankCommand::Load),
  PL_ENUM_CONSTANT(plAudioBankCommand::Unload),
  PL_ENUM_CONSTANT(plAudioBankCommand::Poll),
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioBankMsgToEngine, 1, plRTTIDefaultAllocator<plAudioBankMsgToEngine>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Command", plAudioBankCommand, m_Command),
    PL_MEMBER_PROPERTY("Bank", m_sBank),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioBankStatusMsgToEditor, 1, plRTTIDefaultAllocator<plAudioBankStatusMsgToEditor>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Available", m_bAvailable),
    PL_ARRAY_MEMBER_PROPERTY("Banks", m_Banks),
    PL_ARRAY_MEMBER_PROPERTY("States", m_States),
    PL_ARRAY_MEMBER_PROPERTY("ReadyItems", m_ReadyItems),
    PL_ARRAY_MEMBER_PROPERTY("Items", m_Items),
    PL_ARRAY_MEMBER_PROPERTY("OverBudget", m_OverBudget),
    PL_ARRAY_MEMBER_PROPERTY("LoadedHere", m_LoadedHere),
    PL_MEMBER_PROPERTY("DecodedBytes", m_uiDecodedBytes),
    PL_MEMBER_PROPERTY("CompressedBytes", m_uiCompressedBytes),
    PL_MEMBER_PROPERTY("PrimingBytes", m_uiPrimingBytes),
    PL_MEMBER_PROPERTY("RingBytes", m_uiRingBytes),
    PL_MEMBER_PROPERTY("BudgetTotalBytes", m_uiBudgetTotalBytes),
    PL_MEMBER_PROPERTY("Error", m_sError),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on
