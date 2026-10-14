#pragma once

#include <EditorUi/Tree.h>
#include <Foundation/Types/UniquePtr.h>
#include <Foundation/Types/Uuid.h>
#include <GameUiPlugin/Elements/UiDrawList.h>
#include <GuiFoundation/EngineUi/EngineUiWidget.h>

class plDocument;
class plDocumentObject;
struct plEditorUiPointerEvent;

/// \brief The engine-drawn mixer console: one channel strip per bus and, after a dashed divider, one per VCA.
///
/// A strip has a header with the system colour as its left edge (Audio for buses, Scripting for aux buses,
/// Utilities for VCAs), the inserts (or the controlled buses of a VCA), the sends (or the environment of an aux
/// bus), a fader from -60 to +12 dB with its scale, the bus meter on the dBFS scale, the readout and the routing.
/// Clicking selects the bus or VCA; dragging a fader snaps to 0.5 dB with a detent at 0 dB and writes one
/// "Set Volume" step on release.
class plQtAudioMixerStripsView : public plQtEngineUiWidget
{
public:
  plQtAudioMixerStripsView(QWidget* pParent, plDocument* pDocument);
  ~plQtAudioMixerStripsView() override;

  /// Re-reads the document.
  void Refresh();

  /// Whether the meters show the editor preview's bus levels.
  void SetLiveMeters(bool bLive);

  /// Redraws the meters (called while the preview runs).
  void UpdateMeters();

  QSize sizeHint() const override { return QSize(800, 420); }
  QSize minimumSizeHint() const override { return QSize(200, 200); }

protected:
  void OnUiUpdate() override;

private:
  struct Strip
  {
    plUuid m_Guid;
    plRectFloat m_Rect;
    plRectFloat m_Fader;
    bool m_bVca = false;
  };

  const plDocumentObject* GetPropertiesObject() const;
  void GatherStrips(plHybridArray<const plDocumentObject*, 12>& out_buses, plHybridArray<const plDocumentObject*, 4>& out_vcas) const;
  float GetContentWidth() const;

  void BuildDocument();
  void Draw();
  void DrawStrip(const plRectFloat& rect, const plDocumentObject* pObject, plUInt32 uiBusIndex, bool bVca, bool bMaster, bool bSelected);
  void OnPointer(const plEditorUiPointerEvent& e);

  plDocument* m_pDocument = nullptr;
  plDynamicArray<Strip> m_Strips;
  bool m_bLiveMeters = true;
  float m_fScrollX = 0.0f;

  // fader drag: previewed, written on release
  plUuid m_DragGuid;
  plRectFloat m_DragFader;
  bool m_bDragging = false;
  float m_fDragValueDb = 0.0f;

  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
  plEventSubscriptionID m_PointerSubscription = 0;
};

/// \brief The Routing dock: the buses as a hierarchy (parents by name, unknown parents under the master bus), the
/// VCAs and the snapshots, each under its group. Clicking selects the object.
class plQtAudioMixerRoutingView : public plQtEngineUiWidget
{
public:
  plQtAudioMixerRoutingView(QWidget* pParent, plDocument* pDocument);
  ~plQtAudioMixerRoutingView() override;

  /// Re-reads the document and shows the document's selection.
  void Refresh();

protected:
  void OnUiUpdate() override;

private:
  class Adapter;

  void BuildDocument();
  void OnTreeEvent(const plEditorUiTreeEvent& e);

  plDocument* m_pDocument = nullptr;
  plUniquePtr<Adapter> m_pAdapter;
  plUniquePtr<plEditorUiTree> m_pTree;
  bool m_bRebuild = true;
  bool m_bSyncingSelection = false;
  plEventSubscriptionID m_TreeSubscription = 0;
};

/// \brief The Snapshots dock: one row per snapshot with its name and "3 overrides · in 0.5s / out 0.5s · prio 10".
/// Clicking selects the snapshot.
class plQtAudioMixerSnapshotsView : public plQtEngineUiWidget
{
public:
  plQtAudioMixerSnapshotsView(QWidget* pParent, plDocument* pDocument);
  ~plQtAudioMixerSnapshotsView() override;

  /// Re-reads the document.
  void Refresh();

protected:
  void OnUiUpdate() override;

private:
  void BuildDocument();
  void Draw();
  void OnPointer(const plEditorUiPointerEvent& e);

  plDocument* m_pDocument = nullptr;
  plDynamicArray<plUuid> m_Rows;
  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
  plEventSubscriptionID m_PointerSubscription = 0;
};
