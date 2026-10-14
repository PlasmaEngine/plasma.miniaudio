#pragma once

#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <Foundation/Threading/TaskSystem.h>
#include <Foundation/Types/RefCounted.h>
#include <Foundation/Types/SharedPtr.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.moc.h>
#include <PlasmaAudioPlugin/Clips/AudioClipDesc.h>

#include <atomic>

class QLabel;
class QListWidget;
class QMenu;
class QTabBar;
class QTimer;
class QToolButton;
class plQtPropertyGridWidget;
struct plAssetCuratorEvent;
struct plDocumentObjectPropertyEvent;
struct plDocumentObjectStructureEvent;

/// \brief The outcome of building a clip in the editor with the settings of one platform tab.
struct plAudioClipPreview : public plRefCounted
{
  plUInt64 m_uiHash = 0;
  bool m_bSuccess = false;
  plString m_sError;

  plAudioClipBuildSettings m_Settings;
  plAudioClipHeader m_Header;
  plUInt64 m_uiFileBytes = 0;   ///< size of the built clip (default language only)
  plDynamicArray<plInt16> m_Pcm; ///< the built payload decoded again, for auditioning

  /// \brief Source time (seconds) of output frame 0: how much leading silence was trimmed.
  double m_fTrimOffset = 0.0;
};

/// \brief One preview build, run on a long running task. The window polls m_bDone.
struct plAudioClipPreviewJob : public plRefCounted
{
  plString m_sAbsSourceFile;
  plAudioClipBuildSettings m_Settings;
  plSharedPtr<plAudioClipPreview> m_pResult;
  std::atomic<bool> m_bDone = false;

  void Run();
};

/// \brief Waveform of a clip preview with draggable loop markers and the audition play cursor.
///
/// Dragging a marker, or dragging across the waveform to create a loop, writes LoopStart, LoopEnd and OverrideLoop to
/// the document when the mouse is released. Loop points are stored in source seconds, so the trimmed leading silence
/// is added back.
class plQtAudioWaveformWidget : public QWidget
{
  Q_OBJECT

public:
  plQtAudioWaveformWidget(QWidget* pParent, plDocument* pDocument, const QColor& accent);

  void SetPreview(const plSharedPtr<plAudioClipPreview>& pPreview);
  void SetMessage(const QString& sMessage);
  void SetPlayCursor(plInt64 iFrame);

  virtual QSize sizeHint() const override { return QSize(600, 220); }
  virtual QSize minimumSizeHint() const override { return QSize(200, 120); }

protected:
  virtual void paintEvent(QPaintEvent* pEvent) override;
  virtual void mousePressEvent(QMouseEvent* pEvent) override;
  virtual void mouseMoveEvent(QMouseEvent* pEvent) override;
  virtual void mouseReleaseEvent(QMouseEvent* pEvent) override;

private:
  enum class Drag
  {
    None,
    LoopStart,
    LoopEnd,
    Create
  };

  QRect GetPlotRect() const;
  double FrameToX(plUInt64 uiFrame) const;
  plUInt64 XToFrame(int x) const;
  bool GetLoop(plUInt64& out_uiStart, plUInt64& out_uiEnd) const;

  plDocument* m_pDocument = nullptr;
  QColor m_Accent;
  plSharedPtr<plAudioClipPreview> m_pPreview;
  QString m_sMessage;
  plInt64 m_iPlayCursor = -1;

  Drag m_Drag = Drag::None;
  plUInt64 m_uiDragAnchor = 0;
  plUInt64 m_uiDragStart = 0;
  plUInt64 m_uiDragEnd = 0;

  /// \brief Recomputes m_Bars when the preview or the plot width changed.
  void UpdateBars(int iPlotWidth);

  /// Bar heights (peak, 0..1) for the current preview and plot width: m_uiBarCount per channel, channel after channel.
  plDynamicArray<float> m_Bars;
  plUInt32 m_uiBarCount = 0;
  const plAudioClipPreview* m_pBarsPreview = nullptr;
  plUInt64 m_uiBarsHash = 0;
  int m_iBarsWidth = 0;
};

/// \brief Audio Clip editor: waveform with loop markers, info and cost estimates for each platform tab, the property grid
/// of the default settings or of the selected platform override, and the assets that use the clip.
class plAudioClipAssetDocumentWindow : public plQtDocumentWindow
{
  Q_OBJECT

public:
  plAudioClipAssetDocumentWindow(plDocument* pDocument);
  ~plAudioClipAssetDocumentWindow();

  virtual const char* GetGroupName() const { return "AudioClipAsset"; }

private:
  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void AssetCuratorEventHandler(const plAssetCuratorEvent& e);

  const plDocumentObject* GetPropertiesObject() const;
  const plDocumentObject* GetActiveOverrideObject() const;

  void RebuildTabs();
  void OnTabChanged(int iIndex);
  void OnFillAddOverrideMenu();
  void OnTabContextMenu(const QPoint& pos);
  void UpdatePropertyGridSelection();

  void RequestPreview(bool bImmediate);
  void OnTimer();
  void StartPreviewIfNeeded();
  void ShowPreview(const plSharedPtr<plAudioClipPreview>& pPreview);
  void UpdateLoopLabel();
  void RefreshUsedBy();

  void OnPlay();
  void OnStop();

  plAudioClipAssetDocument* m_pAssetDoc = nullptr;
  QColor m_Accent;

  plQtAudioWaveformWidget* m_pWaveform = nullptr;
  QTabBar* m_pTabBar = nullptr;
  QToolButton* m_pAddTabButton = nullptr;
  QMenu* m_pAddTabMenu = nullptr;
  QToolButton* m_pLoopButton = nullptr;
  QLabel* m_pInfoLabel = nullptr;
  QLabel* m_pLoopLabel = nullptr;
  QLabel* m_pResidentLabel = nullptr;
  QLabel* m_pDiskLabel = nullptr;
  QLabel* m_pDecodeLabel = nullptr;
  QLabel* m_pLoudnessLabel = nullptr;
  QLabel* m_pPlatformLabel = nullptr;
  plQtPropertyGridWidget* m_pPropertyGrid = nullptr;
  QListWidget* m_pUsedByList = nullptr;
  QTimer* m_pTimer = nullptr;

  plUuid m_ActiveOverrideGuid;              ///< invalid: the Default tab
  plHybridArray<plUuid, 8> m_TabOverrides;  ///< override object per tab, invalid for the Default tab
  bool m_bUpdatingTabs = false;
  bool m_bTabsDirty = false;
  std::atomic<bool> m_bUsedByDirty = true;

  // preview builds
  bool m_bPreviewRequested = false;
  plTime m_PreviewDue;
  plSharedPtr<plAudioClipPreviewJob> m_pRunningJob;
  plTaskGroupID m_RunningTask;
  plHybridArray<plSharedPtr<plAudioClipPreview>, 8> m_PreviewCache; ///< most recent last
  plSharedPtr<plAudioClipPreview> m_pShownPreview;
  plUInt64 m_uiWantedHash = 0;
};
