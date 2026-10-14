#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/Preferences/MiniAudioPreferences.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plPlasmaAudioProjectPreferences, 1, plRTTIDefaultAllocator<plPlasmaAudioProjectPreferences>)
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

plPlasmaAudioProjectPreferences::plPlasmaAudioProjectPreferences()
  : plPreferences(Domain::Project, "PlasmaAudio")
{
  plEditorEngineProcessConnection::s_Events.AddEventHandler(plMakeDelegate(&plPlasmaAudioProjectPreferences::ProcessEventHandler, this));
}

plPlasmaAudioProjectPreferences::~plPlasmaAudioProjectPreferences()
{
  plEditorEngineProcessConnection::s_Events.RemoveEventHandler(plMakeDelegate(&plPlasmaAudioProjectPreferences::ProcessEventHandler, this));
}

void plPlasmaAudioProjectPreferences::SetMute(bool bMute)
{
  m_bMute = bMute;

  SyncCVars();
}

void plPlasmaAudioProjectPreferences::SetVolume(float fVolume)
{
  m_fMasterVolume = plMath::Clamp(fVolume, 0.0f, 1.0f);

  SyncCVars();
}

void plPlasmaAudioProjectPreferences::SyncCVars()
{
  TriggerPreferencesChangedEvent();

  {
    plChangeCVarMsgToEngine msg;
    msg.m_sCVarName = "PlasmaAudio.Mute";
    msg.m_NewValue = m_bMute;

    plEditorEngineProcessConnection::GetSingleton()->SendMessage(&msg);
  }

  {
    plChangeCVarMsgToEngine msg;
    msg.m_sCVarName = "PlasmaAudio.Volume";
    msg.m_NewValue = m_fMasterVolume;

    plEditorEngineProcessConnection::GetSingleton()->SendMessage(&msg);
  }
}

void plPlasmaAudioProjectPreferences::ProcessEventHandler(const plEditorEngineProcessConnection::Event& e)
{
  if (e.m_Type == plEditorEngineProcessConnection::Event::Type::ProcessRestarted)
  {
    SyncCVars();
  }
}
