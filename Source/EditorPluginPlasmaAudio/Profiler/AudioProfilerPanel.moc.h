#pragma once

#include <EditorFramework/IPC/EngineProcessConnection.h>
#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <EditorPluginPlasmaAudio/Profiler/AudioProfilerView.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Types/VariantType.h>
#include <GuiFoundation/DockPanels/ApplicationPanel.moc.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilerCapture.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>

class QTimer;

/// \brief The editor's Audio Profiler panel: the PlasmaAudio state of a target, live and recorded.
///
/// The target is the editor's engine process or a Player on this computer. For the engine process the panel sets
/// PlasmaAudio.ProfilerSnapshotHz there to 10 (back to 0 when it closes) and polls its stats map with
/// plStatsSnapshotMsgToEngine every 250 ms, with at most one request in flight. A Player is reached as a telemetry
/// client on port 1040 (its Inspector plugin sends the stats and takes the cvar). Either way the stat
/// "PlasmaAudio/VoiceSnapshot" carries a serialized plAudioProfilingSnapshot.
///
/// While Record is on, every snapshot is added to the capture, which the views draw as graphs and instance lanes. Save
/// writes it as a .plAudioCapture file, Open reads one (and stops recording). Record after Open, after a full capture or
/// after switching the target starts a new capture.
///
/// The content is the engine-drawn plEditorUiAudioProfilerView. The engine shell's "Panel.AudioProfiler" shows a view of
/// its own, which this panel feeds and polls for while it exists.
class PL_EDITORPLUGINPLASMAAUDIO_DLL plQtAudioProfilerPanel : public plQtApplicationPanel
{
  Q_OBJECT

  PL_DECLARE_SINGLETON(plQtAudioProfilerPanel);

public:
  plQtAudioProfilerPanel();
  ~plQtAudioProfilerPanel();

  static constexpr const char* ShellPanelId = "Panel.AudioProfiler";

protected:
  virtual void ToolsProjectEventHandler(const plToolsProjectEvent& e) override;

private Q_SLOTS:
  void OnTick();

private:
  enum class LinkState
  {
    NoEngine, ///< the target is not running or not connected
    NoData,   ///< the target answers, but without PlasmaAudio snapshot data
    Live,
  };

  enum class Target
  {
    EngineProcess,
    Player,
  };

  void EngineProcessMsgHandler(const plEditorEngineProcessConnection::Event& e);

  /// \brief Registers the engine shell's panel (again, which replaces it).
  void RegisterShellPanel();

  /// \brief The engine process connection when it can take messages, else null.
  static plEditorEngineProcessConnection* GetLiveConnection();

  /// \brief Sends PlasmaAudio.ProfilerSnapshotHz to the target.
  void SendSnapshotRate(plInt32 iHz);

  void TickEngineProcess();
  void TickPlayer();

  /// One received event log entry, or a marker for lost entries (m_uiType == LogGapType).
  using LogLine = plAudioProfilerLogLine;

  static constexpr plUInt8 LogGapType = plAudioProfilerLogLine::GapType;

  void ApplyStats(const plVariant& stats);

  /// A serialized snapshot arrived from the target.
  void ApplySnapshotData(const plDataBuffer& data);

  void SetLinkState(LinkState state);
  void UpdateLinkIndicator();

  /// The log entries of m_Snapshot that were not seen yet, with a gap marker for lost ones.
  void CollectNewLogLines(plHybridArray<LogLine, 32>& out_lines);

  void SetTarget(Target target);
  void SetRecording(bool bRecording);
  void SaveCapture();
  void OpenCapture();
  void SelectEntity(plStringView sEntity);
  void OpenEvent(plStringView sEvent);
  void CaptureChanged();
  void ShowToast(plStringView sText);

  /// The engine shell's view: fed by this panel until it goes.
  plUniquePtr<plEditorUiContent> CreateShellView();
  void OnShellViewDestroyed(plEditorUiAudioProfilerView* pView);

  /// Hooks a view to this panel and gives it the current state.
  void ConnectView(plEditorUiAudioProfilerView* pView);

  /// Whether a view shows the snapshots: this panel, open and visible, or the engine shell's.
  bool IsShown() const;

  /// This panel's view and the engine shell's.
  void GatherViews(plHybridArray<plEditorUiAudioProfilerView*, 2>& out_views) const;

  plEditorUiAudioProfilerView* m_pView = nullptr; ///< the engine-drawn content
  plHybridArray<plEditorUiAudioProfilerView*, 1> m_ShellViews;

  // event log
  plUInt64 m_uiLogNextSequence = 0; ///< sequence number of the next entry not seen yet
  bool m_bLogSequenceKnown = false;

  QTimer* m_pTimer = nullptr;

  plAudioProfilingSnapshot m_Snapshot;
  bool m_bHasSnapshot = false;

  // capture
  plAudioProfilerCapture m_Capture;
  bool m_bRecording = false;
  bool m_bCaptureFromFile = false;
  plTime m_CaptureStart;
  double m_fLastCapturedTime = -1.0; ///< the snapshot time of the last recorded snapshot

  Target m_Target = Target::EngineProcess;
  LinkState m_LinkState = LinkState::NoEngine;
  bool m_bSnapshotsEnabled = false; ///< PlasmaAudio.ProfilerSnapshotHz was set to a rate in the current target
  bool m_bRequestPending = false;
  plTime m_LastRequest;

  // Player
  plString m_sPlayerAddress = "127.0.0.1";
  bool m_bTelemetryOpen = false;
  plTime m_LastConnectAttempt;
  plTime m_LastPlayerData;
};
