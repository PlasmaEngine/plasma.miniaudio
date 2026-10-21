#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EnginePluginPlasmaAudio/EnginePluginPlasmaAudioDLL.h>

/// \brief What the Audio Mixer window asks of the engine process.
struct PL_ENGINEPLUGINPLASMAAUDIO_DLL plAudioMixerConsoleCommand
{
  using StorageType = plUInt8;

  enum Enum
  {
    Apply,            ///< m_MixerData becomes the content of this mixer's resource; a loaded mixer applies it in place
    SetBusMute,       ///< m_sTarget = bus, m_fValue = 1 mutes, 0 unmutes
    SetBusSolo,       ///< m_sTarget = bus, m_fValue = 1 solos, 0 unsolos
    ClearMuteAndSolo, ///<
    SetSnapshot,      ///< m_sTarget = snapshot, m_fValue = intensity, without the snapshot's fade
    SetTestSignal,    ///< m_fValue = 1 plays the test signal into the loaded mixer, 0 stops it
    Poll,             ///< only answers with the status

    Default = Poll
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_ENGINEPLUGINPLASMAAUDIO_DLL, plAudioMixerConsoleCommand);

/// \brief A request of the Audio Mixer window. Every request is answered with a plAudioMixerConsoleStatusMsgToEditor.
class PL_ENGINEPLUGINPLASMAAUDIO_DLL plAudioMixerConsoleMsgToEngine : public plEditorEngineDocumentMsg
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerConsoleMsgToEngine, plEditorEngineDocumentMsg);

public:
  plEnum<plAudioMixerConsoleCommand> m_Command;

  /// \brief Apply: the mixer as a mixer resource file reads it (its path, the asset file header, then the desc).
  plDataBuffer m_MixerData;

  /// \brief Apply: load this mixer when the engine process plays another one or none (the console's Preview source).
  bool m_bLoad = false;

  plString m_sTarget;
  float m_fValue = 0.0f;
};

/// \brief The state of the engine process's mixer after a request.
class PL_ENGINEPLUGINPLASMAAUDIO_DLL plAudioMixerConsoleStatusMsgToEditor : public plEditorEngineDocumentMsg
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerConsoleStatusMsgToEditor, plEditorEngineDocumentMsg);

public:
  bool m_bAvailable = false; ///< the engine process runs the PlasmaAudio middleware
  bool m_bHasMixer = false;
  bool m_bIsThisMixer = false; ///< the loaded mixer is the window's document
  plString m_sLoadedMixer;     ///< resource id of the loaded mixer
  bool m_bTestSignal = false;
  plUInt32 m_uiLayoutRebuilds = 0;
  plUInt32 m_uiRealVoices = 0;

  /// \brief Per bus of the loaded mixer.
  plDynamicArray<plString> m_BusNames;
  plDynamicArray<float> m_BusPeaks;
  plDynamicArray<float> m_BusRms;
  plDynamicArray<plUInt8> m_BusMuted;
  plDynamicArray<plUInt8> m_BusSoloed;

  /// \brief Per bus, per effect, bus after bus: gain reduction in dB.
  plDynamicArray<float> m_EffectGainReductions;

  plDynamicArray<plString> m_SnapshotNames;
  plDynamicArray<float> m_SnapshotIntensities;

  plString m_sError;
};
