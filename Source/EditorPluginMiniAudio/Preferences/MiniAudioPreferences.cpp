#include <EditorPluginMiniAudio/EditorPluginMiniAudioPCH.h>

#include <EditorPluginMiniAudio/Preferences/MiniAudioPreferences.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioProjectPreferences, 1, plRTTIDefaultAllocator<plMiniAudioProjectPreferences>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Mute", m_bMute),
    PL_MEMBER_PROPERTY("Volume", m_fMasterVolume)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 1.0f)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plMiniAudioProjectPreferences::plMiniAudioProjectPreferences()
  : plPreferences(Domain::Project, "MiniAudio")
{
  plEditorEngineProcessConnection::s_Events.AddEventHandler(plMakeDelegate(&plMiniAudioProjectPreferences::ProcessEventHandler, this));
}

plMiniAudioProjectPreferences::~plMiniAudioProjectPreferences()
{
  plEditorEngineProcessConnection::s_Events.RemoveEventHandler(plMakeDelegate(&plMiniAudioProjectPreferences::ProcessEventHandler, this));
}

void plMiniAudioProjectPreferences::SetMute(bool bMute)
{
  m_bMute = bMute;

  SyncCVars();
}

void plMiniAudioProjectPreferences::SetVolume(float fVolume)
{
  m_fMasterVolume = plMath::Clamp(fVolume, 0.0f, 1.0f);

  SyncCVars();
}

void plMiniAudioProjectPreferences::SyncCVars()
{
  TriggerPreferencesChangedEvent();

  {
    plChangeCVarMsgToEngine msg;
    msg.m_sCVarName = "MiniAudio.Mute";
    msg.m_NewValue = m_bMute;

    plEditorEngineProcessConnection::GetSingleton()->SendMessage(&msg);
  }

  {
    plChangeCVarMsgToEngine msg;
    msg.m_sCVarName = "MiniAudio.Volume";
    msg.m_NewValue = m_fMasterVolume;

    plEditorEngineProcessConnection::GetSingleton()->SendMessage(&msg);
  }
}

void plMiniAudioProjectPreferences::ProcessEventHandler(const plEditorEngineProcessConnection::Event& e)
{
  if (e.m_Type == plEditorEngineProcessConnection::Event::Type::ProcessRestarted)
  {
    SyncCVars();
  }
}
