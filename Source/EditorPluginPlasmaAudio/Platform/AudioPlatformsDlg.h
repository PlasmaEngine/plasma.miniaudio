#pragma once

#include <EditorFramework/IPC/EngineProcessConnection.h>
#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <EditorPluginPlasmaAudio/Platform/AudioPlatformsView.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <Foundation/Threading/TaskSystem.h>
#include <Foundation/Time/Time.h>
#include <GuiFoundation/EngineUi/EngineDialog.h>

/// \brief Project > Project Settings > Audio Platforms: the PlasmaAudio output settings saved with each asset profile.
///
/// Every change is written to the profile (SaveAssetProfiles) and its runtime file (RuntimeConfigs/<profile>.plProfile),
/// where a game started with the profile reads it. For the active profile the change is also sent to the editor's engine
/// process as cvars, so it applies at once.
///
/// While the dialog runs it polls the engine process's stats "PlasmaAudio/Output/Device" and "PlasmaAudio/Output/State"
/// and logs every change. The decode benchmark runs as a task; Simulate suspend sets PlasmaAudio.Suspend in the engine
/// process for a moment.
///
/// The body is the engine-drawn plEditorUiAudioPlatformsView.
class PL_EDITORPLUGINPLASMAAUDIO_DLL plAudioPlatformsDlg : public plEngineDialog
{
public:
  plAudioPlatformsDlg();
  ~plAudioPlatformsDlg();

  /// Runs the engine dialog, modal over the window that owns dialogs now; without a UI renderer it logs an error and
  /// rejects.
  static Result Run();

protected:
  void BuildBody(plUiElement* pBody, const plVec2& vSize) override;
  plVec2 GetPreferredSize() const override;

private:
  void OnTick();
  void EngineProcessMsgHandler(const plEditorEngineProcessConnection::Event& e);

  /// \brief The engine process connection when it can take messages, else null.
  static plEditorEngineProcessConnection* GetLiveConnection();

  /// \brief The profile's config, added when the profile has none yet.
  static plPlasmaAudioPlatformConfig* GetConfig(plUInt32 uiProfile);

  void ShowProfile(plUInt32 uiProfile);
  void OnConfigChanged(const plPlasmaAudioPlatformConfig& config);

  /// \brief Sends the active profile's settings to the engine process as cvars.
  void PushActiveConfig();
  static void SendCVar(plEditorEngineProcessConnection* pConnection, const char* szName, const plVariant& value);

  void ApplyStats(const plVariant& stats);
  void FinishBenchmark();
  void RunBenchmark();
  void SimulateSuspend();
  void EndSimulatedSuspend();

  /// \brief Gives the view the profiles and the shown profile's settings.
  void RefreshView();
  void AddLog(plStringView sText, plEditorUiAudioPlatformsView::LogKind kind = plEditorUiAudioPlatformsView::LogKind::Normal);

  plUniquePtr<plEditorUiAudioPlatformsView> m_pView;

  plEditorUiTimerGroup m_Timers; ///< the stats poll and the end of a simulated suspend
  plUInt32 m_uiShownProfile = 0;
  plHybridArray<plString, 8> m_Backends;

  // engine process
  bool m_bRequestPending = false;
  plTime m_LastRequest;
  plString m_sDevice;
  plString m_sState;
  bool m_bSuspendRunning = false;

  // benchmark
  struct BenchmarkJob : public plRefCounted
  {
    plAudioCodecBenchmark::Result m_Result;
    bool m_bSucceeded = false;
    plAtomicBool m_bDone;
  };
  plSharedPtr<BenchmarkJob> m_pBenchmark;
  plTaskGroupID m_BenchmarkTask;
};
