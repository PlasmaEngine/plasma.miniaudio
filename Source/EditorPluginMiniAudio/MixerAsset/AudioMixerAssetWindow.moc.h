#pragma once

#include <EditorPluginMiniAudio/MixerAsset/AudioMixerAsset.h>
#include <Foundation/Basics.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.moc.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
struct plDocumentObjectPropertyEvent;
struct plDocumentObjectStructureEvent;
struct plSelectionManagerEvent;

/// \brief Custom-painted mixer console: one channel strip per bus (fader, insert chips, sends),
/// dashed strips for VCAs. Clicking selects the bus/VCA object; dragging a fader edits its volume.
class plQtAudioMixerStripsWidget : public QWidget
{
  Q_OBJECT

public:
  plQtAudioMixerStripsWidget(QWidget* pParent, plDocument* pDocument, const QColor& accent);

  virtual QSize sizeHint() const override;
  virtual QSize minimumSizeHint() const override { return sizeHint(); }

protected:
  virtual void paintEvent(QPaintEvent* pEvent) override;
  virtual void mousePressEvent(QMouseEvent* pEvent) override;
  virtual void mouseMoveEvent(QMouseEvent* pEvent) override;
  virtual void mouseReleaseEvent(QMouseEvent* pEvent) override;

private:
  friend class plAudioMixerAssetDocumentWindow;

  struct StripHit
  {
    plUuid m_Guid;
    QRect m_StripRect;
    QRect m_FaderRect;  // grab area
    QRect m_FaderTrack; // actual groove for value mapping
  };

  const plDocumentObject* GetPropertiesObject() const;
  void GatherStrips(plHybridArray<const plDocumentObject*, 12>& out_buses, plHybridArray<const plDocumentObject*, 4>& out_vcas) const;

  /// \brief Live level of the bus at this strip index, from the editor preview engine.
  void GetLiveMeter(plUInt32 uiBusIndex, float& out_fPeak, float& out_fRms) const;

  static float PosToDb(const QRect& faderRect, int y);
  static int DbToPos(const QRect& faderRect, float fDb);

  void DrawStrip(QPainter& p, const QRect& stripRect, const plDocumentObject* pObject, plUInt32 uiBusIndex, bool bVca, bool bMaster, bool bSelected);

  plDocument* m_pDocument = nullptr;
  QColor m_Accent;
  mutable plHybridArray<StripHit, 12> m_HitRects;

  // fader drag state
  plUuid m_DragGuid;
  QRect m_DragFaderTrack;
  bool m_bDragging = false;
  float m_fDragValueDb = 0.0f;
};

class plAudioMixerAssetDocumentWindow : public plQtDocumentWindow
{
  Q_OBJECT

public:
  plAudioMixerAssetDocumentWindow(plDocument* pDocument);
  ~plAudioMixerAssetDocumentWindow();

  virtual const char* GetGroupName() const { return "AudioMixerAsset"; }

private:
  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void SelectionEventHandler(const plSelectionManagerEvent& e);

  const plDocumentObject* GetPropertiesObject() const;

  void OnAddBus(bool bAux);
  void OnAddVca();
  void OnAddSnapshot();
  void OnRemoveSelected();
  void OnSnapshotItemClicked(QListWidgetItem* pItem);
  void OnRoutingItemClicked(QTreeWidgetItem* pItem, int iColumn);

  void QueueRefresh();
  void RebuildSnapshotList();
  void RebuildRoutingTree();

  /// \brief Mirrors the edited mixer into the editor preview engine, so auditioning an event
  /// routes through these buses and the strips' meters respond.
  void PushMixerToPreview();

  void SelectObjectByGuidString(const QVariant& itemData);

  plAudioMixerAssetDocument* m_pAssetDoc = nullptr;
  plQtAudioMixerStripsWidget* m_pStrips = nullptr;
  QListWidget* m_pSnapshotList = nullptr;
  QTreeWidget* m_pRoutingTree = nullptr;
  QLabel* m_pStatusLabel = nullptr;
  QTimer* m_pMeterTimer = nullptr;
  QColor m_Accent;
  bool m_bRefreshQueued = false;
};
