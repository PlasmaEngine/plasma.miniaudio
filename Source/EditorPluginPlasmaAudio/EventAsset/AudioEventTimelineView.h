#pragma once

#include <EditorUi/Tabs.h>
#include <Foundation/Containers/Map.h>
#include <Foundation/Types/UniquePtr.h>
#include <Foundation/Types/Uuid.h>
#include <GameUiPlugin/Elements/UiDrawList.h>
#include <GuiFoundation/EngineUi/EngineUiWidget.h>

class plDocument;
class plDocumentObject;
struct plEditorUiKeyEvent;
struct plEditorUiPointerEvent;

/// \brief Engine-drawn instrument timeline of the Audio Event window, with the parameter tabs above it.
///
/// One row per instrument: a header with the track colour, name, routing and state, the M (saved Mute) and S
/// (session solo) toggles, and the clip block at its start delay. Below the rows the Logic lane shows the fade in
/// and stop fade and the Automation lane one chip per automation. Clicking selects, dragging a clip moves its
/// start delay (10 ms snap), dragging the loop and fade handles edits them, double-clicking a header renames the
/// instrument. Every drag is one undo step, written on release.
class plQtAudioEventTimelineView : public plQtEngineUiWidget
{
public:
  plQtAudioEventTimelineView(QWidget* pParent, plDocument* pDocument);
  ~plQtAudioEventTimelineView() override;

  /// Re-reads the document: instruments, parameters, markers and the selection.
  void Refresh();

  /// Forgets the measured lengths of embedded sound files (one of them changed).
  void ClearDurationCache();

  /// Session-only solo: while any track is solo, auditioning plays only solo tracks.
  bool IsSolo(const plUuid& instrumentGuid) const { return m_SoloTracks.Contains(instrumentGuid); }
  bool HasSolo() const { return !m_SoloTracks.IsEmpty(); }

  /// The "+ Parameter" tab was clicked.
  plDelegate<void()> m_OnAddParameter;

  QSize sizeHint() const override { return QSize(800, 320); }
  QSize minimumSizeHint() const override { return QSize(300, 140); }

protected:
  void OnUiUpdate() override;
  void OnUiEvent(const plMsgUiEvent& event) override;

private:
  enum class DragHandle
  {
    None,
    LoopStart,
    LoopEnd,
    FadeIn,
    FadeOut
  };

  enum class HitKind
  {
    Clip,
    Header,
    Mute,
    Solo,
    Chip
  };

  struct Hit
  {
    HitKind m_Kind = HitKind::Clip;
    plUuid m_Guid;
    plRectFloat m_Rect;
  };

  const plDocumentObject* GetPropertiesObject() const;
  void GatherInstruments(plHybridArray<const plDocumentObject*, 8>& out_instruments) const;
  double GetSampleDuration(plStringView sFile) const;
  double GetEntryDuration(const plDocumentObject* pEntry) const;
  double GetLongestEntry(const plDocumentObject* pInstrument) const;
  double GetInstrumentDelay(const plDocumentObject* pInstrument) const;
  double GetMarkerValue(const char* szProperty, DragHandle handle) const;
  double GetTimeExtent() const;

  float GetCanvasTop() const;
  float TimeToX(double fSeconds) const;
  double XToTime(float x) const;
  float GetRowTop(plUInt32 uiRow) const;
  plVec2 GetContentSize() const;
  void ClampScroll();

  void BuildDocument();
  void UpdateTabs();
  void Draw();
  void DrawRuler(const plRectFloat& canvas, double fExtent, bool bLoopRegion);
  void DrawTrack(const plDocumentObject* pInstrument, plUInt32 uiIndex, bool bAnySolo, const plString& sOutputBus, const plUuid& selected);
  void DrawLanes(plUInt32 uiInstruments, const plUuid& selected);

  void OnPointer(const plEditorUiPointerEvent& e);
  void OnKey(const plEditorUiKeyEvent& e);
  void OnTabChanged(plUInt32 uiTab);
  const Hit* FindHit(const plVec2& vPos) const;
  DragHandle PickHandle(const plVec2& vPos) const;
  void ApplyHandleDrag(DragHandle handle, float x, bool bFinal);
  void ToggleMute(const plUuid& instrumentGuid);
  void ToggleSolo(const plUuid& instrumentGuid);

  void BeginRename(const plUuid& instrumentGuid);
  void CommitRename();
  void CancelRename();

  plDocument* m_pDocument = nullptr;

  plDynamicArray<Hit> m_Hits;
  mutable plMap<plString, double> m_DurationCache; ///< file path -> seconds (0 = could not decode)
  plHybridArray<plUuid, 8> m_SoloTracks;

  // tabs: Timeline and one per parameter
  plUniquePtr<plEditorUiTabs> m_pTabs;
  plHybridArray<plString, 8> m_TabLabels;
  plHybridArray<plUuid, 8> m_TabParameters; ///< invalid for the Timeline tab
  plUInt32 m_uiCurrentTab = 0;
  float m_fAddTabX = 0.0f;

  // marker drag: previewed live, written to the document once on release
  DragHandle m_ActiveHandle = DragHandle::None;
  bool m_bHandlePreviewValid = false;
  double m_fHandlePreview = 0.0;

  // clip drag: moves an instrument's StartDelay along the timeline
  plUuid m_DragClipGuid;
  bool m_bDraggingClip = false;
  double m_fDragClipDelay = 0.0;
  double m_fDragGrabOffset = 0.0;

  // inline rename of a track; it commits once its field had focus and lost it
  plUuid m_RenameGuid;
  bool m_bRenameHadFocus = false;

  float m_fScrollX = 0.0f;
  float m_fScrollY = 0.0f;

  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
  plEventSubscriptionID m_PointerSubscription = 0;
  plEventSubscriptionID m_KeySubscription = 0;
  plEventSubscriptionID m_TabSubscription = 0;
};
