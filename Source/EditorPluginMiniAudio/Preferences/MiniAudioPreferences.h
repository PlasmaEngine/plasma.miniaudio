#pragma once

#include <EditorFramework/IPC/EngineProcessConnection.h>
#include <EditorFramework/Preferences/Preferences.h>
#include <EditorPluginMiniAudio/EditorPluginMiniAudioDLL.h>
#include <Foundation/Strings/String.h>

class PL_EDITORPLUGINMINIAUDIO_DLL plMiniAudioProjectPreferences : public plPreferences
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioProjectPreferences, plPreferences);

public:
  plMiniAudioProjectPreferences();
  ~plMiniAudioProjectPreferences();

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
