#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Actions/AssetActions.h>
#include <EditorFramework/Actions/ProjectActions.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/EngineShell/WindowContent.h>
#include <GuiFoundation/UIServices/DynamicStringEnum.h>
#include <EditorPluginPlasmaAudio/Actions/MiniAudioActions.h>
#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationAsset.h>
#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationAssetWindow.h>
#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAssetWindow.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAssetWindow.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAsset.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAssetWindow.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerAsset.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerAssetWindow.h>
#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicAsset.h>
#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicAssetWindow.h>
#include <EditorPluginPlasmaAudio/Preferences/MiniAudioPreferences.h>
#include <EditorPluginPlasmaAudio/Preview/AudioEnumRegistry.h>
#include <EditorPluginPlasmaAudio/Profiler/AudioProfilerPanel.h>
#include <EditorPluginPlasmaAudio/SoundAsset/MiniAudioSoundAsset.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Configuration/Plugin.h>
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

    // audio events and music, plus the legacy sound asset which ActivateTrigger still plays
    if (!sDocType.IsEqual_NoCase("Audio Event") && !sDocType.IsEqual_NoCase("Audio Music") && !sDocType.IsEqual_NoCase("MiniAudioSound"))
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

/// The package's icon folder: the icons under their ids (":/AssetIcons/AudioClip.svg" is
/// "EditorIcons/AssetIcons/AudioClip.svg"). The plugin binary lives in <package>/Bin/<configuration>.
static plString s_sIconFolder;

static void AddPackageIconFolder()
{
  plStringBuilder sPluginFile, sCopiedFile;
  plPlugin::GetPluginPaths("plEditorPluginPlasmaAudio", sPluginFile, sCopiedFile, 0);

  plStringBuilder sFolder = sPluginFile.GetFileDirectory();
  sFolder.AppendPath("..", "..", "EditorIcons");
  sFolder.MakeCleanPath();

  s_sIconFolder = sFolder;
  plEditorUi::AddIconFolder(s_sIconFolder);
}

static void RemovePackageIconFolder()
{
  if (s_sIconFolder.IsEmpty())
    return;

  plEditorUi::RemoveIconFolder(s_sIconFolder);
  s_sIconFolder.Clear();
}

/// How the audio windows arrange their panels in the engine shell when a document opens without a saved layout.
static void SetPanelArrangements(bool bSet)
{
  plWindowContentProvider::SetDefaultArrangement("Audio Clip", bSet ? &plAudioClipAssetDocumentWindow::ArrangePanels : nullptr);
  plWindowContentProvider::SetDefaultArrangement("Audio Event", bSet ? &plAudioEventAssetDocumentWindow::ArrangePanels : nullptr);
  plWindowContentProvider::SetDefaultArrangement("Audio Mixer", bSet ? &plAudioMixerAssetDocumentWindow::ArrangePanels : nullptr);
  plWindowContentProvider::SetDefaultArrangement("Audio Attenuation", bSet ? &plAudioAttenuationAssetDocumentWindow::ArrangePanels : nullptr);
  plWindowContentProvider::SetDefaultArrangement("Audio Bank", bSet ? &plAudioBankAssetDocumentWindow::ArrangePanels : nullptr);
  plWindowContentProvider::SetDefaultArrangement("Audio Music", bSet ? &plAudioMusicAssetDocumentWindow::ArrangePanels : nullptr);
}

void OnLoadPlugin()
{
  AddPackageIconFolder();

  plToolsProject::GetSingleton()->s_Events.AddEventHandler(ToolsProjectEventHandler);
  plAssetCurator::GetSingleton()->m_Events.AddEventHandler(AssetCuratorEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plMiniAudioSoundAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plAudioEventAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plAudioMixerAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plAudioClipAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plAudioAttenuationAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(plAudioMusicAssetProperties::PropertyMetaStateEventHandler);

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

  // Audio Clip, Audio Bank, Audio Attenuation and Audio Music assets
  for (const char* szAsset : {"AudioClipAsset", "AudioBankAsset", "AudioAttenuationAsset", "AudioMusicAsset"})
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

  SetPanelArrangements(true);

  // the panel registers itself as an application panel of the engine shell (closed, listed in the Panels menu); the
  // user layout restored after plugin loading decides whether it is open
  if (!plEditorApp::GetSingleton()->IsInHeadlessMode())
  {
    PL_DEFAULT_NEW(plAudioProfilerPanel);
  }
}

void OnUnloadPlugin()
{
  // unregistering the panel destroys the shell's content of it before the plugin's code goes
  if (plAudioProfilerPanel* pPanel = plAudioProfilerPanel::GetSingleton())
  {
    PL_DEFAULT_DELETE(pPanel);
  }

  SetPanelArrangements(false);
  plPlasmaAudioActions::UnregisterActions();
  plAssetCurator::GetSingleton()->m_Events.RemoveEventHandler(AssetCuratorEventHandler);
  plToolsProject::GetSingleton()->s_Events.RemoveEventHandler(ToolsProjectEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plMiniAudioSoundAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plAudioEventAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plAudioMixerAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plAudioClipAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plAudioAttenuationAssetProperties::PropertyMetaStateEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(plAudioMusicAssetProperties::PropertyMetaStateEventHandler);

  RemovePackageIconFolder();
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
