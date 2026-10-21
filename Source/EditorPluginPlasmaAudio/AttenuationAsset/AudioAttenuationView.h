#pragma once

#include <EditorUi/Content.h>
#include <Foundation/Types/Uuid.h>
#include <GameUiPlugin/Elements/UiDrawList.h>

class plDocument;
class plDocumentObject;
struct plAudioEventDesc;
struct plDocumentObjectPropertyEvent;
struct plDocumentObjectStructureEvent;
struct plEditorUiPointerEvent;

/// \brief The ATTENUATION panel, engine-drawn: the curve dropdown, the gain over distance in dB with draggable min and
/// max distance handles (and the points of a custom curve), and the listener marker.
///
/// It edits Curve, MinDistance, MaxDistance and CurvePoints of the document's properties object, an Audio Event or an
/// Audio Attenuation. An event that uses a shared Audio Attenuation shows that asset's curve, read-only.
///
/// A drag is previewed and written on release, one undo step each. Double-click adds a point to a custom curve and
/// right-click removes one. Dragging the listener marker reports the distance (m_OnListenerDistanceChanged).
class plEditorUiAudioAttenuation : public plEditorUiContent
{
public:
  explicit plEditorUiAudioAttenuation(plDocument* pDocument);
  ~plEditorUiAudioAttenuation() override;

  /// The distance the listener marker shows.
  void SetListenerDistance(float fDistance);

  /// The listener marker was dragged.
  plDelegate<void(float)> m_OnListenerDistanceChanged;

  /// The least height that shows the whole panel.
  static float GetMinimumHeight();

  plString GetToolTipAt(const plVec2& vLogical) const override;

protected:
  void OnUiUpdate() override;
  void OnUiEvent(const plMsgUiEvent& event) override;

private:
  enum class Drag
  {
    None,
    MinDistance,
    MaxDistance,
    Point,
    Listener
  };

  struct CurvePoint
  {
    plUuid m_Guid;
    float m_fX = 0.0f;
    float m_fY = 0.0f;
  };

  const plDocumentObject* GetPropertiesObject() const;

  /// The curve as it is drawn: the document's values with the drag preview, or the shared asset's. False without a
  /// properties object.
  bool GetCurve(plAudioEventDesc& out_desc, plString& out_sSharedName) const;
  void GatherPoints(plHybridArray<CurvePoint, 8>& out_points) const;
  float GetRange() const; ///< the distance at the right edge of the plot

  plRectFloat GetPlotRect() const;
  float DistanceToX(float fDistance) const;
  float XToDistance(float x) const;
  float GainToY(float fGain) const;
  float YToGain(float y) const;
  Drag PickHandle(const plVec2& vPos, plUuid& out_pointGuid) const;

  void BuildDocument();
  void Draw();
  void ShowCurveMenu();
  void SetCurve(plInt64 iCurve);
  void OnPointer(const plEditorUiPointerEvent& e);
  void CommitDrag();

  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);

  plDocument* m_pDocument = nullptr;
  float m_fListenerDistance = 0.0f;

  // a drag in progress: previewed, written on release
  Drag m_Drag = Drag::None;
  plUuid m_DragPointGuid;
  float m_fDragDistance = 0.0f;
  float m_fDragX = 0.0f;
  float m_fDragY = 0.0f;
  float m_fDragRange = 0.0f; ///< the plot's range when the drag started, so the axis holds still

  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
  plEventSubscriptionID m_PointerSubscription = 0;
};
