#pragma once

#include <Foundation/Types/Uuid.h>
#include <GameUiPlugin/Elements/UiDrawList.h>
#include <GuiFoundation/EngineUi/EngineUiWidget.h>

class plDocument;
class plDocumentObject;
struct plEditorUiPointerEvent;

/// \brief The upper row of the Audio Event deck, engine-drawn: the INSTRUMENT column (instrument dropdown, playlist
/// rows with weight, clip picker and remove, the add buttons) and the PLAYBACK column (playlist mode, volume and
/// pitch randomization, loop, start delay, retrigger gap).
///
/// Number fields commit on Enter or when they lose focus, sliders on release: one undo step per edit or drag.
class plQtAudioEventDeckView : public plQtEngineUiWidget
{
public:
  plQtAudioEventDeckView(QWidget* pParent, plDocument* pDocument);
  ~plQtAudioEventDeckView() override;

  /// The instrument the deck edits (invalid: none) and a re-read of the document.
  void SetInstrument(const plUuid& instrumentGuid);

  /// An instrument was picked in the dropdown.
  plDelegate<void(const plUuid&)> m_OnInstrumentPicked;

  /// The add buttons under the playlist.
  plDelegate<void()> m_OnAddFiles;
  plDelegate<void()> m_OnAddClip;
  plDelegate<void()> m_OnAddSilence;

  /// "..." of a playlist entry: pick its Audio Clip.
  plDelegate<void(const plUuid&)> m_OnPickEntryClip;

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

protected:
  void OnUiUpdate() override;
  void OnUiEvent(const plMsgUiEvent& event) override;
  plString GetToolTipAt(const plVec2& vLogical) const override;

private:
  enum class Slider
  {
    None,
    Volume,
    Pitch
  };

  const plDocumentObject* GetInstrument() const;
  void GatherPlaylist(plHybridArray<const plDocumentObject*, 8>& out_entries) const;
  float GetContentHeight() const;
  float GetColumnSplit() const;
  plRectFloat GetSliderRect(Slider slider) const;

  void BuildDocument();
  void Draw();
  void DrawInstrumentColumn(float fWidth);
  void DrawPlaybackColumn(float x, float fWidth);
  void UpdateFieldTexts();

  void OnPointer(const plEditorUiPointerEvent& e);
  void CommitField(plStringView sField);
  void SetInstrumentValue(const char* szProperty, const plVariant& value, const char* szTransaction);
  void ShowInstrumentMenu();
  void ShowModeMenu();

  plDocument* m_pDocument = nullptr;
  plUuid m_InstrumentGuid;
  plUInt32 m_uiBuiltEntries = 0xFFFFFFFF;
  bool m_bBuiltWithInstrument = false;

  Slider m_DragSlider = Slider::None;
  float m_fDragValue = 0.0f;

  plString m_sFocusedField; ///< the number field being edited; it commits when focus leaves
  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
  plEventSubscriptionID m_PointerSubscription = 0;
};

/// \brief The AUTOMATION column of the Audio Event deck, engine-drawn: the target line, the curve of the selected
/// automation and "+ automate property...".
///
/// Dragging a point is one "Move Curve Point" undo step, written on release; double-click adds a point, right-click
/// removes one (two stay).
class plQtAudioEventAutomationView : public plQtEngineUiWidget
{
public:
  plQtAudioEventAutomationView(QWidget* pParent, plDocument* pDocument);
  ~plQtAudioEventAutomationView() override;

  /// The automation the curve shows (invalid: none) and a re-read of the document.
  void SetAutomation(const plUuid& automationGuid);

  /// "+ automate property..." picked a target (plAudioEventAutomationTarget) and a parameter.
  plDelegate<void(plUInt8, const plString&)> m_OnAddAutomation;

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

protected:
  void OnUiUpdate() override;
  void OnUiEvent(const plMsgUiEvent& event) override;
  plString GetToolTipAt(const plVec2& vLogical) const override;

private:
  struct CurvePoint
  {
    plUuid m_Guid;
    double m_fX = 0.0;
    double m_fY = 1.0;
  };

  void GatherPoints(plHybridArray<CurvePoint, 8>& out_points) const;
  void GetRanges(double& out_fMinX, double& out_fMaxX, double& out_fMaxY) const;
  plRectFloat GetCurveRect() const;
  plRectFloat GetPlotRect() const;
  plVec2 ToView(double fX, double fY) const;
  void FromView(const plVec2& vPos, double& out_fX, double& out_fY) const;
  plInt32 FindPointAt(const plVec2& vPos) const;

  void BuildDocument();
  void Draw();
  void OnPointer(const plEditorUiPointerEvent& e);
  void ShowAddMenu();

  plDocument* m_pDocument = nullptr;
  plUuid m_AutomationGuid;

  // a point being dragged: previewed, written on release
  plUuid m_DragPointGuid;
  bool m_bDragging = false;
  double m_fDragX = 0.0;
  double m_fDragY = 0.0;

  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
  plEventSubscriptionID m_PointerSubscription = 0;
};
