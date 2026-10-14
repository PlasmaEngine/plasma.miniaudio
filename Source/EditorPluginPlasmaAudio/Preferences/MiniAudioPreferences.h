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

private:
  void ProcessEventHandler(const plEditorEngineProcessConnection::Event& e);

  bool m_bMute = false;
  float m_fMasterVolume = 1.0f;
};
