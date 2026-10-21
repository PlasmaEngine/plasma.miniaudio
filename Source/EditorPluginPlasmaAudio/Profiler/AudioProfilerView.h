#pragma once

#include <EditorUi/Content.h>
#include <EditorUi/SearchField.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Types/UniquePtr.h>
#include <GameUiPlugin/Elements/UiDrawList.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilerCapture.h>
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

/// \brief The engine-drawn content of the Audio Profiler panel: Record, Save capture, Open and the target, the capture's
/// graphs (voices, DSP load, streams with underruns, memory against the budget) with a draggable time range, the tabs
/// Instances, Event log, Voices, Buses and Memory with their counts, the tab's content and the status line.
///
/// The panel polls the target and hands every snapshot and the new log lines to this view, and records them into the
/// capture the view draws. Without a range the tabs show the live state; with a range, Voices, Buses and Memory show the
/// snapshot at its end, and Instances and Event log what happened in it. The Qt panel shows one view, the engine shell's
/// Audio Profiler panel another, fed by the same panel.
class plEditorUiAudioProfilerView : public plEditorUiContent
{
public:
  plEditorUiAudioProfilerView();
  ~plEditorUiAudioProfilerView() override;

  enum class LinkState : plUInt8
  {
    NoEngine, ///< the target is not running or not connected
    NoData,   ///< the target answers, but without PlasmaAudio snapshot data
    Live,
  };

  void SetLinkState(LinkState state);

  /// What the profiler listens to, as the header shows it ("Editor engine process", "Player · 127.0.0.1").
  void SetTargetText(plStringView sText);

  /// A new live snapshot and the log lines it brought.
  void SetSnapshot(const plAudioProfilingSnapshot& snapshot, plArrayPtr<const plAudioProfilerLogLine> newLines);

  /// The capture the graphs and the Instances tab draw; the panel owns it.
  void SetCapture(const plAudioProfilerCapture* pCapture);

  /// The capture changed (a snapshot was recorded, a file was opened, it was cleared).
  void OnCaptureChanged();

  void SetRecording(bool bRecording);

  /// A line for the status row that goes away after a few seconds ("Saved ...").
  void ShowToast(plStringView sText);

  /// Record was toggled.
  plDelegate<void(bool)> m_OnRecordToggled;

  /// Save capture, Open... and the target menu's choice (0: editor engine process, 1: Player on this computer).
  plDelegate<void()> m_OnSave;
  plDelegate<void()> m_OnOpen;
  plDelegate<void(plUInt32)> m_OnTargetChosen;

  /// The selected instance's Select entity (the entity's name) and Open event (the event's resource name).
  plDelegate<void(plStringView)> m_OnSelectEntity;
  plDelegate<void(plStringView)> m_OnOpenEvent;

  /// The view goes (the engine shell closed its panel): the panel stops feeding it.
  plDelegate<void(plEditorUiAudioProfilerView*)> m_OnDestroyed;

  plString GetToolTipAt(const plVec2& vLogical) const override;

protected:
  void OnUiUpdate() override;
  void OnUiEvent(const plMsgUiEvent& event) override;

private:
  enum class Tab : plUInt8
  {
    Instances,
    Log,
    Voices,
    Buses,
    Memory,
    Count
  };

  /// The instances of one event in the range, in start order, as one lane.
  struct Lane
  {
    plString m_sEvent;
    plHybridArray<plUInt32, 8> m_Instances; ///< indices into the capture's instances
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
  void DrawGraphs();
  void DrawInstances();
  void DrawInstanceCard(const plRectFloat& rect);
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

  /// The capture's samples, lanes and log for the range; the shown snapshot is the range's last one, or the live one.
  void ApplyRange();
  void RebuildLanes();

  /// The capture time range the tabs show: the selection, or the whole capture.
  void GetRange(double& out_fStart, double& out_fEnd) const;
  bool HasRange() const { return m_fRangeA >= 0.0 && m_fRangeB >= 0.0 && plMath::Abs(m_fRangeA - m_fRangeB) >= 0.05; }

  /// The graphs' plot area (right of the row titles) and the capture time at an x in it.
  plRectFloat GetGraphRect() const;
  float GetGraphHeight() const;
  double GraphTimeAt(float x) const;
  float GraphXAt(double fTime) const;

  /// The log the Event log tab shows: the live lines, or the capture's lines in the range.
  const plDeque<plAudioProfilerLogLine>& GetShownLog() const { return HasRange() ? m_RangeLog : m_LogLines; }

  void ShowTargetMenu();

  void OnPointer(const plEditorUiPointerEvent& e);
  void OnSearch(const plEditorUiSearchFieldEvent& e);
  void ClampScroll();

  // data
  plAudioProfilingSnapshot m_Snapshot;     ///< the snapshot the tabs show
  plAudioProfilingSnapshot m_LiveSnapshot; ///< the last live snapshot
  bool m_bHasSnapshot = false;
  bool m_bHasLiveSnapshot = false;
  LinkState m_LinkState = LinkState::NoEngine;
  plString m_sTargetText = "Editor engine process";

  // capture
  const plAudioProfilerCapture* m_pCapture = nullptr;
  bool m_bRecording = false;
  double m_fRangeA = -1.0;
  double m_fRangeB = -1.0;
  bool m_bDraggingRange = false;
  double m_fHoverTime = -1.0;
  plDynamicArray<Lane> m_Lanes;
  plInt32 m_iSelectedInstance = -1;
  plDeque<plAudioProfilerLogLine> m_RangeLog;
  plString m_sToast;
  plTime m_ToastTime;

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

  Tab m_Tab = Tab::Instances;
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
