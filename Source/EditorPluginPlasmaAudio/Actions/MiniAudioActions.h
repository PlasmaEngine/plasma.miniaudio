#pragma once

#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <Foundation/Configuration/CVar.h>
#include <GuiFoundation/Action/BaseActions.h>
#include <GuiFoundation/GuiFoundationDLL.h>

class plPreferences;

class PL_EDITORPLUGINPLASMAAUDIO_DLL plPlasmaAudioActions
{
public:
  static void RegisterActions();
  static void UnregisterActions();

  static void MapMenuActions(plStringView sMapping);
  static void MapToolbarActions(plStringView sMapping);

  /// \brief Maps "Extract Clips" into the Audio Event window's menu bar and tool bar.
  static void MapEventAssetActions(plStringView sMenuBar, plStringView sToolBar);

  static plActionDescriptorHandle s_hCategoryMiniAudio;
  static plActionDescriptorHandle s_hMute;
  static plActionDescriptorHandle s_hVolume;
  static plActionDescriptorHandle s_hExtractClips;
  static plActionDescriptorHandle s_hMigrateAllEvents;

private:
  static void MapProjectMenuActions(plStringView sMapping);
};


class PL_EDITORPLUGINPLASMAAUDIO_DLL plPlasmaAudioAction : public plButtonAction
{
  PL_ADD_DYNAMIC_REFLECTION(plPlasmaAudioAction, plButtonAction);

public:
  enum class ActionType
  {
    Mute,
    ExtractClips,     ///< the context Audio Event: embedded sound files -> Audio Clip references, then save
    MigrateAllEvents, ///< ExtractClips on every Audio Event of the project
  };

  plPlasmaAudioAction(const plActionContext& context, const char* szName, ActionType type);
  ~plPlasmaAudioAction();

  virtual void Execute(const plVariant& value) override;

private:
  void OnPreferenceChange(plPreferences* pref);
  void ExecuteExtractClips();
  void ExecuteMigrateAllEvents();

  ActionType m_Type;
};

class PL_EDITORPLUGINPLASMAAUDIO_DLL plPlasmaAudioSliderAction : public plSliderAction
{
  PL_ADD_DYNAMIC_REFLECTION(plPlasmaAudioSliderAction, plSliderAction);

public:
  enum class ActionType
  {
    Volume,
  };

  plPlasmaAudioSliderAction(const plActionContext& context, const char* szName, ActionType type);
  ~plPlasmaAudioSliderAction();

  virtual void Execute(const plVariant& value) override;

private:
  void OnPreferenceChange(plPreferences* pref);
  void UpdateState();

  ActionType m_Type;
};
