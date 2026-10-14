#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Actions/AssetActions.h>
#include <EditorFramework/Actions/ProjectActions.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <GuiFoundation/UIServices/DynamicStringEnum.h>
#include <EditorPluginPlasmaAudio/Actions/MiniAudioActions.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAsset.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerAsset.h>
#include <EditorPluginPlasmaAudio/Preferences/MiniAudioPreferences.h>
#include <EditorPluginPlasmaAudio/Preview/AudioEnumRegistry.h>
#include <EditorPluginPlasmaAudio/Profiler/AudioProfilerPanel.moc.h>
#include <GuiFoundation/ContainerWindow/ContainerWindow.moc.h>
#include <EditorPluginPlasmaAudio/SoundAsset/MiniAudioSoundAsset.h>
#include <GuiFoundation/Action/CommandHistoryActions.h>
#include <GuiFoundation/Action/DocumentActions.h>
#include <GuiFoundation/Action/StandardMenus.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>

static void ToolsProjectEventHandler(const plToolsProjectEvent& e);
static void AssetCuratorEventHandler(const plAssetCuratorEvent& e);

/// \brief Fills the shared "AudioSystem.Trigger" enum with everything this backend can play,
/// so plAudioTriggerComponent offers a dropdown of the project's audio events.
///
/// Values are stored as asset GUIDs because that is what the PlasmaAudio middleware resolves
/// at runtime; the property grid renders them as their asset path.
static void UpdateAudioSystemTriggerEnum()
{
  if (plAssetCurator::GetSingleton() == nullptr)
    return;

  auto& triggerEnum = plDynamicStringEnum::CreateDynamicEnum("AudioSystem.Trigger");
  triggerEnum.Clear();

  const auto knownSubAssets = plAssetCurator::GetSingleton()->GetKnownSubAssets();

  for (auto it = knownSubAssets->GetIterator(); it.IsValid(); ++it)
  {
    const plSubAsset& subAsset = it.Value();

    if (!subAsset.m_bMainAsset || subAsset.m_pAssetInfo == nullptr || subAsset.m_pAssetInfo->m_pDocumentTypeDescriptor == nullptr)
      continue;

    const plStringView sDocType = subAsset.m_pAssetInfo->m_pDocumentTypeDescriptor->m_sDocumentTypeName;

    // audio events, plus the legacy sound asset which ActivateTrigger still plays
    if (!sDocType.IsEqual_NoCase("Audio Event") && !sDocType.IsEqual_NoCase("MiniAudioSound"))
      continue;

    plStringBuilder sGuid;
    plConversionUtils::ToString(subAsset.m_Data.m_Guid, sGuid);

    triggerEnum.AddValidValue(sGuid);
  }

  triggerEnum.SortValues();
}

/// \brief Fills the shared "AudioSystem.Bank" enum with the project's audio banks, so plAudioBankLoaderComponent and
/// the startup banks offer a dropdown.
///
/// Values are asset GUIDs: the PlasmaAudio middleware loads a bank by that resource id.
static void UpdateAudioSystemBankEnum()
{
  if (plAssetCurator::GetSingleton() == nullptr)
    return;

  auto& bankEnum = plDynamicStringEnum::CreateDynamicEnum("AudioSystem.Bank");
  bankEnum.Clear();

  const auto knownSubAssets = plAssetCurator::GetSingleton()->GetKnownSubAssets();

  for (auto it = knownSubAssets->GetIterator(); it.IsValid(); ++it)
  {
    const plSubAsset& subAsset = it.Value();

    if (!subAsset.m_bMainAsset || subAsset.m_pAssetInfo == nullptr || subAsset.m_pAssetInfo->m_pDocumentTypeDescriptor == nullptr)
      continue;

    if (!subAsset.m_pAssetInfo->m_pDocumentTypeDescriptor->m_sDocumentTypeName.IsEqual_NoCase("Audio Bank"))
      continue;

    plStringBuilder sGuid;
    plConversionUtils::ToString(subAsset.m_Data.m_Guid, sGuid);

    bankEnum.AddValidValue(sGuid);
  }

  bankEnum.SortValues();
}

/// \brief Fills the control-name enums (buses, environments, snapshots, RTPCs, switch states)
/// from the meta info the audio assets publish, so the components offer dropdowns without the
/// mixer or event documents being open.
///
/// Only ever adds: these enums are user-editable and shared with other audio backends.
static void UpdateAudioSystemControlEnums()
{
  if (plAssetCurator::GetSingleton() == nullptr)
    return;

  const auto knownSubAssets = plAssetCurator::GetSingleton()->GetKnownSubAssets();

  for (auto it = knownSubAssets->GetIterator(); it.IsValid(); ++it)
  {
    const plSubAsset& subAsset = it.Value();

    if (!subAsset.m_bMainAsset || subAsset.m_pAssetInfo == nullptr || subAsset.m_pAssetInfo->m_Info == nullptr)
      continue;

    if (const auto* pMixerMeta = subAsset.m_pAssetInfo->m_Info->GetMetaInfo<plAudioMixerAssetMetaInfo>())
    {
      for (const plString& sName : pMixerMeta->m_Buses)
        plAudioEnumRegistry::RegisterBusName(sName);

      if (!pMixerMeta->m_OutputBuses.IsEmpty())
      {
        for (const plString& sName : pMixerMeta->m_OutputBuses)
          plAudioEnumRegistry::RegisterOutputBusName(sName);
      }
      else
      {
        // mixer saved before OutputBuses existed: it cannot tell us which buses are aux, so offer
        // all of them rather than an empty dropdown. Re-saving the mixer restores the aux filtering.
        for (const plString& sName : pMixerMeta->m_Buses)
          plAudioEnumRegistry::RegisterOutputBusName(sName);
      }

      for (const plString& sName : pMixerMeta->m_Environments)
        plAudioEnumRegistry::RegisterEnvironmentName(sName);

      for (const plString& sName : pMixerMeta->m_Snapshots)
        plAudioEnumRegistry::RegisterSnapshotName(sName);
    }

    if (const auto* pEventMeta = subAsset.m_pAssetInfo->m_Info->GetMetaInfo<plAudioEventAssetMetaInfo>())
    {
      for (const plString& sName : pEventMeta->m_Parameters)
        plAudioEnumRegistry::RegisterRtpcName(sName);

      for (const plString& sName : pEventMeta->m_SwitchStates)
        plAudioEnumRegistry::RegisterSwitchStateName(sName);
    }
  }
}

void OnLoadPlugin()
{
  plToolsProject::GetSingleton()->s_Events.AddEventHandler(ToolsProjectEventHandler);
  plAssetCurator::GetSingleton()->m_Events.AddEventHandler(AssetCuratorEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plMiniAudioSoundAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plAudioEventAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plAudioMixerAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plAudioClipAssetProperties::PropertyMetaStateEventHandler);

  // inline attenuation curve for the event's Rolloff property
  plQtPropertyGridWidget::GetFactory().RegisterCreator(plGetStaticRTTI<plAudioRolloffCurveWidgetAttribute>(),
    [](const plRTTI* pRtti) -> plQtPropertyWidget* { PL_IGNORE_UNUSED(pRtti); return new plQtAudioRolloffPropertyWidget(); });

  // Mesh
  {
    // Menu Bar
    plActionMapManager::RegisterActionMap("MiniAudioSoundAssetMenuBar").IgnoreResult();

    // Tool Bar
    {
      plActionMapManager::RegisterActionMap("MiniAudioSoundAssetToolBar").IgnoreResult();
    }
  }

  // Audio Event asset
  {
    const char* szMenuBar = "AudioEventAssetMenuBar";
    plActionMapManager::RegisterActionMap(szMenuBar).IgnoreResult();
    plStandardMenus::MapActions(szMenuBar, plStandardMenuTypes::Default | plStandardMenuTypes::Edit);
    plProjectActions::MapActions(szMenuBar);
    plDocumentActions::MapMenuActions(szMenuBar);
    plAssetActions::MapMenuActions(szMenuBar);
    plCommandHistoryActions::MapActions(szMenuBar);

    const char* szToolBar = "AudioEventAssetToolBar";
    plActionMapManager::RegisterActionMap(szToolBar).IgnoreResult();
    plDocumentActions::MapToolbarActions(szToolBar);
    plCommandHistoryActions::MapActions(szToolBar, "");
    plAssetActions::MapToolBarActions(szToolBar, true);
  }

  // Audio Mixer asset
  {
    const char* szMenuBar = "AudioMixerAssetMenuBar";
    plActionMapManager::RegisterActionMap(szMenuBar).IgnoreResult();
    plStandardMenus::MapActions(szMenuBar, plStandardMenuTypes::Default | plStandardMenuTypes::Edit);
    plProjectActions::MapActions(szMenuBar);
    plDocumentActions::MapMenuActions(szMenuBar);
    plAssetActions::MapMenuActions(szMenuBar);
    plCommandHistoryActions::MapActions(szMenuBar);

    const char* szToolBar = "AudioMixerAssetToolBar";
    plActionMapManager::RegisterActionMap(szToolBar).IgnoreResult();
    plDocumentActions::MapToolbarActions(szToolBar);
    plCommandHistoryActions::MapActions(szToolBar, "");
    plAssetActions::MapToolBarActions(szToolBar, true);
  }

  // Audio Clip and Audio Bank assets
  for (const char* szAsset : {"AudioClipAsset", "AudioBankAsset"})
  {
    plStringBuilder szMenuBar(szAsset, "MenuBar");
    plActionMapManager::RegisterActionMap(szMenuBar).IgnoreResult();
    plStandardMenus::MapActions(szMenuBar, plStandardMenuTypes::Default | plStandardMenuTypes::Edit);
    plProjectActions::MapActions(szMenuBar);
    plDocumentActions::MapMenuActions(szMenuBar);
    plAssetActions::MapMenuActions(szMenuBar);
    plCommandHistoryActions::MapActions(szMenuBar);

    plStringBuilder szToolBar(szAsset, "ToolBar");
    plActionMapManager::RegisterActionMap(szToolBar).IgnoreResult();
    plDocumentActions::MapToolbarActions(szToolBar);
    plCommandHistoryActions::MapActions(szToolBar, "");
    plAssetActions::MapToolBarActions(szToolBar, true);
  }

  // Scene
  {
    // Menu Bar
    {
      plPlasmaAudioActions::RegisterActions();
      plPlasmaAudioActions::MapMenuActions("EditorPluginScene_DocumentMenuBar");
      plPlasmaAudioActions::MapMenuActions("EditorPluginScene_Scene2MenuBar");
      plPlasmaAudioActions::MapToolbarActions("EditorPluginScene_DocumentToolBar");
      plPlasmaAudioActions::MapToolbarActions("EditorPluginScene_Scene2ToolBar");
    }
  }

  plPlasmaAudioActions::MapEventAssetActions("AudioEventAssetMenuBar", "AudioEventAssetToolBar");

  // the panel adds itself to the container window's bottom dock area and to the Panels menu; the user layout restored
  // after plugin loading decides whether it is open
  if (!plQtEditorApp::GetSingleton()->IsInHeadlessMode() && plQtContainerWindow::GetContainerWindow() != nullptr)
  {
    plQtAudioProfilerPanel* pPanel = new plQtAudioProfilerPanel();
    pPanel->toggleView(false);
  }
}

void OnUnloadPlugin()
{
  // Plugins unload after the container window was deleted, which deletes its panels as Qt children. A panel that is
  // still alive at that point is no longer referenced by any dock manager.
  if (plQtAudioProfilerPanel* pPanel = plQtAudioProfilerPanel::GetSingleton())
  {
    delete pPanel;
  }

  plPlasmaAudioActions::UnregisterActions();
  plAssetCurator::GetSingleton()->m_Events.RemoveEventHandler(AssetCuratorEventHandler);
  plToolsProject::GetSingleton()->s_Events.RemoveEventHandler(ToolsProjectEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plMiniAudioSoundAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plAudioEventAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plAudioMixerAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plAudioClipAssetProperties::PropertyMetaStateEventHandler);
  plQtPropertyGridWidget::GetFactory().UnregisterCreator(plGetStaticRTTI<plAudioRolloffCurveWidgetAttribute>());
}

static void ToolsProjectEventHandler(const plToolsProjectEvent& e)
{
  if (e.m_Type == plToolsProjectEvent::Type::ProjectOpened)
  {
    UpdateAudioSystemTriggerEnum();
    UpdateAudioSystemBankEnum();
    UpdateAudioSystemControlEnums();

    plPlasmaAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plPlasmaAudioProjectPreferences>();
    pPreferences->SyncCVars();
  }
}

static void AssetCuratorEventHandler(const plAssetCuratorEvent& e)
{
  // keep the trigger dropdown in sync as audio events are added, removed or renamed
  switch (e.m_Type)
  {
    case plAssetCuratorEvent::Type::AssetAdded:
    case plAssetCuratorEvent::Type::AssetRemoved:
    case plAssetCuratorEvent::Type::AssetMoved:
    case plAssetCuratorEvent::Type::AssetUpdated:
    case plAssetCuratorEvent::Type::AssetListReset:
      UpdateAudioSystemTriggerEnum();
      UpdateAudioSystemBankEnum();
      UpdateAudioSystemControlEnums();
      break;

    default:
      break;
  }
}

PL_PLUGIN_ON_LOADED()
{
  OnLoadPlugin();
}

PL_PLUGIN_ON_UNLOADED()
{
  OnUnloadPlugin();
}
