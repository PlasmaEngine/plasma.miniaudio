#pragma once

#include <EditorFramework/IPC/EngineProcessConnection.h>
#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Time/Time.h>
#include <EditorPluginPlasmaAudio/Profiler/AudioProfilerView.h>
#include <GuiFoundation/DockPanels/ApplicationPanel.moc.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>

#include <QColor>

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QTabBar;
class QTableWidget;
class QTimer;
class QToolButton;
class plQtAudioBusStripsWidget;

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
/// The content is the engine-drawn plEditorUiAudioProfilerView; "-engineAudioEditors false" creates the Qt widgets
/// instead. The engine shell's "Panel.AudioProfiler" shows a view of its own, which this panel feeds and polls for while
/// it exists.
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
  void OnFilterChanged();
  void OnSelectionChanged();
  void OnTabChanged(int iTab);
  void OnLogFilterChanged();
  void OnLogClear();

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

  void CreateQtContent();

  QWidget* CreateBusesPage(QWidget* pParent);
  QWidget* CreateMemoryPage(QWidget* pParent);
  QWidget* CreateLogPage(QWidget* pParent);

  void ApplyStats(const plVariant& stats);
  void RebuildTable();
  void UpdateDetails();
  void UpdateStatusLine();
  void UpdateLinkIndicator();
  void RebuildBuses();
  void RebuildMemory();

  /// The log entries of m_Snapshot that were not seen yet, with a gap marker for lost ones.
  void CollectNewLogLines(plHybridArray<LogLine, 32>& out_lines);

  /// Appends the log entries of m_Snapshot that were not seen yet.
  void AppendLogEntries();

  /// Whether the line passes the log filter box and type pills.
  bool IsLogLineVisible(const LogLine& line) const;

  /// Adds the line as the last row of the log table; drops the first row beyond the row limit.
  void AddLogRow(const LogLine& line);

  void RebuildLogTable();
  void UpdateTabTexts();

  /// The engine shell's view: fed by this panel until it goes.
  plUniquePtr<plEditorUiContent> CreateShellView();
  void OnShellViewDestroyed(plEditorUiAudioProfilerView* pView);

  /// Whether a view shows the snapshots: this panel, open and visible, or the engine shell's.
  bool IsShown() const;

  /// This panel's view and the engine shell's.
  void GatherViews(plHybridArray<plEditorUiAudioProfilerView*, 2>& out_views) const;

  plEditorUiAudioProfilerView* m_pView = nullptr; ///< the engine-drawn content; null with the Qt widgets
  plHybridArray<plEditorUiAudioProfilerView*, 1> m_ShellViews;

  QLabel* m_pLinkDot = nullptr;
  QLabel* m_pLinkText = nullptr;
  QToolButton* m_pPauseButton = nullptr;
  QTabBar* m_pTabs = nullptr;
  QStackedWidget* m_pPages = nullptr;

  // Buses tab
  QHBoxLayout* m_pSnapshotPills = nullptr;
  QLabel* m_pNoSnapshots = nullptr;
  QLabel* m_pMasterLevel = nullptr;
  plQtAudioBusStripsWidget* m_pStrips = nullptr;
  QString m_sSnapshotPillsKey; ///< the active snapshots the pills show, to rebuild them only on change

  // Memory tab
  QTableWidget* m_pMemoryTable = nullptr;
  QLabel* m_pMemoryTotals = nullptr;

  // Event log tab
  static constexpr plUInt32 LogCategoryCount = 4; ///< type filter pills: lifecycle, voices, warnings, system
  QToolButton* m_pLogCategory[LogCategoryCount] = {};
  QLineEdit* m_pLogFilter = nullptr;
  QTableWidget* m_pLogTable = nullptr;
  plDeque<LogLine> m_LogLines; ///< the newest received lines, at most the row limit
  plUInt64 m_uiLogNextSequence = 0; ///< sequence number of the next entry not seen yet
  bool m_bLogSequenceKnown = false;
  plUInt64 m_uiLogReceived = 0;
  QToolButton* m_pStateFilter[3] = {};
  QLineEdit* m_pEventFilter = nullptr;
  QTableWidget* m_pTable = nullptr;
  QLabel* m_pDetailsTitle = nullptr;
  QLabel* m_pDetailsHint = nullptr;
  QWidget* m_pDetailsGrid = nullptr;
  QLabel* m_pDetailValues[9] = {};
  QLabel* m_pStatusLine = nullptr;
  QTimer* m_pTimer = nullptr;
  QColor m_Accent;

  plAudioProfilingSnapshot m_Snapshot;
  bool m_bHasSnapshot = false;

  /// the voice ids of the table rows, in row order
  plDynamicArray<plUInt32> m_RowIds;

  plUInt32 m_uiSelectedId = 0;
  bool m_bHasSelection = false;
  plAudioVoiceSnapshotEntry m_SelectedVoice; ///< the selected voice as last seen
  bool m_bSelectedVoiceEnded = false;

  LinkState m_LinkState = LinkState::NoEngine;
  bool m_bSnapshotsEnabled = false; ///< PlasmaAudio.ProfilerSnapshotHz was set to a rate in the current engine process
  bool m_bRequestPending = false;
  bool m_bPaused = false;
  bool m_bUpdatingSelection = false;
  plTime m_LastRequest;
};
