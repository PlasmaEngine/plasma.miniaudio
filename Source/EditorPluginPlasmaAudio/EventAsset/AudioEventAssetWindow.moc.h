#pragma once

#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAsset.h>
#include <Foundation/Basics.h>
#include <Foundation/Containers/Map.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Types/UniquePtr.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.moc.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

class QTimer;

/// \brief Horizontal output level meter (green/amber/red) for the event window's top bar.
class plQtAudioLevelMeterWidget : public QWidget
{
  Q_OBJECT

public:
  plQtAudioLevelMeterWidget(QWidget* pParent);

  /// \brief Feeds a new linear peak (0..1). Holds a decaying peak marker.
  void SetPeak(float fPeak);

  virtual QSize sizeHint() const override { return QSize(150, 14); }

protected:
  virtual void paintEvent(QPaintEvent* pEvent) override;

private:
  float m_fPeak = 0.0f;
  float m_fPeakHold = 0.0f;
};

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QSlider;
class QToolButton;
class QVBoxLayout;
class plQtPropertyGridWidget;
struct plDocumentObjectPropertyEvent;
struct plDocumentObjectStructureEvent;
struct plSelectionManagerEvent;

/// \brief Custom-painted instrument timeline: one row per instrument, clips placed at their
/// start delay, plus an automation chip lane at the bottom. Clicking selects.
class plQtAudioEventTimelineWidget : public QWidget
{
  Q_OBJECT

public:
  plQtAudioEventTimelineWidget(QWidget* pParent, plDocument* pDocument, const QColor& accent);

  virtual QSize sizeHint() const override;
  virtual QSize minimumSizeHint() const override { return sizeHint(); }

protected:
  virtual void paintEvent(QPaintEvent* pEvent) override;
  virtual void mousePressEvent(QMouseEvent* pEvent) override;
  virtual void mouseMoveEvent(QMouseEvent* pEvent) override;
  virtual void mouseReleaseEvent(QMouseEvent* pEvent) override;
  virtual void mouseDoubleClickEvent(QMouseEvent* pEvent) override;

private:
  friend class plAudioEventAssetDocumentWindow;

  struct HitRect
  {
    plUuid m_Guid;
    QRect m_Rect;
  };

  /// \brief What the user grabbed in the ruler / logic lane.
  enum class DragHandle
  {
    None,
    LoopStart,
    LoopEnd,
    FadeIn,
    FadeOut
  };

  const plDocumentObject* GetPropertiesObject() const;
  void GatherInstruments(plHybridArray<const plDocumentObject*, 8>& out_instruments) const;
  double GetTimeExtent() const;

  /// \brief Duration of a sound file in seconds (decoded via miniaudio, cached per path). 0 when unknown.
  double GetSampleDuration(plStringView sFile) const;

  /// \brief Duration of a playlist entry in seconds: the published duration of its Audio Clip, else of its embedded file. 0 when unknown.
  double GetEntryDuration(const plDocumentObject* pEntry) const;

  /// \brief Longest playlist entry duration of the instrument in seconds (>= a small floor for drawing).
  double GetInstrumentLength(const plDocumentObject* pInstrument) const;

  /// \brief Start delay of an instrument, reflecting an in-progress clip drag.
  double GetInstrumentDelay(const plDocumentObject* pInstrument) const;

  /// \brief Event-level marker value, reflecting an in-progress handle drag.
  double GetMarkerValue(const char* szProperty, DragHandle handle) const;

  /// \brief Opens an inline line edit over the track header to rename the instrument.
  void BeginRename(const plUuid& instrumentGuid, const QRect& headerRect);

  /// \brief Session-only solo state: when any track is solo'd, audition plays only solo'd tracks.
  PL_NODISCARD bool IsSolo(const plUuid& instrumentGuid) const { return m_SoloTracks.Contains(instrumentGuid); }
  void ToggleSolo(const plUuid& instrumentGuid);

  DragHandle PickHandle(const QPoint& pos) const;
  void ApplyHandleDrag(DragHandle handle, const QPoint& pos, bool bFinal);
  PL_NODISCARD double PixelToTime(int x) const;
  PL_NODISCARD int TimeToPixel(double fSeconds) const;

  plDocument* m_pDocument = nullptr;
  QColor m_Accent;
  mutable plHybridArray<HitRect, 8> m_HitRects;
  mutable plHybridArray<HitRect, 8> m_HeaderHitRects; // track name areas (click = select, double-click = rename)
  mutable plHybridArray<HitRect, 8> m_MuteHitRects;   // per-track M buttons (toggle the persisted Mute property)
  mutable plHybridArray<HitRect, 8> m_SoloHitRects;   // per-track S buttons (session-only audition solo)
  mutable plMap<plString, double> m_DurationCache;    // file path -> seconds (0 = could not decode)
  plHybridArray<plUuid, 8> m_SoloTracks;
  QLineEdit* m_pRenameEdit = nullptr;

  // marker drag: previewed live, written to the document once on release
  DragHandle m_ActiveHandle = DragHandle::None;
  bool m_bHandlePreviewValid = false;
  double m_fHandlePreview = 0.0;

  // clip drag: moves an instrument's StartDelay along the timeline
  plUuid m_DragClipGuid;
  bool m_bDraggingClip = false;
  double m_fDragClipDelay = 0.0;
  double m_fDragGrabOffset = 0.0;
};

/// \brief Editable curve widget for the deck's automation column: drag points, double-click to
/// add one, right-click to delete. Commits through the document's command history.
class plQtAudioCurvePreviewWidget : public QWidget
{
  Q_OBJECT

public:
  plQtAudioCurvePreviewWidget(QWidget* pParent, plDocument* pDocument, const QColor& accent);

  void SetAutomation(const plUuid& automationGuid);

protected:
  virtual void paintEvent(QPaintEvent* pEvent) override;
  virtual void mousePressEvent(QMouseEvent* pEvent) override;
  virtual void mouseMoveEvent(QMouseEvent* pEvent) override;
  virtual void mouseReleaseEvent(QMouseEvent* pEvent) override;
  virtual void mouseDoubleClickEvent(QMouseEvent* pEvent) override;

private:
  struct CurvePoint
  {
    plUuid m_Guid;
    double m_fX = 0.0;
    double m_fY = 1.0;
  };

  void GatherPoints(plHybridArray<CurvePoint, 8>& out_points) const;
  QRectF GetPlotArea() const;
  QPointF ToWidget(const QRectF& plot, double fX, double fY) const;
  void FromWidget(const QRectF& plot, const QPointF& pos, double& out_fX, double& out_fY) const;
  plInt32 FindPointAt(const QPoint& pos) const;
  void GetRanges(double& out_fMinX, double& out_fMaxX, double& out_fMaxY) const;

  plDocument* m_pDocument = nullptr;
  QColor m_Accent;
  plUuid m_AutomationGuid;

  plUuid m_DragPointGuid;
  bool m_bDragging = false;
};

/// \brief FMOD-style audio event editor: multi-track timeline on top, mockup deck below
/// (instrument playlist with weights, playback controls, automation curve), macros dock right.
class plAudioEventAssetDocumentWindow : public plQtDocumentWindow
{
  Q_OBJECT

public:
  plAudioEventAssetDocumentWindow(plDocument* pDocument);
  ~plAudioEventAssetDocumentWindow();

  virtual const char* GetGroupName() const { return "AudioEventAsset"; }

private:
  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void SelectionEventHandler(const plSelectionManagerEvent& e);

  const plDocumentObject* GetPropertiesObject() const;
  const plDocumentObject* GetSelectedObjectOfProperty(const char* szParentProperty) const;

  /// \brief The instrument shown in the deck: the selected one (or the owner of the selected
  /// playlist entry), else the one last picked in the dropdown, else the first instrument.
  const plDocumentObject* GetActiveInstrument() const;

  /// \brief The automation shown in the deck: the selected one, else the first.
  const plDocumentObject* GetActiveAutomation() const;

  void OnAddInstrument();
  void OnAddParameter();
  void OnRemoveSelected();

  /// \brief Auditions the event in the editor process (respects per-track mute and solo).
  void OnPlayEvent();
  void OnStopEvent();

  void OnAddPlaylistEntry();
  void OnAddFilesToInstrument();
  void OnAddClipToInstrument();

  /// \brief Asset browser for Audio Clips, preselecting sCurrentClip; returns the picked clip's GUID string.
  bool PickClip(plStringView sCurrentClip, plString& out_sClipGuid);
  void OnAddAutomationFor(plUInt8 uiTarget, const plString& sParameter);

  void SetInstrumentValue(const char* szProperty, const plVariant& value, const char* szTransaction);

  /// \brief Multi-select sound file dialog; returns data-directory-relative paths.
  bool PickSoundFiles(plHybridArray<plString, 8>& out_files);

  void QueueRefresh();
  void RebuildParameterTabs();
  void RebuildInstrumentCombo();
  void RebuildPlaylistPanel();
  void RefreshPlaybackControls();
  void RefreshAutomationColumn();

  plAudioEventAssetDocument* m_pAssetDoc = nullptr;

  plQtAudioEventTimelineWidget* m_pTimeline = nullptr;

  QWidget* m_pTabsHost = nullptr;
  QHBoxLayout* m_pTabsLayout = nullptr;

  // deck: instrument column
  QLabel* m_pInstrumentHeader = nullptr;
  QComboBox* m_pInstrumentCombo = nullptr;
  QWidget* m_pPlaylistHost = nullptr;
  QVBoxLayout* m_pPlaylistLayout = nullptr;

  // deck: playback column
  QComboBox* m_pModeCombo = nullptr;
  QSlider* m_pVolumeRandSlider = nullptr;
  QLabel* m_pVolumeRandLabel = nullptr;
  QSlider* m_pPitchRandSlider = nullptr;
  QLabel* m_pPitchRandLabel = nullptr;
  QCheckBox* m_pLoopCheck = nullptr;
  QDoubleSpinBox* m_pStartDelaySpin = nullptr;
  QDoubleSpinBox* m_pMinRetriggerSpin = nullptr;
  QDoubleSpinBox* m_pMaxRetriggerSpin = nullptr;

  // deck: automation column
  QLabel* m_pAutomationHeader = nullptr;
  QLabel* m_pAutomationTargetLabel = nullptr;
  plQtAudioCurvePreviewWidget* m_pCurvePreview = nullptr;
  QToolButton* m_pAddAutomationButton = nullptr;

  QLabel* m_pStatusLabel = nullptr;

  plUuid m_ActiveInstrumentGuid;
  QColor m_Accent;
  bool m_bRefreshQueued = false;
  bool m_bUpdatingUi = false;

  plRandom m_AuditionRng;

  plQtAudioLevelMeterWidget* m_pLevelMeter = nullptr;
  QLabel* m_pMeterLabel = nullptr;
  QTimer* m_pMeterTimer = nullptr;
};
