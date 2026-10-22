#pragma once

#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <EditorUi/Content.h>
#include <GameUiPlugin/Elements/UiDrawList.h>
#include <PlasmaAudioPlugin/Codecs/AudioCodecBenchmark.h>
#include <PlasmaAudioPlugin/Platform/AudioPlatformConfig.h>

struct plEditorUiPointerEvent;

/// \brief The Audio Platforms page: the PlasmaAudio output settings of each platform profile.
///
/// One tab per asset profile. Output: backend, speakers, sample rate, period and whether the output follows the
/// system's default device. Decoding: the Opus kernels this CPU uses and a decode benchmark. Suspend and resume: the
/// policy and fade, and a button that simulates a suspend in the editor's engine process. The side shows what the
/// engine process's output runs on and a log of its state changes.
///
/// The view only shows and asks; plAudioPlatformsDlg owns the profiles and talks to the engine process.
class PL_EDITORPLUGINPLASMAAUDIO_DLL plEditorUiAudioPlatformsView : public plEditorUiContent
{
public:
  plEditorUiAudioPlatformsView();
  ~plEditorUiAudioPlatformsView() override;

  /// \brief The profile tabs and which is shown; the active profile (the one the editor runs) is marked.
  void SetProfiles(plArrayPtr<const plString> names, plUInt32 uiShown, plUInt32 uiActive);

  /// \brief The settings of the shown profile.
  void SetConfig(const plPlasmaAudioPlatformConfig& config);

  /// \brief The backends that can be chosen (the empty name, "the usual order", is added in front).
  void SetBackends(plArrayPtr<const plString> names);

  /// \brief What the engine process's output runs on, and its state.
  void SetOutput(plStringView sDevice, plStringView sState);

  /// \brief A line in the output log, newest on top.
  enum class LogKind : plUInt8
  {
    Normal,
    Ok,
    Warning
  };
  void AddLog(plStringView sText, LogKind kind = LogKind::Normal);

  void SetSimdName(plStringView sName);
  void SetBenchmark(const plAudioCodecBenchmark::Result& result);
  void SetBenchmarkRunning(bool bRunning);
  void SetSuspendRunning(bool bRunning);

  plDelegate<void(plUInt32)> m_OnProfileChosen;
  plDelegate<void(const plPlasmaAudioPlatformConfig&)> m_OnConfigChanged;
  plDelegate<void()> m_OnRunBenchmark;
  plDelegate<void()> m_OnSimulateSuspend;

protected:
  void OnUiUpdate() override;
  void OnUiEvent(const plMsgUiEvent& event) override;

private:
  enum class Slider : plUInt8
  {
    None,
    Period,
    Fade
  };

  plRectFloat GetSliderRect(Slider slider) const;
  float GetRowY(plUInt32 uiRow) const;
  float GetSideX() const;

  void BuildDocument();
  void Draw();
  void DrawSettings();
  void DrawSide();
  void ShowMenu(plStringView sAction);
  void Changed();
  void OnPointer(const plEditorUiPointerEvent& e);

  plHybridArray<plString, 4> m_Profiles;
  plUInt32 m_uiShownProfile = 0;
  plUInt32 m_uiActiveProfile = 0;
  plHybridArray<plString, 8> m_Backends;
  plPlasmaAudioPlatformConfig m_Config;

  plString m_sDevice;
  plString m_sState;

  struct LogLine
  {
    plString m_sText;
    LogKind m_Kind = LogKind::Normal;
  };
  plDynamicArray<LogLine> m_Log; ///< oldest first

  plString m_sSimd;
  plAudioCodecBenchmark::Result m_Benchmark;
  bool m_bHasBenchmark = false;
  bool m_bBenchmarkRunning = false;
  bool m_bSuspendRunning = false;

  Slider m_DragSlider = Slider::None;
  float m_fDragValue = 0.0f;

  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
  plEventSubscriptionID m_PointerSubscription = 0;
};
