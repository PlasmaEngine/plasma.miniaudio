#pragma once

#include <EditorUi/Tabs.h>
#include <EditorUi/Tree.h>
#include <Foundation/Types/SharedPtr.h>
#include <Foundation/Types/UniquePtr.h>
#include <EditorUi/Content.h>
#include <GameUiPlugin/Elements/UiDrawList.h>

struct plAudioClipPreview;
struct plEditorUiPointerEvent;
class plDocument;

/// \brief Engine-drawn body of the Audio Clip dock: the platform tabs with their "+" button, the shared time ruler,
/// the waveform with the loop region, its markers and the play cursor, and the stat row.
///
/// Dragging across the waveform creates a loop, dragging a marker moves it. On release the window's
/// m_OnLoopChanged writes LoopStart, LoopEnd and OverrideLoop as one transaction.
class plEditorUiAudioClipWaveform : public plEditorUiContent
{
public:
  plEditorUiAudioClipWaveform();
  ~plEditorUiAudioClipWaveform() override;

  /// One stat of the row under the waveform: "Resident 64 KB per voice".
  struct Stat
  {
    plString m_sLabel;
    plString m_sValue;
    plString m_sToolTip;
  };

  void SetPreview(const plSharedPtr<plAudioClipPreview>& pPreview);
  void SetMessage(plStringView sMessage);
  void SetPlayCursor(plInt64 iFrame);
  void SetTabs(plArrayPtr<const plString> labels, plUInt32 uiCurrent);
  void SetStats(plArrayPtr<const Stat> stats);

  /// The tab the user picked.
  plDelegate<void(plUInt32)> m_OnTabPicked;

  /// "+" was clicked; the position is where its menu opens, in surface units.
  plDelegate<void(const plVec2&)> m_OnAddTab;

  /// A tab was right-clicked.
  plDelegate<void(plUInt32, const plVec2&)> m_OnTabMenu;

  /// A loop drag ended: start and end frame of the built clip.
  plDelegate<void(plUInt64, plUInt64)> m_OnLoopChanged;

  plString GetToolTipAt(const plVec2& vLogical) const override;

protected:
  void OnUiUpdate() override;
  void OnUiEvent(const plMsgUiEvent& event) override;

private:
  enum class Drag
  {
    None,
    LoopStart,
    LoopEnd,
    Create
  };

  void BuildDocument();
  void Draw();
  void DrawStats();
  void OnPointer(const plEditorUiPointerEvent& e);
  void OnTabChanged(plUInt32 uiTab);

  plRectFloat GetWaveRect() const;
  plRectFloat GetPlotRect() const;
  float FrameToX(plUInt64 uiFrame) const;
  plUInt64 XToFrame(float x) const;
  bool GetLoop(plUInt64& out_uiStart, plUInt64& out_uiEnd) const;
  void UpdateBars(float fPlotWidth);

  plSharedPtr<plAudioClipPreview> m_pPreview;
  plString m_sMessage;
  plInt64 m_iPlayCursor = -1;
  plDynamicArray<Stat> m_Stats;
  plDynamicArray<plString> m_TabLabels;
  plUInt32 m_uiCurrentTab = 0;

  Drag m_Drag = Drag::None;
  plUInt64 m_uiDragAnchor = 0;
  plUInt64 m_uiDragStart = 0;
  plUInt64 m_uiDragEnd = 0;

  /// Bar heights (peak, 0..1): m_uiBarCount per channel, channel after channel.
  plDynamicArray<float> m_Bars;
  plUInt32 m_uiBarCount = 0;
  const plAudioClipPreview* m_pBarsPreview = nullptr;
  plUInt64 m_uiBarsHash = 0;
  float m_fBarsWidth = 0.0f;

  plUniquePtr<plEditorUiTabs> m_pTabs;
  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
  plUiDrawListWriter m_StatsWriter;
  plEventSubscriptionID m_PointerSubscription = 0;
  plEventSubscriptionID m_TabSubscription = 0;
};

/// \brief Engine-drawn "Used by" list of the Clip Properties dock: a section header with the count and one row per
/// asset that references the clip. Double-click (or Enter) opens the asset.
class plEditorUiAudioClipUsedBy : public plEditorUiContent
{
public:
  plEditorUiAudioClipUsedBy();
  ~plEditorUiAudioClipUsedBy() override;

  /// One referencing asset: its data-directory relative path and absolute path.
  struct Entry
  {
    plString m_sPath;
    plString m_sAbsPath;
  };

  void SetEntries(plArrayPtr<const Entry> entries);

  static constexpr float Height = 170.0f;

protected:
  void OnUiUpdate() override;

private:
  class Adapter;

  void BuildDocument();

  plDynamicArray<Entry> m_Entries;
  plUniquePtr<Adapter> m_pAdapter;
  plUniquePtr<plEditorUiTree> m_pTree;
  bool m_bRebuild = true;
};
