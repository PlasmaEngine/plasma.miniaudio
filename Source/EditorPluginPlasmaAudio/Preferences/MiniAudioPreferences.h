#pragma once

#include <EditorFramework/IPC/EngineProcessConnection.h>
#include <EditorFramework/Preferences/Preferences.h>
#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <Foundation/Strings/String.h>

class PL_EDITORPLUGINPLASMAAUDIO_DLL plPlasmaAudioProjectPreferences : public plPreferences
{
  PL_ADD_DYNAMIC_REFLECTION(plPlasmaAudioProjectPreferences, plPreferences);

public:
  plPlasmaAudioProjectPreferences();
  ~plPlasmaAudioProjectPreferences();

  void SetMute(bool bMute);
  bool GetMute() const { return m_bMute; }

  void SetVolume(float fVolume);
  float GetVolume() const { return m_fMasterVolume; }

  void SyncCVars();

  /// \brief The mixer the Audio Event window auditions through when the engine process has none loaded: the one set in
  /// the preferences, else the project's only mixer. Empty when there is none, or several and none is set.
  plString GetAuditionMixer() const;

private:
  void ProcessEventHandler(const plEditorEngineProcessConnection::Event& e);

  bool m_bMute = false;
  float m_fMasterVolume = 1.0f;
  plString m_sAuditionMixer; ///< asset GUID of an Audio Mixer
};
