#pragma once

#include <EditorPluginMiniAudio/EditorPluginMiniAudioDLL.h>
#include <Foundation/Configuration/CVar.h>
#include <GuiFoundation/Action/BaseActions.h>
#include <GuiFoundation/GuiFoundationDLL.h>

class plPreferences;

class PL_EDITORPLUGINMINIAUDIO_DLL plMiniAudioActions
{
public:
  static void RegisterActions();
  static void UnregisterActions();

  static void MapMenuActions(plStringView sMapping);
  static void MapToolbarActions(plStringView sMapping);

  static plActionDescriptorHandle s_hCategoryMiniAudio;
  static plActionDescriptorHandle s_hMute;
  static plActionDescriptorHandle s_hVolume;
};


class PL_EDITORPLUGINMINIAUDIO_DLL plMiniAudioAction : public plButtonAction
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioAction, plButtonAction);

public:
  enum class ActionType
  {
    Mute,
  };

  plMiniAudioAction(const plActionContext& context, const char* szName, ActionType type);
  ~plMiniAudioAction();

  virtual void Execute(const plVariant& value) override;

private:
  void OnPreferenceChange(plPreferences* pref);

  ActionType m_Type;
};

class PL_EDITORPLUGINMINIAUDIO_DLL plMiniAudioSliderAction : public plSliderAction
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioSliderAction, plSliderAction);

public:
  enum class ActionType
  {
    Volume,
  };

  plMiniAudioSliderAction(const plActionContext& context, const char* szName, ActionType type);
  ~plMiniAudioSliderAction();

  virtual void Execute(const plVariant& value) override;

private:
  void OnPreferenceChange(plPreferences* pref);
  void UpdateState();

  ActionType m_Type;
};
