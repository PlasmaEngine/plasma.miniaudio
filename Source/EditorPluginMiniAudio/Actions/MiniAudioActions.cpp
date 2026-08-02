#include <EditorPluginMiniAudio/EditorPluginMiniAudioPCH.h>

#include <EditorPluginMiniAudio/Actions/MiniAudioActions.h>
#include <EditorPluginMiniAudio/Preferences/MiniAudioPreferences.h>
#include <GuiFoundation/Action/ActionManager.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioAction, 0, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioSliderAction, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

plActionDescriptorHandle plMiniAudioActions::s_hCategoryMiniAudio;
plActionDescriptorHandle plMiniAudioActions::s_hMute;
plActionDescriptorHandle plMiniAudioActions::s_hVolume;

void plMiniAudioActions::RegisterActions()
{
  s_hCategoryMiniAudio = PL_REGISTER_CATEGORY("MiniAudio");
  s_hMute = PL_REGISTER_ACTION_1("MiniAudio.Mute", plActionScope::Document, "MiniAudio", "", plMiniAudioAction, plMiniAudioAction::ActionType::Mute);
  s_hVolume = PL_REGISTER_ACTION_1("MiniAudio.Volume", plActionScope::Document, "MiniAudio", "", plMiniAudioSliderAction, plMiniAudioSliderAction::ActionType::Volume);
}

void plMiniAudioActions::UnregisterActions()
{
  plActionManager::UnregisterAction(s_hCategoryMiniAudio);
  plActionManager::UnregisterAction(s_hMute);
  plActionManager::UnregisterAction(s_hVolume);
}

void plMiniAudioActions::MapMenuActions(plStringView sMapping)
{
  plActionMap* pMap = plActionMapManager::GetActionMap(sMapping);
  PL_ASSERT_DEV(pMap != nullptr, "Mapping the actions failed!");

  pMap->MapAction(s_hCategoryMiniAudio, "G.Plugins.Settings", 9.0f);

  pMap->MapAction(s_hCategoryMiniAudio, "G.Scene", 5.0f);
  pMap->MapAction(s_hMute, "G.Scene", "MiniAudio", 0.0f);
  pMap->MapAction(s_hVolume, "G.Scene", "MiniAudio", 1.0f);
}

void plMiniAudioActions::MapToolbarActions(plStringView sMapping)
{
  plActionMap* pSceneMap = plActionMapManager::GetActionMap(sMapping);
  PL_ASSERT_DEV(pSceneMap != nullptr, "Mapping the actions failed!");

  pSceneMap->MapAction(s_hCategoryMiniAudio, "", 12.0f);
  pSceneMap->MapAction(s_hMute, "MiniAudio", 0.0f);
}

plMiniAudioAction::plMiniAudioAction(const plActionContext& context, const char* szName, ActionType type)
  : plButtonAction(context, szName, false, "")
{
  m_Type = type;

  switch (m_Type)
  {
    case ActionType::Mute:
    {
      SetCheckable(true);

      plMiniAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plMiniAudioProjectPreferences>();
      pPreferences->m_ChangedEvent.AddEventHandler(plMakeDelegate(&plMiniAudioAction::OnPreferenceChange, this));

      if (pPreferences->GetMute())
        SetIconPath(":/Icons/SoundOff.svg");
      else
        SetIconPath(":/Icons/SoundOn.svg");

      SetChecked(pPreferences->GetMute());
    }
    break;
  }
}

plMiniAudioAction::~plMiniAudioAction()
{
  if (m_Type == ActionType::Mute)
  {
    plMiniAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plMiniAudioProjectPreferences>();
    pPreferences->m_ChangedEvent.RemoveEventHandler(plMakeDelegate(&plMiniAudioAction::OnPreferenceChange, this));
  }
}

void plMiniAudioAction::Execute(const plVariant& value)
{
  if (m_Type == ActionType::Mute)
  {
    plMiniAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plMiniAudioProjectPreferences>();
    pPreferences->SetMute(!pPreferences->GetMute());

    if (GetContext().m_pDocument)
    {
      GetContext().m_pDocument->ShowDocumentStatus(plFmt("Sound is {}", pPreferences->GetMute() ? "muted" : "on"));
    }
  }
}

void plMiniAudioAction::OnPreferenceChange(plPreferences* pref)
{
  plMiniAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plMiniAudioProjectPreferences>();

  if (m_Type == ActionType::Mute)
  {
    if (pPreferences->GetMute())
      SetIconPath(":/Icons/SoundOff.svg");
    else
      SetIconPath(":/Icons/SoundOn.svg");

    SetChecked(pPreferences->GetMute());
  }
}

//////////////////////////////////////////////////////////////////////////

plMiniAudioSliderAction::plMiniAudioSliderAction(const plActionContext& context, const char* szName, ActionType type)
  : plSliderAction(context, szName)
{
  m_Type = type;

  switch (m_Type)
  {
    case ActionType::Volume:
    {
      plMiniAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plMiniAudioProjectPreferences>();

      pPreferences->m_ChangedEvent.AddEventHandler(plMakeDelegate(&plMiniAudioSliderAction::OnPreferenceChange, this));

      SetRange(0, 20);
    }
    break;
  }

  UpdateState();
}

plMiniAudioSliderAction::~plMiniAudioSliderAction()
{
  switch (m_Type)
  {
    case ActionType::Volume:
    {
      plMiniAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plMiniAudioProjectPreferences>();
      pPreferences->m_ChangedEvent.RemoveEventHandler(plMakeDelegate(&plMiniAudioSliderAction::OnPreferenceChange, this));
    }
    break;
  }
}

void plMiniAudioSliderAction::Execute(const plVariant& value)
{
  const plInt32 iValue = value.Get<plInt32>();

  switch (m_Type)
  {
    case ActionType::Volume:
    {
      plMiniAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plMiniAudioProjectPreferences>();

      pPreferences->SetVolume(iValue / 20.0f);

      if (GetContext().m_pDocument)
      {
        GetContext().m_pDocument->ShowDocumentStatus(plFmt("Sound Volume: {}%%", (int)(pPreferences->GetVolume() * 100.0f)));
      }
    }
    break;
  }
}

void plMiniAudioSliderAction::OnPreferenceChange(plPreferences* pref)
{
  UpdateState();
}

void plMiniAudioSliderAction::UpdateState()
{
  switch (m_Type)
  {
    case ActionType::Volume:
    {
      plMiniAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plMiniAudioProjectPreferences>();

      SetValue(plMath::Clamp((plInt32)(pPreferences->GetVolume() * 20.0f), 0, 20));
    }
    break;
  }
}
