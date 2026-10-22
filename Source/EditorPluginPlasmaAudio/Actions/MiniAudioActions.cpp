#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Actions/ProjectActions.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/Actions/MiniAudioActions.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAsset.h>
#include <EditorPluginPlasmaAudio/Platform/AudioPlatformsDlg.h>
#include <EditorPluginPlasmaAudio/Preferences/MiniAudioPreferences.h>
#include <GuiFoundation/Action/ActionManager.h>
#include <GuiFoundation/UIServices/UIServices.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plPlasmaAudioAction, 0, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plPlasmaAudioSliderAction, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

plActionDescriptorHandle plPlasmaAudioActions::s_hCategoryMiniAudio;
plActionDescriptorHandle plPlasmaAudioActions::s_hMute;
plActionDescriptorHandle plPlasmaAudioActions::s_hVolume;
plActionDescriptorHandle plPlasmaAudioActions::s_hExtractClips;
plActionDescriptorHandle plPlasmaAudioActions::s_hMigrateAllEvents;
plActionDescriptorHandle plPlasmaAudioActions::s_hPlatforms;

void plPlasmaAudioActions::RegisterActions()
{
  s_hCategoryMiniAudio = PL_REGISTER_CATEGORY("PlasmaAudio");
  s_hMute = PL_REGISTER_ACTION_1("PlasmaAudio.Mute", plActionScope::Document, "PlasmaAudio", "", plPlasmaAudioAction, plPlasmaAudioAction::ActionType::Mute);
  s_hVolume = PL_REGISTER_ACTION_1("PlasmaAudio.Volume", plActionScope::Document, "PlasmaAudio", "", plPlasmaAudioSliderAction, plPlasmaAudioSliderAction::ActionType::Volume);
  s_hExtractClips = PL_REGISTER_ACTION_1("PlasmaAudio.ExtractClips", plActionScope::Document, "PlasmaAudio", "", plPlasmaAudioAction, plPlasmaAudioAction::ActionType::ExtractClips);

  // Document scope although it acts on the project: it is mapped into every document's Project menu, and a Global
  // proxy would be cached with the first window's document context
  s_hMigrateAllEvents = PL_REGISTER_ACTION_1("PlasmaAudio.MigrateAllEvents", plActionScope::Document, "PlasmaAudio", "", plPlasmaAudioAction, plPlasmaAudioAction::ActionType::MigrateAllEvents);
  s_hPlatforms = PL_REGISTER_ACTION_1("PlasmaAudio.Platforms", plActionScope::Document, "PlasmaAudio", "", plPlasmaAudioAction, plPlasmaAudioAction::ActionType::Platforms);
  plProjectActions::AddProjectMenuMapper(plMakeDelegate(&plPlasmaAudioActions::MapProjectMenuActions));
}

void plPlasmaAudioActions::UnregisterActions()
{
  plProjectActions::RemoveProjectMenuMapper(plMakeDelegate(&plPlasmaAudioActions::MapProjectMenuActions));

  plActionManager::UnregisterAction(s_hCategoryMiniAudio);
  plActionManager::UnregisterAction(s_hMute);
  plActionManager::UnregisterAction(s_hVolume);
  plActionManager::UnregisterAction(s_hExtractClips);
  plActionManager::UnregisterAction(s_hMigrateAllEvents);
  plActionManager::UnregisterAction(s_hPlatforms);
}

void plPlasmaAudioActions::MapProjectMenuActions(plStringView sMapping)
{
  plActionMap* pMap = plActionMapManager::GetActionMap(sMapping);
  if (pMap == nullptr)
    return;

  pMap->MapAction(s_hMigrateAllEvents, "G.Project.Assets", 6.0f);

  // next to Asset Profiles: the settings are per profile
  pMap->MapAction(s_hPlatforms, "G.Project.Settings", 6.5f);
}

void plPlasmaAudioActions::MapEventAssetActions(plStringView sMenuBar, plStringView sToolBar)
{
  plActionMap* pMenuMap = plActionMapManager::GetActionMap(sMenuBar);
  PL_ASSERT_DEV(pMenuMap != nullptr, "Mapping the actions failed!");
  pMenuMap->MapAction(s_hExtractClips, "G.AssetDoc", 5.0f);

  plActionMap* pToolMap = plActionMapManager::GetActionMap(sToolBar);
  PL_ASSERT_DEV(pToolMap != nullptr, "Mapping the actions failed!");
  pToolMap->MapAction(s_hExtractClips, "AssetCategory", 5.0f);
}

void plPlasmaAudioActions::MapMenuActions(plStringView sMapping)
{
  plActionMap* pMap = plActionMapManager::GetActionMap(sMapping);
  PL_ASSERT_DEV(pMap != nullptr, "Mapping the actions failed!");

  pMap->MapAction(s_hCategoryMiniAudio, "G.Plugins.Settings", 9.0f);

  pMap->MapAction(s_hCategoryMiniAudio, "G.Scene", 5.0f);
  pMap->MapAction(s_hMute, "G.Scene", "PlasmaAudio", 0.0f);
  pMap->MapAction(s_hVolume, "G.Scene", "PlasmaAudio", 1.0f);
}

void plPlasmaAudioActions::MapToolbarActions(plStringView sMapping)
{
  plActionMap* pSceneMap = plActionMapManager::GetActionMap(sMapping);
  PL_ASSERT_DEV(pSceneMap != nullptr, "Mapping the actions failed!");

  pSceneMap->MapAction(s_hCategoryMiniAudio, "", 12.0f);
  pSceneMap->MapAction(s_hMute, "PlasmaAudio", 0.0f);
}

plPlasmaAudioAction::plPlasmaAudioAction(const plActionContext& context, const char* szName, ActionType type)
  : plButtonAction(context, szName, false, "")
{
  m_Type = type;

  switch (m_Type)
  {
    case ActionType::Mute:
    {
      SetCheckable(true);

      plPlasmaAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plPlasmaAudioProjectPreferences>();
      pPreferences->m_ChangedEvent.AddEventHandler(plMakeDelegate(&plPlasmaAudioAction::OnPreferenceChange, this));

      if (pPreferences->GetMute())
        SetIconPath(":/Icons/SoundOff.svg");
      else
        SetIconPath(":/Icons/SoundOn.svg");

      SetChecked(pPreferences->GetMute());
    }
    break;

    case ActionType::ExtractClips:
    case ActionType::MigrateAllEvents:
      SetIconPath(":/AssetIcons/AudioClip.svg");
      break;

    case ActionType::Platforms:
      SetIconPath(":/AssetIcons/AudioMixer.svg");
      break;
  }
}

plPlasmaAudioAction::~plPlasmaAudioAction()
{
  if (m_Type == ActionType::Mute)
  {
    plPlasmaAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plPlasmaAudioProjectPreferences>();
    pPreferences->m_ChangedEvent.RemoveEventHandler(plMakeDelegate(&plPlasmaAudioAction::OnPreferenceChange, this));
  }
}

void plPlasmaAudioAction::Execute(const plVariant& value)
{
  if (m_Type == ActionType::Mute)
  {
    plPlasmaAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plPlasmaAudioProjectPreferences>();
    pPreferences->SetMute(!pPreferences->GetMute());

    if (GetContext().m_pDocument)
    {
      GetContext().m_pDocument->ShowDocumentStatus(plFmt("Sound is {}", pPreferences->GetMute() ? "muted" : "on"));
    }
  }
  else if (m_Type == ActionType::ExtractClips)
  {
    ExecuteExtractClips();
  }
  else if (m_Type == ActionType::MigrateAllEvents)
  {
    ExecuteMigrateAllEvents();
  }
  else if (m_Type == ActionType::Platforms)
  {
    plAudioPlatformsDlg::Run();
  }
}

void plPlasmaAudioAction::ExecuteExtractClips()
{
  plAudioEventAssetDocument* pEvent = plDynamicCast<plAudioEventAssetDocument*>(GetContext().m_pDocument);
  if (pEvent == nullptr)
    return;

  plUInt32 uiConverted = 0;
  const plStatus res = pEvent->ExtractClips(uiConverted);
  if (res.Failed())
  {
    plEditorUiServices::MessageBoxStatus(res, "Extracting the clips failed.");
    return;
  }

  if (uiConverted == 0)
  {
    pEvent->ShowDocumentStatus("No playlist entry embeds a sound file.");
    return;
  }

  pEvent->SaveDocument(true).LogFailure();
  pEvent->ShowDocumentStatus(plFmt("{} playlist entries now reference Audio Clips.", uiConverted));
}

void plPlasmaAudioAction::ExecuteMigrateAllEvents()
{
  // events whose transform dependencies include a file path embed sound files; clip references are GUIDs
  plHybridArray<plString, 32> eventPaths;
  {
    const auto knownSubAssets = plAssetCurator::GetSingleton()->GetKnownSubAssets();

    for (auto it = knownSubAssets->GetIterator(); it.IsValid(); ++it)
    {
      const plSubAsset& subAsset = it.Value();

      if (!subAsset.m_bMainAsset || subAsset.m_pAssetInfo == nullptr || subAsset.m_pAssetInfo->m_pDocumentTypeDescriptor == nullptr || subAsset.m_pAssetInfo->m_Info == nullptr)
        continue;

      if (!subAsset.m_pAssetInfo->m_pDocumentTypeDescriptor->m_sDocumentTypeName.IsEqual_NoCase("Audio Event"))
        continue;

      bool bEmbedsFiles = false;
      for (const plString& sDependency : subAsset.m_pAssetInfo->m_Info->m_TransformDependencies)
      {
        if (!plConversionUtils::IsStringUuid(sDependency))
        {
          bEmbedsFiles = true;
          break;
        }
      }

      if (bEmbedsFiles)
        eventPaths.PushBack(subAsset.m_pAssetInfo->m_Path.GetAbsolutePath());
    }
  }

  plUInt32 uiEvents = 0;
  plUInt32 uiEntries = 0;
  plUInt32 uiFailed = 0;

  for (const plString& sPath : eventPaths)
  {
    const plDocumentTypeDescriptor* pTypeDesc = nullptr;
    if (plDocumentManager::FindDocumentTypeFromPath(sPath, false, pTypeDesc).Failed())
      continue;

    plDocument* pDoc = pTypeDesc->m_pManager->GetDocumentByPath(sPath);
    const bool bOpenedHere = pDoc == nullptr;

    if (bOpenedHere)
      pDoc = plEditorApp::GetSingleton()->OpenDocument(sPath, plDocumentFlags::None);

    plAudioEventAssetDocument* pEvent = plDynamicCast<plAudioEventAssetDocument*>(pDoc);
    if (pEvent != nullptr)
    {
      plUInt32 uiConverted = 0;
      const plStatus res = pEvent->ExtractClips(uiConverted);

      if (res.Failed())
      {
        plLog::Error("Migrating audio event '{0}' failed: {1}", sPath, res.m_sMessage);
        ++uiFailed;
      }
      else if (uiConverted > 0)
      {
        if (pEvent->SaveDocument(true).Succeeded())
        {
          ++uiEvents;
          uiEntries += uiConverted;
        }
        else
        {
          plLog::Error("Saving the migrated audio event '{0}' failed.", sPath);
          ++uiFailed;
        }
      }
    }
    else
    {
      ++uiFailed;
    }

    if (bOpenedHere && pDoc != nullptr)
      pDoc->GetDocumentManager()->CloseDocument(pDoc);
  }

  plStringBuilder sMsg;
  sMsg.SetFormat("Migrated {0} audio event(s): {1} playlist entries now reference Audio Clips.", uiEvents, uiEntries);
  if (uiFailed > 0)
    sMsg.AppendFormat("\n{0} event(s) failed, see the log.", uiFailed);

  plLog::Info("{}", sMsg);
  plEditorUiServices::MessageBoxInformation(plFmt("{}", sMsg));
}

void plPlasmaAudioAction::OnPreferenceChange(plPreferences* pref)
{
  plPlasmaAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plPlasmaAudioProjectPreferences>();

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

plPlasmaAudioSliderAction::plPlasmaAudioSliderAction(const plActionContext& context, const char* szName, ActionType type)
  : plSliderAction(context, szName)
{
  m_Type = type;

  switch (m_Type)
  {
    case ActionType::Volume:
    {
      plPlasmaAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plPlasmaAudioProjectPreferences>();

      pPreferences->m_ChangedEvent.AddEventHandler(plMakeDelegate(&plPlasmaAudioSliderAction::OnPreferenceChange, this));

      SetRange(0, 20);
    }
    break;
  }

  UpdateState();
}

plPlasmaAudioSliderAction::~plPlasmaAudioSliderAction()
{
  switch (m_Type)
  {
    case ActionType::Volume:
    {
      plPlasmaAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plPlasmaAudioProjectPreferences>();
      pPreferences->m_ChangedEvent.RemoveEventHandler(plMakeDelegate(&plPlasmaAudioSliderAction::OnPreferenceChange, this));
    }
    break;
  }
}

void plPlasmaAudioSliderAction::Execute(const plVariant& value)
{
  const plInt32 iValue = value.Get<plInt32>();

  switch (m_Type)
  {
    case ActionType::Volume:
    {
      plPlasmaAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plPlasmaAudioProjectPreferences>();

      pPreferences->SetVolume(iValue / 20.0f);

      if (GetContext().m_pDocument)
      {
        GetContext().m_pDocument->ShowDocumentStatus(plFmt("Sound Volume: {}%%", (int)(pPreferences->GetVolume() * 100.0f)));
      }
    }
    break;
  }
}

void plPlasmaAudioSliderAction::OnPreferenceChange(plPreferences* pref)
{
  UpdateState();
}

void plPlasmaAudioSliderAction::UpdateState()
{
  switch (m_Type)
  {
    case ActionType::Volume:
    {
      plPlasmaAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plPlasmaAudioProjectPreferences>();

      SetValue(plMath::Clamp((plInt32)(pPreferences->GetVolume() * 20.0f), 0, 20));
    }
    break;
  }
}
