#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EnginePluginPlasmaAudio/EnginePluginPlasmaAudioDLL.h>

/// \brief What the Audio Event window asks of its audition in the engine process.
struct PL_ENGINEPLUGINPLASMAAUDIO_DLL plAudioAuditionCommand
{
  using StorageType = plUInt8;

  enum Enum
  {
    Play,          ///< loads m_EventData as the audition's event and plays it
    Stop,          ///< stops with the event's stop fade
    SetParameters, ///< changes parameter values of the playing audition
    Poll,          ///< only answers with the status

    Default = Poll
  };
};
PL_DECLARE_REFLECTABLE_TYPE(PL_ENGINEPLUGINPLASMAAUDIO_DLL, plAudioAuditionCommand);

/// \brief A request of the Audio Event window's audition. Every request is answered with a plAudioAuditionStatusMsgToEditor.
class PL_ENGINEPLUGINPLASMAAUDIO_DLL plAudioAuditionMsgToEngine : public plEditorEngineDocumentMsg
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioAuditionMsgToEngine, plEditorEngineDocumentMsg);

public:
  plEnum<plAudioAuditionCommand> m_Command;

  /// \brief Play: the event as an event resource file reads it (its path, the asset file header, then the data).
  plDataBuffer m_EventData;

  /// \brief Play: the mixer resource loaded first when the engine process has no mixer yet.
  plString m_sMixer;

  /// \brief Play: where on the timeline to start, in seconds.
  double m_fStartOffset = 0.0;

  /// \brief Play: a seeded audition repeats its random picks.
  bool m_bSeeded = false;
  plUInt32 m_uiSeed = 0;

  /// \brief Play: switch states as "Group/State".
  plDynamicArray<plString> m_SwitchStates;

  /// \brief Play: heard at this distance from the listener (the Distance tab); negative plays it unspatialized.
  float m_fDistance = -1.0f;

  /// \brief SetParameters: the values to set. "Distance" moves the audition to that distance.
  plDynamicArray<plString> m_ParameterNames;
  plDynamicArray<float> m_ParameterValues;
};

/// \brief The state of the audition after a request.
class PL_ENGINEPLUGINPLASMAAUDIO_DLL plAudioAuditionStatusMsgToEditor : public plEditorEngineDocumentMsg
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioAuditionStatusMsgToEditor, plEditorEngineDocumentMsg);

public:
  bool m_bAvailable = false; ///< the engine process runs the PlasmaAudio middleware
  bool m_bPlaying = false;
  bool m_bWaiting = false;   ///< the event or the mixer is still loading
  bool m_bHasMixer = false;
  double m_fPosition = 0.0;  ///< timeline position in seconds
  float m_fPeak = 0.0f;      ///< peak of the master bus

  /// \brief Per instrument: whether it sounds (1) and the volume of its parameter range.
  plDynamicArray<plUInt8> m_InstrumentSounding;
  plDynamicArray<float> m_InstrumentRangeGains;

  /// \brief Why the last Play did not start, if it failed.
  plString m_sError;
};
