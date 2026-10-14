#pragma once

#include <EditorUi/SearchField.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Types/UniquePtr.h>
#include <GameUiPlugin/Elements/UiDrawList.h>
#include <GuiFoundation/EngineUi/EngineUiWidget.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>

struct plEditorUiPointerEvent;

/// \brief One received line of the PlasmaAudio event log, or a marker for lines that were lost.
struct plAudioProfilerLogLine
{
  static constexpr plUInt8 GapType = 0xFF;

  double m_fTime = 0.0;
  plUInt8 m_uiType = 0;
  plUInt64 m_uiLost = 0; ///< lines lost before this marker (m_uiType == GapType)
  plString m_sName;
  plString m_sDetail;
};

/// \brief The engine-drawn content of the Audio Profiler panel: the link state and Pause, the tabs Voices, Buses,
/// Memory and Event log with their counts, the tab's content and the status line.
///
/// The panel polls the engine process and hands every snapshot and the new log lines to this view; the view keeps
/// the filters, the selected voice and the last 2000 log lines.
class plQtAudioProfilerView : public plQtEngineUiWidget
{
public:
  explicit plQtAudioProfilerView(QWidget* pParent);
  ~plQtAudioProfilerView() override;

  enum class LinkState : plUInt8
  {
    NoEngine, ///< the engine process is not running
    NoData,   ///< the engine process answers, but without PlasmaAudio snapshot data
    Live,
  };

  void SetLinkState(LinkState state);

  /// A new snapshot and the log lines it brought.
  void SetSnapshot(const plAudioProfilingSnapshot& snapshot, plArrayPtr<const plAudioProfilerLogLine> newLines);

  bool IsPaused() const { return m_bPaused; }

  /// Pause was toggled: the panel stops or resumes polling.
  plDelegate<void(bool)> m_OnPauseToggled;

protected:
  void OnUiUpdate() override;
  void OnUiEvent(const plMsgUiEvent& event) override;
  plString GetToolTipAt(const plVec2& vLogical) const override;

private:
  enum class Tab : plUInt8
  {
    Voices,
    Buses,
    Memory,
    Log,
    Count
  };

  /// A row of the memory table.
  struct MemoryRow
  {
    plString m_sName;
    plString m_sToolTip;
    plString m_sType;
    plString m_sLoadMode;
    plString m_sCodec;
    plString m_sStreams;
    plUInt64 m_uiResident = 0;
    bool m_bHasResident = true;
    bool m_bWarning = false;
  };

  /// The geometry of the current tab's parts, shared by building, drawing and hit testing.
  plRectFloat GetBodyRect() const;
  plRectFloat GetToolRowRect() const;
  plRectFloat GetTableRect() const;
  plRectFloat GetDetailsRect() const;
  float GetTabWidth(Tab tab) const;
  plString GetTabLabel(Tab tab) const;
  plString GetTabCount(Tab tab) const;

  void BuildDocument();
  void Draw();
  void DrawHeader();
  void DrawVoices();
  void DrawBuses();
  void DrawMemory();
  void DrawLog();
  void DrawStatus();
  void DrawTableHeader(const plRectFloat& rect, plArrayPtr<const char* const> titles, plArrayPtr<const float> widths, plArrayPtr<const bool> rightAligned);

  void RebuildVoiceRows();
  void RebuildMemoryRows();
  bool IsLogLineVisible(const plAudioProfilerLogLine& line) const;
  void RebuildVisibleLog();

  void OnPointer(const plEditorUiPointerEvent& e);
  void OnSearch(const plEditorUiSearchFieldEvent& e);
  void ClampScroll();

  // data
  plAudioProfilingSnapshot m_Snapshot;
  bool m_bHasSnapshot = false;
  LinkState m_LinkState = LinkState::NoEngine;
  bool m_bPaused = false;

  // voices
  bool m_StateFilter[3] = {true, true, true};
  plString m_sVoiceFilter;
  plDynamicArray<const plAudioVoiceSnapshotEntry*> m_VoiceRows;
  bool m_bHasSelection = false;
  plUInt32 m_uiSelectedId = 0;
  plAudioVoiceSnapshotEntry m_SelectedVoice; ///< the selected voice as last seen
  bool m_bSelectedVoiceEnded = false;

  // memory
  plDynamicArray<MemoryRow> m_MemoryRows;

  // event log
  static constexpr plUInt32 LogCategoryCount = 4;
  bool m_LogCategory[LogCategoryCount] = {true, true, true, true};
  plString m_sLogFilter;
  plDeque<plAudioProfilerLogLine> m_LogLines;
  plDynamicArray<plUInt32> m_VisibleLog; ///< indices into m_LogLines
  plUInt64 m_uiLogReceived = 0;
  bool m_bFollowLog = true;

  Tab m_Tab = Tab::Voices;
  float m_fScroll = 0.0f;
  plUniquePtr<plEditorUiSearchField> m_pVoiceSearch;
  plUniquePtr<plEditorUiSearchField> m_pLogSearch;
  plEventSubscriptionID m_VoiceSearchSubscription = 0;
  plEventSubscriptionID m_LogSearchSubscription = 0;

  float m_BuiltTabWidths[static_cast<int>(Tab::Count)] = {};
  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
  plEventSubscriptionID m_PointerSubscription = 0;
};
