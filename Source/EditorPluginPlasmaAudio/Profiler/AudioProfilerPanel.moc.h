#pragma once

#include <EditorFramework/IPC/EngineProcessConnection.h>
#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Time/Time.h>
#include <EditorPluginPlasmaAudio/Profiler/AudioProfilerView.h>
#include <GuiFoundation/DockPanels/ApplicationPanel.moc.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>

class QTimer;

/// \brief The editor's Audio Profiler panel, showing the PlasmaAudio state of the engine process on four tabs:
/// - Voices: the active voices with their state, priority, audibility, distance, bus and codec.
/// - Buses: a strip per mixer bus with its RMS and peak meter and gain, and the active mixer snapshots.
/// - Memory: the loaded clips, events and banks with their resident bytes and open streams.
/// - Event log: instance starts and stops, stealing, voice changes, clip problems, device, mixer, bank and language
///   changes, received through the sequence-numbered log of the snapshots.
///
/// While the panel is open and the engine process runs, it sets PlasmaAudio.ProfilerSnapshotHz in the engine process
/// to 10 (back to 0 when it closes) and polls the engine's stats map with plStatsSnapshotMsgToEngine every 250 ms, with
/// at most one request in flight. The stat "PlasmaAudio/VoiceSnapshot" carries a serialized plAudioProfilingSnapshot.
/// Pause freezes every tab and stops polling; log entries produced meanwhile are reported as lost.
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

private Q_SLOTS:
  void OnTick();
  void OnPauseToggled(bool bPaused);

private:
  enum class LinkState
  {
    NoEngine,   ///< the engine process is not running
    NoData,     ///< the engine process answers, but without PlasmaAudio snapshot data
    Live,
  };

  void EngineProcessMsgHandler(const plEditorEngineProcessConnection::Event& e);

  /// \brief The engine process connection when it can take messages, else null.
  static plEditorEngineProcessConnection* GetLiveConnection();

  /// \brief Sends PlasmaAudio.ProfilerSnapshotHz to the engine process.
  void SendSnapshotRate(plInt32 iHz);

  /// One received event log entry, or a marker for lost entries (m_uiType == LogGapType).
  using LogLine = plAudioProfilerLogLine;

  static constexpr plUInt8 LogGapType = plAudioProfilerLogLine::GapType;

  void ApplyStats(const plVariant& stats);
  void UpdateLinkIndicator();

  /// The log entries of m_Snapshot that were not seen yet, with a gap marker for lost ones.
  void CollectNewLogLines(plHybridArray<LogLine, 32>& out_lines);

  /// The engine shell's view: fed by this panel until it goes.
  plUniquePtr<plEditorUiContent> CreateShellView();
  void OnShellViewDestroyed(plEditorUiAudioProfilerView* pView);

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

  LinkState m_LinkState = LinkState::NoEngine;
  bool m_bSnapshotsEnabled = false; ///< PlasmaAudio.ProfilerSnapshotHz was set to a rate in the current engine process
  bool m_bRequestPending = false;
  bool m_bPaused = false;
  plTime m_LastRequest;
};
