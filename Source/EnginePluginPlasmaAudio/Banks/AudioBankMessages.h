#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EnginePluginPlasmaAudio/EnginePluginPlasmaAudioDLL.h>

/// \brief What the Audio Bank window asks of the engine process.
struct PL_ENGINEPLUGINPLASMAAUDIO_DLL plAudioBankCommand
{
  using StorageType = plUInt8;

  enum Enum
  {
    Load,   ///< loads the bank m_sBank (its asset GUID) through the audio system, as a bank loader component does
    Unload, ///< unloads the bank m_sBank, if the window loaded it
    Poll,   ///< only answers with the status

    Default = Poll
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_ENGINEPLUGINPLASMAAUDIO_DLL, plAudioBankCommand);

/// \brief A request of the Audio Bank window. Every request is answered with a plAudioBankStatusMsgToEditor.
class PL_ENGINEPLUGINPLASMAAUDIO_DLL plAudioBankMsgToEngine : public plEditorEngineDocumentMsg
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioBankMsgToEngine, plEditorEngineDocumentMsg);

public:
  plEnum<plAudioBankCommand> m_Command;
  plString m_sBank;
};

/// \brief The banks the engine process has loaded and the memory of its loaded audio content.
class PL_ENGINEPLUGINPLASMAAUDIO_DLL plAudioBankStatusMsgToEditor : public plEditorEngineDocumentMsg
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioBankStatusMsgToEditor, plEditorEngineDocumentMsg);

public:
  bool m_bAvailable = false; ///< the engine process runs the PlasmaAudio middleware

  /// \brief Per loaded bank: its resource id, plAudioBankMemorySnapshotEntry::State, the events and clips that finished
  /// loading and all of them, and whether loading it took the content over a budget.
  plDynamicArray<plString> m_Banks;
  plDynamicArray<plUInt8> m_States;
  plDynamicArray<plUInt32> m_ReadyItems;
  plDynamicArray<plUInt32> m_Items;
  plDynamicArray<plUInt8> m_OverBudget;

  /// \brief The banks of m_Banks this window loaded.
  plDynamicArray<plString> m_LoadedHere;

  /// \brief plAudioMemoryUsage of the loaded content.
  plUInt64 m_uiDecodedBytes = 0;
  plUInt64 m_uiCompressedBytes = 0;
  plUInt64 m_uiPrimingBytes = 0;
  plUInt64 m_uiRingBytes = 0;

  /// \brief The budget the engine process checks against (that of the last loaded bank with one), 0 = none.
  plUInt64 m_uiBudgetTotalBytes = 0;

  plString m_sError;
};
