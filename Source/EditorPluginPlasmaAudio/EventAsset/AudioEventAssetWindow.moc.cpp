#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetBrowserDlg.moc.h>
#include <EditorFramework/Assets/AssetStatusIndicator.moc.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineViews.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAssetWindow.moc.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventDeckView.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventTimelineView.h>
#include <GuiFoundation/EngineUi/EngineUiControlStrip.h>
#include <PlasmaAudioPlugin/Mixer/AudioMixerData.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Math/ColorScheme.h>
#include <EditorPluginPlasmaAudio/Preview/AudioEnumRegistry.h>
#include <EditorPluginPlasmaAudio/Preview/AudioPreviewService.h>
#include <GuiFoundation/ActionViews/MenuBarActionMapView.moc.h>
#include <GuiFoundation/ActionViews/ToolBarActionMapView.moc.h>
#include <GuiFoundation/DockPanels/DocumentPanel.moc.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>

namespace
{
  constexpr int kRulerHeight = 24;
  constexpr int kRowHeight = 66;
  constexpr int kRowHeaderWidth = 156;
  constexpr int kPixelsPerSecond = 120;
  constexpr int kLogicRowHeight = 30;                                        // loop/fade markers
  constexpr int kAutomationLaneHeight = kLogicRowHeight + kLogicRowHeight;   // + automation chip row

  QColor ToQt(const plColor& c)
  {
    return plToQtColor(plColorGammaUB(c));
  }

  QColor AccentColor(const plDocument* pDocument)
  {
    plStringView sCategory = "Audio";
    if (pDocument != nullptr && pDocument->GetDocumentTypeDescriptor() != nullptr && !pDocument->GetDocumentTypeDescriptor()->m_sAssetCategory.IsEmpty())
    {
      sCategory = pDocument->GetDocumentTypeDescriptor()->m_sAssetCategory;
    }
    return ToQt(plColorScheme::GetCategoryColor(sCategory, plColorScheme::CategoryColorUsage::BorderColor));
  }

  QColor TrackColor(plUInt32 uiIndex)
  {
    static const plColorScheme::Enum palette[] = {plColorScheme::Blue, plColorScheme::Orange, plColorScheme::Teal, plColorScheme::Violet, plColorScheme::Cyan, plColorScheme::Lime};
    return ToQt(plColorScheme::LightUI(palette[uiIndex % PL_ARRAY_SIZE(palette)]));
  }

  void GatherChildrenOfProperty(const plDocumentObject* pParent, const char* szProperty, plHybridArray<const plDocumentObject*, 8>& out_children)
  {
    out_children.Clear();

    if (pParent == nullptr)
      return;

    for (const plDocumentObject* pChild : pParent->GetChildren())
    {
      if (pChild->GetParentProperty().IsEqual(szProperty))
      {
        out_children.PushBack(pChild);
      }
    }
  }

  // Mockup theme tokens: chrome #1e1e1e, panels #202020, headers #191919, controls #333333,
  // borders #3c3c3c, text #cccccc, dim #8e8e8e. Scoped by object name so the property grids
  // keep the editor's own styling.
  QString AudioToolStyle(const QColor& accent)
  {
    return QString(
      // button rows
      "QWidget#AudioButtonRow { background: #202020; border-bottom: 1px solid #303030; }"
      "QWidget#AudioButtonRow QToolButton { background: #333333; border: 1px solid #3c3c3c; border-radius: 3px; padding: 3px 10px; color: #cccccc; }"
      "QWidget#AudioButtonRow QToolButton:hover { border-color: %1; color: #ffffff; }"
      "QWidget#AudioButtonRow QToolButton:pressed { background: #2a2a2a; }"
      "QWidget#AudioButtonRow QToolButton#PlayButton { color: #9fe8c9; }"

      // tab strip
      "QWidget#TabStrip { background: #191919; border-bottom: 1px solid #303030; }"
      "QWidget#TabStrip QToolButton { background: #2d2d2d; border: 1px solid #303030; border-bottom: none; border-top-left-radius: 4px; border-top-right-radius: 4px; padding: 3px 14px; color: #8e8e8e; }"
      "QWidget#TabStrip QToolButton:hover { color: #ffffff; }"
      "QWidget#TabStrip QToolButton[activeTab=\"true\"] { background: #1e1e1e; color: #ffffff; border-color: #3c3c3c; }"
      "QWidget#TabStrip QToolButton#AddTab { border-style: dashed; color: %1; }"

      // deck columns
      "QWidget#Deck { background: #1e1e1e; border-top: 1px solid #303030; }"
      "QWidget#DeckColumn { background: #1e1e1e; border-right: 1px solid #303030; }"
      "QWidget#DeckColumn QLabel { color: #cccccc; }"
      "QLabel#DeckHeader { color: %1; font-size: 10px; font-weight: bold; letter-spacing: 1px; padding: 8px 10px 4px 10px; }"
      "QLabel#DeckSubLabel { color: #8e8e8e; font-size: 10px; padding-top: 6px; }"
      "QLabel#DeckValueLabel { color: #cccccc; font-size: 10px; }"

      // deck controls
      "QWidget#DeckColumn QComboBox { background: #333333; border: 1px solid #3c3c3c; border-radius: 3px; padding: 3px 10px; color: #ffffff; }"
      "QWidget#DeckColumn QComboBox:hover { border-color: %1; }"
      "QWidget#DeckColumn QComboBox QAbstractItemView { background: #2d2d2d; color: #cccccc; selection-background-color: #2d7f69; }"
      "QWidget#DeckColumn QDoubleSpinBox { background: #333333; border: 1px solid #3c3c3c; border-radius: 3px; padding: 1px 4px; color: #cccccc; }"
      "QWidget#DeckColumn QDoubleSpinBox:hover { border-color: %1; }"
      "QWidget#DeckColumn QCheckBox { color: #cccccc; }"
      "QWidget#DeckColumn QSlider::groove:horizontal { background: #333333; height: 4px; border-radius: 2px; }"
      "QWidget#DeckColumn QSlider::handle:horizontal { background: %1; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }"
      "QWidget#DeckColumn QToolButton { background: #333333; border: 1px solid #3c3c3c; border-radius: 3px; padding: 3px 10px; color: #cccccc; }"
      "QWidget#DeckColumn QToolButton:hover { border-color: %1; color: #ffffff; }"

      // playlist rows
      "QFrame#PlaylistRow { background: #202020; border: 1px solid #262626; border-radius: 2px; }"
      "QFrame#PlaylistRow:hover { border-color: #3c3c3c; }"
      "QFrame#PlaylistRow[rowSelected=\"true\"] { border-color: %1; }"
      "QFrame#PlaylistRow QToolButton#RowFile { background: transparent; border: none; color: #cccccc; font-family: Consolas; font-size: 11px; text-align: left; padding: 1px 2px; }"
      "QFrame#PlaylistRow QToolButton#RowFile:hover { color: %1; }"
      "QFrame#PlaylistRow QToolButton#RowFile[silence=\"true\"] { color: #8e8e8e; font-style: italic; }"
      "QFrame#PlaylistRow QLabel#RowWeightLabel { color: #8e8e8e; font-size: 10px; }"
      "QFrame#PlaylistRow QDoubleSpinBox { background: #2a2a2a; border: 1px solid #303030; border-radius: 2px; padding: 0px 2px; color: #cccccc; max-width: 56px; font-size: 10px; }"
      "QFrame#PlaylistRow QDoubleSpinBox:hover { border-color: %1; }"
      "QFrame#PlaylistRow QToolButton#RowButton { background: transparent; border: none; color: #8e8e8e; padding: 0px 3px; }"
      "QFrame#PlaylistRow QToolButton#RowButton:hover { color: #ffffff; }"
      "QWidget#DeckColumn QToolButton#AddRow { background: #202020; border: 1px dashed #3c3c3c; border-radius: 3px; padding: 4px; color: %1; }"
      "QWidget#DeckColumn QToolButton#AddRow:hover { border-color: %1; color: #ffffff; }"

      // headers + status
      "QLabel#PanelHeader { color: %1; font-size: 10px; font-weight: bold; letter-spacing: 1px; background: #191919; padding: 5px; border-bottom: 1px solid #303030; }"
      "QLabel#StatusBar { color: #8e8e8e; font-size: 10px; background: #181818; padding: 4px 10px; border-top: 1px solid #000000; }"

      "QScrollArea#AudioScroll { background: #1e1e1e; border: none; }"
      "QScrollArea#AudioScroll > QWidget > QWidget { background: #1e1e1e; }")
      .arg(accent.name());
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// plQtAudioLevelMeterWidget
//////////////////////////////////////////////////////////////////////////

plQtAudioLevelMeterWidget::plQtAudioLevelMeterWidget(QWidget* pParent)
  : QWidget(pParent)
{
  setFixedHeight(14);
  setMinimumWidth(120);
  setToolTip("Audition output level");
}

void plQtAudioLevelMeterWidget::SetPeak(float fPeak)
{
  m_fPeak = plMath::Clamp(fPeak, 0.0f, 1.5f);
  m_fPeakHold = plMath::Max(m_fPeak, m_fPeakHold * 0.94f);
  update();
}

void plQtAudioLevelMeterWidget::paintEvent(QPaintEvent* pEvent)
{
  PL_IGNORE_UNUSED(pEvent);

  QPainter p(this);

  const QRect area = rect().adjusted(0, 0, -1, -1);

  p.setPen(QPen(QColor(60, 60, 60), 1));
  p.setBrush(QColor(17, 17, 17));
  p.drawRect(area);

  const QRect inner = area.adjusted(1, 1, -1, -1);

  // green up to -6 dB, amber to -1 dB, red above
  QLinearGradient gradient(inner.topLeft(), inner.topRight());
  gradient.setColorAt(0.0, QColor(63, 174, 124));
  gradient.setColorAt(0.7, QColor(63, 174, 124));
  gradient.setColorAt(0.85, QColor(217, 195, 74));
  gradient.setColorAt(1.0, QColor(217, 93, 93));

  const int iWidth = static_cast<int>(inner.width() * plMath::Clamp(m_fPeak, 0.0f, 1.0f));
  if (iWidth > 0)
  {
    p.fillRect(QRect(inner.left(), inner.top(), iWidth, inner.height()), gradient);
  }

  // peak-hold marker
  if (m_fPeakHold > 0.01f)
  {
    const int iHoldX = inner.left() + static_cast<int>(inner.width() * plMath::Clamp(m_fPeakHold, 0.0f, 1.0f));
    p.setPen(QPen(m_fPeakHold >= 0.99f ? QColor(255, 120, 120) : QColor(220, 220, 220), 1));
    p.drawLine(iHoldX, inner.top(), iHoldX, inner.bottom());
  }

  // tick marks at -12 / -6 dB
  p.setPen(QColor(70, 70, 70));
  for (float fMark : {0.25f, 0.5f})
  {
    const int iX = inner.left() + static_cast<int>(inner.width() * fMark);
    p.drawLine(iX, inner.bottom() - 3, iX, inner.bottom());
  }
}

//////////////////////////////////////////////////////////////////////////
// plQtAudioEventTimelineWidget
//////////////////////////////////////////////////////////////////////////

plQtAudioEventTimelineWidget::plQtAudioEventTimelineWidget(QWidget* pParent, plDocument* pDocument, const QColor& accent)
  : QWidget(pParent)
  , m_pDocument(pDocument)
  , m_Accent(accent)
{
  setMouseTracking(true); // hover cursor feedback for the loop/fade handles
  setAutoFillBackground(false);
}

const plDocumentObject* plQtAudioEventTimelineWidget::GetPropertiesObject() const
{
  const auto& children = m_pDocument->GetObjectManager()->GetRootObject()->GetChildren();
  return children.IsEmpty() ? nullptr : children[0];
}

void plQtAudioEventTimelineWidget::GatherInstruments(plHybridArray<const plDocumentObject*, 8>& out_instruments) const
{
  GatherChildrenOfProperty(GetPropertiesObject(), "Instruments", out_instruments);
}

double plQtAudioEventTimelineWidget::GetSampleDuration(plStringView sFile) const
{
  if (sFile.IsEmpty())
    return 0.0;

  const plString sKey = sFile;

  if (auto it = m_DurationCache.Find(sKey); it.IsValid())
    return it.Value();

  double fDuration = 0.0;

  plStringBuilder sAbsPath = sFile;
  if (plQtEditorApp::GetSingleton()->MakeDataDirectoryRelativePathAbsolute(sAbsPath))
  {
    ma_decoder decoder;
    if (ma_decoder_init_file(sAbsPath.GetData(), nullptr, &decoder) == MA_SUCCESS)
    {
      ma_uint64 uiFrames = 0;
      if (ma_decoder_get_length_in_pcm_frames(&decoder, &uiFrames) == MA_SUCCESS && decoder.outputSampleRate > 0)
      {
        fDuration = static_cast<double>(uiFrames) / static_cast<double>(decoder.outputSampleRate);
      }
      ma_decoder_uninit(&decoder);
    }
  }

  m_DurationCache[sKey] = fDuration;
  return fDuration;
}

double plQtAudioEventTimelineWidget::GetEntryDuration(const plDocumentObject* pEntry) const
{
  const plString sClip = pEntry->GetTypeAccessor().GetValue("Clip").ConvertTo<plString>();
  if (!sClip.IsEmpty())
  {
    plAudioClipAssetMetaInfo info;
    return plAudioClipAssetUtils::GetClipInfo(sClip, info) ? info.m_fDuration : 0.0;
  }

  return GetSampleDuration(pEntry->GetTypeAccessor().GetValue("File").ConvertTo<plString>());
}

double plQtAudioEventTimelineWidget::GetInstrumentLength(const plDocumentObject* pInstrument) const
{
  plHybridArray<const plDocumentObject*, 8> playlist;
  GatherChildrenOfProperty(pInstrument, "Playlist", playlist);

  double fLongest = 0.0;
  for (const plDocumentObject* pEntry : playlist)
  {
    fLongest = plMath::Max(fLongest, GetEntryDuration(pEntry));
  }

  // small floor so unknown/silence-only instruments still draw a usable clip
  return plMath::Max(fLongest, 0.6);
}

double plQtAudioEventTimelineWidget::GetInstrumentDelay(const plDocumentObject* pInstrument) const
{
  if (pInstrument == nullptr)
    return 0.0;

  // a clip being dragged previews its new position before it is committed
  if (m_bDraggingClip && pInstrument->GetGuid() == m_DragClipGuid)
    return m_fDragClipDelay;

  return pInstrument->GetTypeAccessor().GetValue("StartDelay").ConvertTo<double>();
}

double plQtAudioEventTimelineWidget::GetMarkerValue(const char* szProperty, DragHandle handle) const
{
  if (m_ActiveHandle == handle && m_bHandlePreviewValid)
    return m_fHandlePreview;

  const plDocumentObject* pProps = GetPropertiesObject();
  return pProps != nullptr ? pProps->GetTypeAccessor().GetValue(szProperty).ConvertTo<double>() : 0.0;
}

double plQtAudioEventTimelineWidget::GetTimeExtent() const
{
  // the event's real end: the latest point at which any instrument stops sounding.
  // everything that anchors to "the end" (the stop-fade marker) uses this, so the timeline
  // length always represents its contents.
  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherInstruments(instruments);

  double fEnd = 0.0;
  for (const plDocumentObject* pInstrument : instruments)
  {
    fEnd = plMath::Max(fEnd, GetInstrumentDelay(pInstrument) + GetInstrumentLength(pInstrument));
  }

  // an empty event still needs a usable ruler
  return fEnd > 0.0 ? fEnd : 2.0;
}

QSize plQtAudioEventTimelineWidget::sizeHint() const
{
  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherInstruments(instruments);

  // padding is pixels, not seconds, so the content end stays at its true time position
  const int iWidth = kRowHeaderWidth + static_cast<int>(GetTimeExtent() * kPixelsPerSecond) + 90;
  const int iHeight = kRulerHeight + plMath::Max<int>(1, instruments.GetCount()) * kRowHeight + kAutomationLaneHeight + 8;
  return QSize(iWidth, iHeight);
}

void plQtAudioEventTimelineWidget::paintEvent(QPaintEvent* pEvent)
{
  PL_IGNORE_UNUSED(pEvent);

  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  const QColor bgColor(30, 30, 30);
  const QColor rulerColor(25, 25, 25);
  const QColor lineColor(48, 48, 48);
  const QColor headerColor(32, 32, 32);
  const QColor textColor(204, 204, 204);
  const QColor dimColor(142, 142, 142);

  p.fillRect(rect(), bgColor);

  m_HitRects.Clear();
  m_HeaderHitRects.Clear();
  m_MuteHitRects.Clear();
  m_SoloHitRects.Clear();

  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherInstruments(instruments);

  const bool bAnySolo = !m_SoloTracks.IsEmpty();

  const plDocumentObject* pProps = GetPropertiesObject();

  plString sOutputBus;
  if (pProps != nullptr)
  {
    sOutputBus = pProps->GetTypeAccessor().GetValue("OutputBus").ConvertTo<plString>();
  }
  if (sOutputBus.IsEmpty())
    sOutputBus = "Master";

  const plUuid selectedGuid = (m_pDocument->GetSelectionManager()->GetCurrentObject() != nullptr)
                                ? m_pDocument->GetSelectionManager()->GetCurrentObject()->GetGuid()
                                : plUuid();

  const double fExtent = GetTimeExtent();

  QFont smallFont = font();
  smallFont.setPointSizeF(7.5);

  QFont nameFont = font();
  nameFont.setBold(true);

  // ruler
  p.fillRect(QRect(0, 0, width(), kRulerHeight), rulerColor);
  p.setPen(lineColor);
  p.drawLine(0, kRulerHeight, width(), kRulerHeight);
  p.setFont(smallFont);

  // loop region band across the whole timeline (mockup: LOOP REGION)
  const bool bLoopRegion = pProps != nullptr && pProps->GetTypeAccessor().GetValue("LoopRegion").ConvertTo<bool>();
  if (bLoopRegion)
  {
    const double fLoopStart = GetMarkerValue("LoopStart", DragHandle::LoopStart);
    const double fLoopEnd = GetMarkerValue("LoopEnd", DragHandle::LoopEnd);

    const int iStartX = TimeToPixel(plMath::Min(fLoopStart, fLoopEnd));
    const int iEndX = TimeToPixel(plMath::Max(fLoopStart, fLoopEnd));

    p.fillRect(QRect(iStartX, 0, iEndX - iStartX, height()), QColor(m_Accent.red(), m_Accent.green(), m_Accent.blue(), 26));

    p.setPen(QPen(m_Accent, 1));
    p.drawLine(iStartX, 0, iStartX, height());
    p.drawLine(iEndX, 0, iEndX, height());

    // grab handles in the ruler
    p.setBrush(m_Accent);
    p.setPen(Qt::NoPen);
    p.drawRect(QRect(iStartX, 0, 5, 12));
    p.drawRect(QRect(iEndX - 4, 0, 5, 12));

    p.setPen(QColor(16, 26, 22));
    p.setFont(smallFont);
    p.drawText(QRect(iStartX + 7, 0, 90, 12), Qt::AlignLeft | Qt::AlignVCenter, "LOOP REGION");
  }

  // ruler ticks span the whole visible width; the content end is marked separately below
  for (double fTime = 0.0; TimeToPixel(fTime) < width(); fTime += 0.5)
  {
    const int x = TimeToPixel(fTime);

    p.setPen(QColor(38, 38, 38));
    for (int iSub = 1; iSub < 5; ++iSub)
    {
      const int iSubX = x + (iSub * kPixelsPerSecond) / 10;
      p.drawLine(iSubX, kRulerHeight - 4, iSubX, kRulerHeight);
    }

    p.setPen(lineColor);
    p.drawLine(x, kRulerHeight, x, height());
    p.setPen(dimColor);
    p.drawText(QPoint(x + 4, 15), QString::asprintf("%.1fs", fTime));
  }

  // end-of-event marker: where the last instrument stops sounding
  {
    const int iEndX = TimeToPixel(fExtent);
    p.setPen(QPen(QColor(120, 120, 120), 1, Qt::DashLine));
    p.drawLine(iEndX, kRulerHeight, iEndX, height());

    p.setPen(QColor(160, 160, 160));
    p.setFont(smallFont);
    p.drawText(QRect(iEndX + 4, kRulerHeight + 2, 80, 12), Qt::AlignLeft | Qt::AlignTop, QString::asprintf("end %.2fs", fExtent));
  }

  // row header column background
  p.fillRect(QRect(0, kRulerHeight, kRowHeaderWidth, height() - kRulerHeight), headerColor);
  p.setPen(lineColor);
  p.drawLine(kRowHeaderWidth, 0, kRowHeaderWidth, height());

  if (instruments.IsEmpty())
  {
    p.setPen(dimColor);
    p.setFont(font());
    p.drawText(rect().adjusted(kRowHeaderWidth, kRulerHeight, 0, 0), Qt::AlignCenter, "No instruments yet.\nUse '+ Instrument' to add a track,\nthen fill its playlist with '+ Add Files...'.");
    return;
  }

  for (plUInt32 i = 0; i < instruments.GetCount(); ++i)
  {
    const plDocumentObject* pInstrument = instruments[i];
    const auto& accessor = pInstrument->GetTypeAccessor();

    const int iRowY = kRulerHeight + i * kRowHeight;
    const bool bRowSelected = pInstrument->GetGuid() == selectedGuid;

    // selected track: tinted row + accent bar down the left edge, so the deck's target is obvious
    if (bRowSelected)
    {
      p.fillRect(QRect(0, iRowY, width(), kRowHeight), QColor(m_Accent.red(), m_Accent.green(), m_Accent.blue(), 20));
      p.fillRect(QRect(0, iRowY, 3, kRowHeight), m_Accent);
    }

    p.setPen(lineColor);
    p.drawLine(0, iRowY + kRowHeight, width(), iRowY + kRowHeight);

    const QColor trackColor = TrackColor(i);

    plStringBuilder sName = accessor.GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      sName.SetFormat("Instrument {0}", i + 1);

    const bool bMuted = accessor.GetValue("Mute").ConvertTo<bool>();
    const bool bSolo = IsSolo(pInstrument->GetGuid());
    const bool bSilencedBySolo = bAnySolo && !bSolo;
    const bool bAudible = !bMuted && !bSilencedBySolo;

    // track header: color chip + name + routing + mute/solo buttons
    p.fillRect(QRect(8, iRowY + 11, 8, 8), bAudible ? trackColor : trackColor.darker(200));
    p.setPen(bRowSelected ? QColor(255, 255, 255) : (bAudible ? textColor : dimColor));
    p.setFont(nameFont);
    p.drawText(QRect(22, iRowY + 5, kRowHeaderWidth - 76, 20), Qt::AlignLeft | Qt::AlignVCenter, QString::fromUtf8(sName.GetData()));

    const bool bLoop = accessor.GetValue("Loop").ConvertTo<bool>();

    // longest decoded playlist entry: 0 when nothing could be decoded (missing files, silence only)
    plHybridArray<const plDocumentObject*, 8> playlist;
    GatherChildrenOfProperty(pInstrument, "Playlist", playlist);

    double fRealLength = 0.0;
    for (const plDocumentObject* pEntry : playlist)
    {
      fRealLength = plMath::Max(fRealLength, GetEntryDuration(pEntry));
    }

    const bool bLengthKnown = fRealLength > 0.0;

    p.setFont(smallFont);
    p.setPen(dimColor);

    // sub-line always reports the measured length here, where the clip width cannot clip it away
    QString sState;
    if (bMuted)
      sState = "muted";
    else if (bSilencedBySolo)
      sState = "silenced (solo)";
    else
      sState = QString::asprintf("-> %s", sOutputBus.GetData());

    QString sSub = sState + (bLengthKnown ? QString::asprintf("  \xC2\xB7 %.2fs", fRealLength) : QString("  \xC2\xB7 length ?"));
    if (bLoop)
      sSub += " \xE2\x88\x9E";
    p.drawText(QRect(22, iRowY + 26, kRowHeaderWidth - 28, 16), Qt::AlignLeft | Qt::AlignVCenter, sSub);

    // M / S buttons
    {
      const QRect muteRect(kRowHeaderWidth - 48, iRowY + 8, 20, 16);
      const QRect soloRect(kRowHeaderWidth - 25, iRowY + 8, 20, 16);

      auto drawToggle = [&](const QRect& r, const char* szLabel, bool bOn, const QColor& onColor)
      {
        p.setPen(QPen(bOn ? onColor : QColor(60, 60, 60), 1));
        p.setBrush(bOn ? onColor.darker(220) : QColor(40, 40, 40));
        p.drawRoundedRect(r, 3, 3);
        p.setPen(bOn ? onColor.lighter(130) : dimColor);
        p.setFont(smallFont);
        p.drawText(r, Qt::AlignCenter, szLabel);
      };

      drawToggle(muteRect, "M", bMuted, QColor(224, 108, 108));
      drawToggle(soloRect, "S", bSolo, QColor(230, 200, 110));

      auto& muteHit = m_MuteHitRects.ExpandAndGetRef();
      muteHit.m_Guid = pInstrument->GetGuid();
      muteHit.m_Rect = muteRect.adjusted(-2, -3, 2, 3);

      auto& soloHit = m_SoloHitRects.ExpandAndGetRef();
      soloHit.m_Guid = pInstrument->GetGuid();
      soloHit.m_Rect = soloRect.adjusted(-2, -3, 2, 3);
    }

    // track header hit area (click = select instrument, double-click = rename)
    {
      auto& headerHit = m_HeaderHitRects.ExpandAndGetRef();
      headerHit.m_Guid = pInstrument->GetGuid();
      headerHit.m_Rect = QRect(0, iRowY, kRowHeaderWidth, kRowHeight);
    }

    // clip width == the instrument's longest playlist entry (unknown lengths get a placeholder width)
    const double fDelay = GetInstrumentDelay(pInstrument);
    const double fDrawLength = bLengthKnown ? fRealLength : 0.6;
    const int iClipX = TimeToPixel(fDelay);
    const int iClipWidth = plMath::Max(24, static_cast<int>(fDrawLength * kPixelsPerSecond));
    const QRect clipRect(iClipX + 1, iRowY + 7, iClipWidth, kRowHeight - 16);

    const bool bSelected = bRowSelected;

    QLinearGradient gradient(clipRect.topLeft(), clipRect.bottomLeft());
    if (bAudible)
    {
      gradient.setColorAt(0.0, trackColor.lighter(112));
      gradient.setColorAt(1.0, trackColor.darker(118));
    }
    else
    {
      // disabled channel: desaturated and dark, so it reads as "not playing"
      const QColor mutedColor = QColor(trackColor.red() / 3 + 40, trackColor.green() / 3 + 40, trackColor.blue() / 3 + 40);
      gradient.setColorAt(0.0, mutedColor);
      gradient.setColorAt(1.0, mutedColor.darker(120));
    }

    p.setPen(bSelected ? QPen(m_Accent, 2) : QPen(trackColor.darker(150), bAudible ? 1 : 1));
    if (!bAudible)
      p.setPen(bSelected ? QPen(m_Accent, 2) : QPen(QColor(70, 70, 70), 1, Qt::DashLine));
    p.setBrush(gradient);
    p.drawRoundedRect(clipRect, 4, 4);

    // while dragging, show where the clip now starts
    if (m_bDraggingClip && pInstrument->GetGuid() == m_DragClipGuid)
    {
      const QString sPos = QString::asprintf("%.2fs", fDelay);
      p.setFont(smallFont);

      const int iBadgeWidth = p.fontMetrics().horizontalAdvance(sPos) + 10;
      const QRect badge(clipRect.left(), clipRect.top() - 15, iBadgeWidth, 14);

      p.setPen(Qt::NoPen);
      p.setBrush(m_Accent);
      p.drawRoundedRect(badge, 3, 3);

      p.setPen(QColor(16, 26, 22));
      p.drawText(badge, Qt::AlignCenter, sPos);
    }

    // end-of-sound tick, so the clip's right edge reads as an actual time position
    if (bLengthKnown)
    {
      p.setPen(QPen(QColor(255, 255, 255, 70), 1));
      p.drawLine(clipRect.right(), clipRect.top() + 2, clipRect.right(), clipRect.bottom() - 2);
    }
    else
    {
      // placeholder width: mark it so it is never mistaken for a measured length
      p.setPen(QPen(QColor(230, 200, 110), 1, Qt::DotLine));
      p.drawLine(clipRect.right(), clipRect.top() + 2, clipRect.right(), clipRect.bottom() - 2);
    }

    const plInt64 iMode = accessor.GetValue("PlaylistMode").ConvertTo<plInt64>();
    const char* szMode = (iMode == plAudioEventPlaylistMode::Sequential) ? "sequential" : (iMode == plAudioEventPlaylistMode::Shuffle ? "shuffle" : "random");
    const double fPitchRand = accessor.GetValue("PitchRandomization").ConvertTo<double>();

    QString sKind = playlist.GetCount() > 1 ? "Multi Sound" : "Single Sound";
    QString sInfo = QString::asprintf("%u %s \xC2\xB7 %s", playlist.GetCount(), playlist.GetCount() == 1 ? "entry" : "entries", szMode);
    if (fPitchRand > 0.0)
      sInfo += QString::asprintf(" \xC2\xB7 \xC2\xB1%.1f st", fPitchRand);

    if (bLengthKnown)
      sInfo += QString::asprintf(" \xC2\xB7 %.2fs", fRealLength);

    if (bLoop)
      sInfo += QString::fromUtf8(" \xC2\xB7 \xE2\x88\x9E");

    // dark ink on the bright clip, light ink once the clip is dimmed by mute/solo
    const QColor inkColor = bAudible ? QColor(14, 26, 22) : QColor(150, 150, 150);

    p.setPen(inkColor);
    p.setFont(nameFont);
    {
      const QRect kindRect(clipRect.left() + 8, clipRect.top() + 3, clipRect.width() - 14, 15);
      p.drawText(kindRect, Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(sKind, Qt::ElideRight, kindRect.width()));
    }

    // fake waveform band between title and info line
    {
      const int iCenterY = clipRect.top() + 27;
      QColor waveColor = inkColor;
      waveColor.setAlpha(80);
      p.setPen(QPen(waveColor, 2));

      for (int wx = clipRect.left() + 8; wx < clipRect.right() - 8; wx += 3)
      {
        const float fH = 1.5f + 4.0f * plMath::Abs(plMath::Sin(plAngle::MakeFromRadian(wx * 0.55f + i * 1.7f))) + 1.5f * plMath::Abs(plMath::Sin(plAngle::MakeFromRadian(wx * 0.13f)));
        p.drawLine(wx, iCenterY - static_cast<int>(fH), wx, iCenterY + static_cast<int>(fH));
      }
    }

    p.setPen(inkColor);
    p.setFont(smallFont);
    {
      const QRect infoRect(clipRect.left() + 8, clipRect.bottom() - 15, clipRect.width() - 14, 13);
      p.drawText(infoRect, Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(sInfo, Qt::ElideRight, infoRect.width()));
    }

    auto& hit = m_HitRects.ExpandAndGetRef();
    hit.m_Guid = pInstrument->GetGuid();
    hit.m_Rect = clipRect.adjusted(0, -7, 0, 9);
  }

  // automation lane (mockup's logic lane): one clickable chip per automation curve
  {
    const int iLaneY = kRulerHeight + instruments.GetCount() * kRowHeight;

    p.fillRect(QRect(0, iLaneY, width(), kAutomationLaneHeight), QColor(27, 27, 27));
    p.setPen(lineColor);
    p.drawLine(0, iLaneY, width(), iLaneY);

    p.fillRect(QRect(0, iLaneY, kRowHeaderWidth, kAutomationLaneHeight), headerColor);

    // row 1 header: logic (loop + fades), row 2 header: automation
    p.fillRect(QRect(8, iLaneY + (kLogicRowHeight - 8) / 2, 8, 8), QColor(230, 200, 110));
    p.setPen(textColor);
    p.setFont(nameFont);
    p.drawText(QRect(22, iLaneY, kRowHeaderWidth - 28, kLogicRowHeight), Qt::AlignLeft | Qt::AlignVCenter, "Logic");

    p.fillRect(QRect(8, iLaneY + kLogicRowHeight + (kLogicRowHeight - 8) / 2, 8, 8), QColor(155, 140, 208));
    p.setPen(textColor);
    p.drawText(QRect(22, iLaneY + kLogicRowHeight, kRowHeaderWidth - 28, kLogicRowHeight), Qt::AlignLeft | Qt::AlignVCenter, "Automation");

    p.setPen(lineColor);
    p.drawLine(0, iLaneY + kLogicRowHeight, width(), iLaneY + kLogicRowHeight);

    // fade-in / fade-out markers (mockup's logic lane), draggable
    if (pProps != nullptr)
    {
      const double fFadeIn = GetMarkerValue("FadeIn", DragHandle::FadeIn);
      const double fFadeOut = GetMarkerValue("StopFadeOut", DragHandle::FadeOut);

      p.setFont(smallFont);

      const QColor fadeInColor(110, 200, 160);
      const QColor fadeOutColor(224, 150, 110);

      const int iFadeInX = TimeToPixel(fFadeIn);
      const double fEnd = GetTimeExtent();
      const int iEndX = TimeToPixel(fEnd);
      const int iFadeOutX = TimeToPixel(plMath::Max(fEnd - fFadeOut, 0.0));

      // fade-in region: from 0 to FadeIn
      p.setPen(QPen(fadeInColor, 1));
      p.setBrush(QColor(110, 200, 160, 40));
      p.drawRect(QRect(kRowHeaderWidth, iLaneY + 6, iFadeInX - kRowHeaderWidth, kLogicRowHeight - 12));

      p.setBrush(fadeInColor);
      p.setPen(Qt::NoPen);
      p.drawRect(QRect(iFadeInX - 2, iLaneY + 4, 4, kLogicRowHeight - 8));

      // fade-out region: ends at the event's end
      p.setPen(QPen(fadeOutColor, 1));
      p.setBrush(QColor(224, 150, 110, 40));
      p.drawRect(QRect(iFadeOutX, iLaneY + 6, iEndX - iFadeOutX, kLogicRowHeight - 12));

      p.setBrush(fadeOutColor);
      p.setPen(Qt::NoPen);
      p.drawRect(QRect(iFadeOutX - 2, iLaneY + 4, 4, kLogicRowHeight - 8));

      // labels: keep them from colliding on short events by shortening, then by moving
      // the stop-fade label past the end marker where there is always room
      {
        const QString sInLong = QString::asprintf("fade in %.2fs", fFadeIn);
        const QString sOutLong = QString::asprintf("stop fade %.2fs", fFadeOut);
        const QString sInShort = QString::asprintf("in %.2fs", fFadeIn);
        const QString sOutShort = QString::asprintf("out %.2fs", fFadeOut);

        const QFontMetrics metrics = p.fontMetrics();

        QString sIn = sInLong;
        QString sOut = sOutLong;

        int iInLeft = iFadeInX + 6;
        int iOutRight = iFadeOutX - 6;

        auto fits = [&]()
        { return iInLeft + metrics.horizontalAdvance(sIn) < iOutRight - metrics.horizontalAdvance(sOut); };

        if (!fits())
        {
          sIn = sInShort;
          sOut = sOutShort;
        }

        p.setPen(fadeInColor);
        p.setFont(smallFont);

        bool bOutAfterEnd = false;
        if (!fits())
        {
          // no room between the handles at all: park the stop-fade label right of the end line
          bOutAfterEnd = true;
        }

        p.drawText(QRect(iInLeft, iLaneY, metrics.horizontalAdvance(sIn) + 4, kLogicRowHeight), Qt::AlignLeft | Qt::AlignVCenter, sIn);

        p.setPen(fadeOutColor);
        if (bOutAfterEnd)
        {
          p.drawText(QRect(iEndX + 6, iLaneY, metrics.horizontalAdvance(sOut) + 4, kLogicRowHeight), Qt::AlignLeft | Qt::AlignVCenter, sOut);
        }
        else
        {
          const int iWidth = metrics.horizontalAdvance(sOut) + 4;
          p.drawText(QRect(iOutRight - iWidth, iLaneY, iWidth, kLogicRowHeight), Qt::AlignRight | Qt::AlignVCenter, sOut);
        }
      }
    }

    plHybridArray<const plDocumentObject*, 8> automations;
    GatherChildrenOfProperty(pProps, "Automations", automations);

    p.setFont(smallFont);

    if (automations.IsEmpty())
    {
      p.setPen(dimColor);
      p.drawText(QRect(kRowHeaderWidth + 8, iLaneY + kLogicRowHeight, 400, kLogicRowHeight), Qt::AlignLeft | Qt::AlignVCenter, "no automation curves - add one in the deck's AUTOMATION column");
    }
    else
    {
      int iChipX = kRowHeaderWidth + 8;

      for (const plDocumentObject* pAutomation : automations)
      {
        const auto& autoAccessor = pAutomation->GetTypeAccessor();

        plString sParam = autoAccessor.GetValue("Parameter").ConvertTo<plString>();
        if (sParam.IsEmpty())
          sParam = "?";

        const plInt64 iTarget = autoAccessor.GetValue("Target").ConvertTo<plInt64>();
        const char* szTarget = (iTarget == plAudioEventAutomationTarget::EventPitch) ? "Pitch" : "Volume";

        const QString sLabel = QString::asprintf("%s by %s", szTarget, sParam.GetData());
        const int iChipWidth = p.fontMetrics().horizontalAdvance(sLabel) + 18;
        const QRect chipRect(iChipX, iLaneY + kLogicRowHeight + 5, iChipWidth, kLogicRowHeight - 10);

        const bool bSelected = pAutomation->GetGuid() == selectedGuid;

        p.setPen(bSelected ? QPen(m_Accent, 2) : QPen(QColor(155, 140, 208), 1));
        p.setBrush(QColor(155, 140, 208, 40));
        p.drawRoundedRect(chipRect, 3, 3);

        p.setPen(QColor(205, 194, 239));
        p.drawText(chipRect, Qt::AlignCenter, sLabel);

        auto& hit = m_HitRects.ExpandAndGetRef();
        hit.m_Guid = pAutomation->GetGuid();
        hit.m_Rect = chipRect.adjusted(-2, -4, 2, 4);

        iChipX += iChipWidth + 8;
      }
    }
  }
}

double plQtAudioEventTimelineWidget::PixelToTime(int x) const
{
  return plMath::Max(0.0, static_cast<double>(x - kRowHeaderWidth) / static_cast<double>(kPixelsPerSecond));
}

int plQtAudioEventTimelineWidget::TimeToPixel(double fSeconds) const
{
  return kRowHeaderWidth + static_cast<int>(fSeconds * kPixelsPerSecond);
}

plQtAudioEventTimelineWidget::DragHandle plQtAudioEventTimelineWidget::PickHandle(const QPoint& pos) const
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr || pos.x() < kRowHeaderWidth - 6)
    return DragHandle::None;

  const auto& accessor = pProps->GetTypeAccessor();

  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherInstruments(instruments);

  const int iLaneY = kRulerHeight + instruments.GetCount() * kRowHeight;
  const bool bInLogicRow = pos.y() >= iLaneY && pos.y() < iLaneY + kLogicRowHeight;
  const bool bInRuler = pos.y() < kRulerHeight;

  // loop region handles: grabbable in the ruler and along the region edges
  if (accessor.GetValue("LoopRegion").ConvertTo<bool>())
  {
    const int iStartX = TimeToPixel(accessor.GetValue("LoopStart").ConvertTo<double>());
    const int iEndX = TimeToPixel(accessor.GetValue("LoopEnd").ConvertTo<double>());

    if (bInRuler || !bInLogicRow)
    {
      if (plMath::Abs(pos.x() - iStartX) <= 5)
        return DragHandle::LoopStart;
      if (plMath::Abs(pos.x() - iEndX) <= 5)
        return DragHandle::LoopEnd;
    }
  }

  // fade handles live in the logic row
  if (bInLogicRow)
  {
    const int iFadeInX = TimeToPixel(accessor.GetValue("FadeIn").ConvertTo<double>());
    if (plMath::Abs(pos.x() - iFadeInX) <= 5)
      return DragHandle::FadeIn;

    const double fEnd = GetTimeExtent();
    const int iFadeOutX = TimeToPixel(plMath::Max(fEnd - accessor.GetValue("StopFadeOut").ConvertTo<double>(), 0.0));
    if (plMath::Abs(pos.x() - iFadeOutX) <= 5)
      return DragHandle::FadeOut;
  }

  return DragHandle::None;
}

void plQtAudioEventTimelineWidget::ApplyHandleDrag(DragHandle handle, const QPoint& pos, bool bFinal)
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr || handle == DragHandle::None)
    return;

  const auto& accessor = pProps->GetTypeAccessor();
  const double fTime = plMath::RoundToMultiple(PixelToTime(pos.x()), 0.01);

  const char* szProperty = nullptr;
  float fValue = 0.0f;

  switch (handle)
  {
    case DragHandle::LoopStart:
      szProperty = "LoopStart";
      fValue = static_cast<float>(plMath::Min(fTime, accessor.GetValue("LoopEnd").ConvertTo<double>() - 0.05));
      break;

    case DragHandle::LoopEnd:
      szProperty = "LoopEnd";
      fValue = static_cast<float>(plMath::Max(fTime, accessor.GetValue("LoopStart").ConvertTo<double>() + 0.05));
      break;

    case DragHandle::FadeIn:
      szProperty = "FadeIn";
      fValue = static_cast<float>(plMath::Max(fTime, 0.0));
      break;

    case DragHandle::FadeOut:
    {
      // the handle is the fade's start, so the stored duration is measured back from the end
      const double fEnd = GetTimeExtent();
      szProperty = "StopFadeOut";
      fValue = static_cast<float>(plMath::Clamp(fEnd - fTime, 0.0, 60.0));
      break;
    }

    default:
      return;
  }

  // while dragging only preview; a single command is written on release, so a drag
  // costs one undo step instead of one per mouse move
  if (!bFinal)
  {
    m_fHandlePreview = fValue;
    m_bHandlePreviewValid = true;
    update();
    return;
  }

  m_bHandlePreviewValid = false;

  if (plMath::Abs(accessor.GetValue(szProperty).ConvertTo<float>() - fValue) < 0.0001f)
  {
    update();
    return;
  }

  plObjectCommandAccessor commandAccessor(m_pDocument->GetCommandHistory());
  commandAccessor.StartTransaction("Edit Timeline Marker");
  commandAccessor.SetValueByName(pProps, szProperty, fValue).IgnoreResult();
  commandAccessor.FinishTransaction();

  update();
}

void plQtAudioEventTimelineWidget::ToggleSolo(const plUuid& instrumentGuid)
{
  if (m_SoloTracks.Contains(instrumentGuid))
  {
    m_SoloTracks.RemoveAndCopy(instrumentGuid);
  }
  else
  {
    m_SoloTracks.PushBack(instrumentGuid);
  }

  update();
}

void plQtAudioEventTimelineWidget::mouseMoveEvent(QMouseEvent* pEvent)
{
  if (m_ActiveHandle != DragHandle::None)
  {
    ApplyHandleDrag(m_ActiveHandle, pEvent->pos(), false);
    return;
  }

  if (m_bDraggingClip)
  {
    // snap to 10 ms; the timeline extent and the stop-fade marker follow the clip live
    m_fDragClipDelay = plMath::Max(0.0, plMath::RoundToMultiple(PixelToTime(pEvent->pos().x()) - m_fDragGrabOffset, 0.01));
    updateGeometry();
    update();
    return;
  }

  // hover feedback: handles resize, clips move
  if (PickHandle(pEvent->pos()) != DragHandle::None)
  {
    setCursor(Qt::SizeHorCursor);
  }
  else
  {
    bool bOverClip = false;
    for (const auto& hit : m_HitRects)
    {
      if (hit.m_Rect.contains(pEvent->pos()))
      {
        // only instrument clips are movable; automation chips are not
        if (const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(hit.m_Guid))
          bOverClip = pObject->GetParentProperty().IsEqual("Instruments");
        break;
      }
    }

    setCursor(bOverClip ? Qt::SizeAllCursor : Qt::ArrowCursor);
  }

  QWidget::mouseMoveEvent(pEvent);
}

void plQtAudioEventTimelineWidget::mouseReleaseEvent(QMouseEvent* pEvent)
{
  if (m_ActiveHandle != DragHandle::None)
  {
    ApplyHandleDrag(m_ActiveHandle, pEvent->pos(), true);
    m_ActiveHandle = DragHandle::None;
    return;
  }

  if (m_bDraggingClip)
  {
    const plUuid guid = m_DragClipGuid;
    const double fDelay = m_fDragClipDelay;

    m_bDraggingClip = false;
    m_DragClipGuid = plUuid();

    if (const plDocumentObject* pInstrument = m_pDocument->GetObjectManager()->GetObject(guid))
    {
      if (plMath::Abs(pInstrument->GetTypeAccessor().GetValue("StartDelay").ConvertTo<double>() - fDelay) > 0.0001)
      {
        plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
        accessor.StartTransaction("Move Instrument");
        accessor.SetValueByName(pInstrument, "StartDelay", static_cast<float>(fDelay)).IgnoreResult();
        accessor.FinishTransaction();
      }
    }

    updateGeometry();
    update();
    return;
  }

  QWidget::mouseReleaseEvent(pEvent);
}

void plQtAudioEventTimelineWidget::mousePressEvent(QMouseEvent* pEvent)
{
  if (pEvent->button() == Qt::LeftButton)
  {
    // loop-region / fade handles win over selection
    if (const DragHandle handle = PickHandle(pEvent->pos()); handle != DragHandle::None)
    {
      m_ActiveHandle = handle;
      ApplyHandleDrag(handle, pEvent->pos(), false);
      return;
    }

    // mute (persisted on the instrument) and solo (session-only) toggles come first
    for (const auto& hit : m_MuteHitRects)
    {
      if (hit.m_Rect.contains(pEvent->pos()))
      {
        if (const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(hit.m_Guid))
        {
          const bool bMuted = pObject->GetTypeAccessor().GetValue("Mute").ConvertTo<bool>();

          plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
          accessor.StartTransaction(bMuted ? "Unmute Instrument" : "Mute Instrument");
          accessor.SetValueByName(pObject, "Mute", !bMuted).IgnoreResult();
          accessor.FinishTransaction();
        }
        update();
        return;
      }
    }

    for (const auto& hit : m_SoloHitRects)
    {
      if (hit.m_Rect.contains(pEvent->pos()))
      {
        ToggleSolo(hit.m_Guid);
        return;
      }
    }

    for (const auto& hit : m_HitRects)
    {
      if (hit.m_Rect.contains(pEvent->pos()))
      {
        const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(hit.m_Guid);

        if (pObject != nullptr)
        {
          m_pDocument->GetSelectionManager()->SetSelection(pObject);

          // instrument clips can be dragged along the timeline to change their start delay
          if (pObject->GetParentProperty().IsEqual("Instruments"))
          {
            m_bDraggingClip = true;
            m_DragClipGuid = hit.m_Guid;
            m_fDragClipDelay = pObject->GetTypeAccessor().GetValue("StartDelay").ConvertTo<double>();
            m_fDragGrabOffset = PixelToTime(pEvent->pos().x()) - m_fDragClipDelay;
          }
        }

        update();
        return;
      }
    }

    for (const auto& hit : m_HeaderHitRects)
    {
      if (hit.m_Rect.contains(pEvent->pos()))
      {
        if (const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(hit.m_Guid))
        {
          m_pDocument->GetSelectionManager()->SetSelection(pObject);
        }
        update();
        return;
      }
    }

    if (const plDocumentObject* pProps = GetPropertiesObject())
    {
      m_pDocument->GetSelectionManager()->SetSelection(pProps);
    }
    update();
  }

  QWidget::mousePressEvent(pEvent);
}

void plQtAudioEventTimelineWidget::mouseDoubleClickEvent(QMouseEvent* pEvent)
{
  if (pEvent->button() == Qt::LeftButton)
  {
    for (const auto& hit : m_HeaderHitRects)
    {
      if (hit.m_Rect.contains(pEvent->pos()))
      {
        BeginRename(hit.m_Guid, hit.m_Rect);
        return;
      }
    }
  }

  QWidget::mouseDoubleClickEvent(pEvent);
}

void plQtAudioEventTimelineWidget::BeginRename(const plUuid& instrumentGuid, const QRect& headerRect)
{
  const plDocumentObject* pInstrument = m_pDocument->GetObjectManager()->GetObject(instrumentGuid);
  if (pInstrument == nullptr)
    return;

  if (m_pRenameEdit != nullptr)
  {
    m_pRenameEdit->deleteLater();
    m_pRenameEdit = nullptr;
  }

  const plString sName = pInstrument->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();

  m_pRenameEdit = new QLineEdit(this);
  m_pRenameEdit->setGeometry(headerRect.adjusted(18, 4, -6, -(headerRect.height() - 26)));
  m_pRenameEdit->setStyleSheet(QString("QLineEdit { background: #333333; border: 1px solid %1; border-radius: 2px; color: #ffffff; padding: 1px 4px; }").arg(m_Accent.name()));
  m_pRenameEdit->setText(QString::fromUtf8(sName.GetData()));
  m_pRenameEdit->selectAll();
  m_pRenameEdit->show();
  m_pRenameEdit->setFocus();

  connect(m_pRenameEdit, &QLineEdit::editingFinished, this, [this, instrumentGuid]()
    {
      if (m_pRenameEdit == nullptr)
        return;

      const QString sNewName = m_pRenameEdit->text().trimmed();

      m_pRenameEdit->deleteLater();
      m_pRenameEdit = nullptr;

      if (const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(instrumentGuid))
      {
        const plString sOldName = pObject->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
        const plStringBuilder sNew = sNewName.toUtf8().data();

        if (!sNew.IsEqual(sOldName))
        {
          plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
          accessor.StartTransaction("Rename Instrument");
          accessor.SetValueByName(pObject, "Name", sNew.GetView()).IgnoreResult();
          accessor.FinishTransaction();
        }
      }

      update(); });
}

//////////////////////////////////////////////////////////////////////////
// plQtAudioCurvePreviewWidget
//////////////////////////////////////////////////////////////////////////

plQtAudioCurvePreviewWidget::plQtAudioCurvePreviewWidget(QWidget* pParent, plDocument* pDocument, const QColor& accent)
  : QWidget(pParent)
  , m_pDocument(pDocument)
  , m_Accent(accent)
{
  setMinimumHeight(64);
  setMouseTracking(false);
  setToolTip("Drag points to shape the curve \xC2\xB7 double-click to add a point \xC2\xB7 right-click a point to remove it");
}

void plQtAudioCurvePreviewWidget::SetAutomation(const plUuid& automationGuid)
{
  m_AutomationGuid = automationGuid;
  update();
}

void plQtAudioCurvePreviewWidget::GatherPoints(plHybridArray<CurvePoint, 8>& out_points) const
{
  out_points.Clear();

  const plDocumentObject* pAutomation = m_AutomationGuid.IsValid() ? m_pDocument->GetObjectManager()->GetObject(m_AutomationGuid) : nullptr;
  if (pAutomation == nullptr)
    return;

  plHybridArray<const plDocumentObject*, 8> objects;
  GatherChildrenOfProperty(pAutomation, "Points", objects);

  for (const plDocumentObject* pPoint : objects)
  {
    auto& out = out_points.ExpandAndGetRef();
    out.m_Guid = pPoint->GetGuid();
    out.m_fX = pPoint->GetTypeAccessor().GetValue("X").ConvertTo<double>();
    out.m_fY = pPoint->GetTypeAccessor().GetValue("Y").ConvertTo<double>();
  }

  out_points.Sort([](const CurvePoint& a, const CurvePoint& b)
    { return a.m_fX < b.m_fX; });
}

void plQtAudioCurvePreviewWidget::GetRanges(double& out_fMinX, double& out_fMaxX, double& out_fMaxY) const
{
  out_fMinX = 0.0;
  out_fMaxX = 1.0;
  out_fMaxY = 1.0;

  // the parameter's authored range is the curve's x domain, so dragging maps to real values
  const plDocumentObject* pAutomation = m_AutomationGuid.IsValid() ? m_pDocument->GetObjectManager()->GetObject(m_AutomationGuid) : nullptr;
  if (pAutomation != nullptr)
  {
    const plString sParam = pAutomation->GetTypeAccessor().GetValue("Parameter").ConvertTo<plString>();

    const auto& rootChildren = m_pDocument->GetObjectManager()->GetRootObject()->GetChildren();
    if (!rootChildren.IsEmpty())
    {
      plHybridArray<const plDocumentObject*, 8> parameters;
      GatherChildrenOfProperty(rootChildren[0], "Parameters", parameters);

      for (const plDocumentObject* pParameter : parameters)
      {
        if (pParameter->GetTypeAccessor().GetValue("Name").ConvertTo<plString>() == sParam)
        {
          out_fMinX = pParameter->GetTypeAccessor().GetValue("Min").ConvertTo<double>();
          out_fMaxX = pParameter->GetTypeAccessor().GetValue("Max").ConvertTo<double>();
          break;
        }
      }
    }
  }

  if (out_fMaxX <= out_fMinX)
    out_fMaxX = out_fMinX + 1.0;

  plHybridArray<CurvePoint, 8> points;
  GatherPoints(points);
  for (const CurvePoint& pt : points)
  {
    out_fMaxY = plMath::Max(out_fMaxY, pt.m_fY);
    out_fMinX = plMath::Min(out_fMinX, pt.m_fX);
    out_fMaxX = plMath::Max(out_fMaxX, pt.m_fX);
  }

  out_fMaxY = plMath::Max(out_fMaxY, 1.0);
}

QRectF plQtAudioCurvePreviewWidget::GetPlotArea() const
{
  return QRectF(rect().adjusted(0, 0, -1, -1)).adjusted(8, 8, -8, -8);
}

QPointF plQtAudioCurvePreviewWidget::ToWidget(const QRectF& plot, double fX, double fY) const
{
  double fMinX, fMaxX, fMaxY;
  GetRanges(fMinX, fMaxX, fMaxY);

  const double fTx = (fX - fMinX) / (fMaxX - fMinX);
  const double fTy = fY / fMaxY;
  return QPointF(plot.left() + fTx * plot.width(), plot.bottom() - fTy * plot.height());
}

void plQtAudioCurvePreviewWidget::FromWidget(const QRectF& plot, const QPointF& pos, double& out_fX, double& out_fY) const
{
  double fMinX, fMaxX, fMaxY;
  GetRanges(fMinX, fMaxX, fMaxY);

  const double fTx = plMath::Clamp((pos.x() - plot.left()) / plMath::Max(plot.width(), 1.0), 0.0, 1.0);
  const double fTy = plMath::Clamp((plot.bottom() - pos.y()) / plMath::Max(plot.height(), 1.0), 0.0, 1.0);

  out_fX = fMinX + fTx * (fMaxX - fMinX);
  out_fY = fTy * fMaxY;
}

plInt32 plQtAudioCurvePreviewWidget::FindPointAt(const QPoint& pos) const
{
  plHybridArray<CurvePoint, 8> points;
  GatherPoints(points);

  const QRectF plot = GetPlotArea();

  for (plUInt32 i = 0; i < points.GetCount(); ++i)
  {
    const QPointF widgetPos = ToWidget(plot, points[i].m_fX, points[i].m_fY);
    if (QLineF(widgetPos, QPointF(pos)).length() <= 7.0)
      return static_cast<plInt32>(i);
  }

  return -1;
}

void plQtAudioCurvePreviewWidget::paintEvent(QPaintEvent* pEvent)
{
  PL_IGNORE_UNUSED(pEvent);

  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  const QRect area = rect().adjusted(0, 0, -1, -1);

  p.setPen(QPen(QColor(48, 48, 48), 1));
  p.setBrush(QColor(20, 20, 20));
  p.drawRect(area);

  const plDocumentObject* pAutomation = m_AutomationGuid.IsValid() ? m_pDocument->GetObjectManager()->GetObject(m_AutomationGuid) : nullptr;

  if (pAutomation == nullptr)
  {
    p.setPen(QColor(142, 142, 142));
    p.drawText(area, Qt::AlignCenter, "no automation");
    return;
  }

  plHybridArray<CurvePoint, 8> points;
  GatherPoints(points);

  if (points.IsEmpty())
  {
    p.setPen(QColor(142, 142, 142));
    p.drawText(area, Qt::AlignCenter, "double-click to add a point");
    return;
  }

  const QRectF plot = GetPlotArea();

  double fMinX, fMaxX, fMaxY;
  GetRanges(fMinX, fMaxX, fMaxY);

  // baseline at y=1 (no modulation)
  {
    const double fY = plot.bottom() - (1.0 / fMaxY) * plot.height();
    p.setPen(QPen(QColor(60, 60, 60), 1, Qt::DotLine));
    p.drawLine(QPointF(plot.left(), fY), QPointF(plot.right(), fY));
  }

  QPolygonF polyline;
  for (const CurvePoint& pt : points)
    polyline << ToWidget(plot, pt.m_fX, pt.m_fY);

  p.setPen(QPen(m_Accent, 2));
  p.drawPolyline(polyline);

  for (const QPointF& pt : polyline)
  {
    p.setBrush(m_Accent);
    p.setPen(QPen(QColor(20, 20, 20), 1));
    p.drawEllipse(pt, 4.0, 4.0);
  }

  // axis hints
  QFont smallFont = font();
  smallFont.setPointSizeF(7.0);
  p.setFont(smallFont);
  p.setPen(QColor(120, 120, 120));
  p.drawText(QRectF(plot.left(), plot.bottom() - 11, plot.width() * 0.5, 11), Qt::AlignLeft | Qt::AlignBottom, QString::asprintf("%.2f", fMinX));
  p.drawText(QRectF(plot.center().x(), plot.bottom() - 11, plot.width() * 0.5, 11), Qt::AlignRight | Qt::AlignBottom, QString::asprintf("%.2f", fMaxX));
}

void plQtAudioCurvePreviewWidget::mousePressEvent(QMouseEvent* pEvent)
{
  const plInt32 iPoint = FindPointAt(pEvent->pos());

  plHybridArray<CurvePoint, 8> points;
  GatherPoints(points);

  if (pEvent->button() == Qt::LeftButton && iPoint >= 0)
  {
    m_bDragging = true;
    m_DragPointGuid = points[iPoint].m_Guid;
    return;
  }

  if (pEvent->button() == Qt::RightButton && iPoint >= 0)
  {
    // keep at least two points so the curve stays evaluable
    if (points.GetCount() <= 2)
      return;

    if (const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(points[iPoint].m_Guid))
    {
      plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
      accessor.StartTransaction("Remove Curve Point");
      if (accessor.RemoveObject(pObject).Failed())
      {
        accessor.CancelTransaction();
        return;
      }
      accessor.FinishTransaction();
    }

    update();
    return;
  }

  QWidget::mousePressEvent(pEvent);
}

void plQtAudioCurvePreviewWidget::mouseMoveEvent(QMouseEvent* pEvent)
{
  if (!m_bDragging || !m_DragPointGuid.IsValid())
  {
    QWidget::mouseMoveEvent(pEvent);
    return;
  }

  const plDocumentObject* pPoint = m_pDocument->GetObjectManager()->GetObject(m_DragPointGuid);
  if (pPoint == nullptr)
    return;

  double fX = 0.0, fY = 0.0;
  FromWidget(GetPlotArea(), QPointF(pEvent->pos()), fX, fY);

  plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
  accessor.StartTransaction("Move Curve Point");
  accessor.SetValueByName(pPoint, "X", static_cast<float>(fX)).IgnoreResult();
  accessor.SetValueByName(pPoint, "Y", static_cast<float>(fY)).IgnoreResult();
  accessor.FinishTransaction();

  update();
}

void plQtAudioCurvePreviewWidget::mouseReleaseEvent(QMouseEvent* pEvent)
{
  PL_IGNORE_UNUSED(pEvent);
  m_bDragging = false;
  m_DragPointGuid = plUuid();
}

void plQtAudioCurvePreviewWidget::mouseDoubleClickEvent(QMouseEvent* pEvent)
{
  if (pEvent->button() != Qt::LeftButton)
    return;

  const plDocumentObject* pAutomation = m_AutomationGuid.IsValid() ? m_pDocument->GetObjectManager()->GetObject(m_AutomationGuid) : nullptr;
  if (pAutomation == nullptr)
    return;

  // double-click on an existing point does nothing (dragging is the edit gesture)
  if (FindPointAt(pEvent->pos()) >= 0)
    return;

  double fX = 0.0, fY = 0.0;
  FromWidget(GetPlotArea(), QPointF(pEvent->pos()), fX, fY);

  plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
  accessor.StartTransaction("Add Curve Point");

  plUuid pointGuid;
  if (accessor.AddObjectByName(pAutomation, "Points", -1, plGetStaticRTTI<plAudioEventCurvePoint>(), pointGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  const plDocumentObject* pPoint = m_pDocument->GetObjectManager()->GetObject(pointGuid);
  accessor.SetValueByName(pPoint, "X", static_cast<float>(fX)).IgnoreResult();
  accessor.SetValueByName(pPoint, "Y", static_cast<float>(fY)).IgnoreResult();

  accessor.FinishTransaction();

  update();
}

//////////////////////////////////////////////////////////////////////////
// plAudioEventAssetDocumentWindow
//////////////////////////////////////////////////////////////////////////

plAudioEventAssetDocumentWindow::plAudioEventAssetDocumentWindow(plDocument* pDocument)
  : plQtDocumentWindow(pDocument)
{
  m_pAssetDoc = static_cast<plAudioEventAssetDocument*>(pDocument);
  m_Accent = AccentColor(pDocument);
  m_bEngineViews = plAudioEngineUi::UseEngineViews();
  m_AuditionRng.InitializeFromCurrentTime();

  // Menu Bar
  {
    plQtMenuBarActionMapView* pMenuBar = static_cast<plQtMenuBarActionMapView*>(menuBar());
    plActionContext context;
    context.m_sMapping = "AudioEventAssetMenuBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pMenuBar->SetActionContext(context);
  }

  // Tool Bar
  {
    plQtToolBarActionMapView* pToolBar = new plQtToolBarActionMapView("Toolbar", this);
    plActionContext context;
    context.m_sMapping = "AudioEventAssetToolBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pToolBar->SetActionContext(context);
    pToolBar->setObjectName("AudioEventAssetWindowToolBar");
    addToolBar(pToolBar);
  }

  // Central
  {
    QWidget* pCentralWidget = new QWidget(this);

    QVBoxLayout* pCentralLayout = new QVBoxLayout(pCentralWidget);
    pCentralLayout->setContentsMargins(0, 0, 0, 0);
    pCentralLayout->setSpacing(0);

    if (m_bEngineViews)
      CreateEngineCentralWidget(pCentralWidget, pCentralLayout);
    else
      CreateQtCentralWidget(pCentralWidget, pCentralLayout);

    plQtDocumentPanel* pCentral = new plQtDocumentPanel(this, pDocument);
    pCentral->setObjectName("AudioEventTimelineDockWidget");
    pCentral->setWindowTitle("Audio Event");
    pCentral->setWidget(pCentralWidget, ads::CDockWidget::ForceNoScrollArea);
    m_pDockManager->setCentralWidget(pCentral);
  }

  // Right dock: event macros (root properties, pinned) + asset status
  {
    plQtDocumentPanel* pPropertyPanel = new plQtDocumentPanel(this, pDocument);
    pPropertyPanel->setObjectName("AudioEventAssetDockWidget");
    pPropertyPanel->setWindowTitle("Event Macros");
    pPropertyPanel->show();

    plQtPropertyGridWidget* pPropertyGrid = new plQtPropertyGridWidget(pPropertyPanel, pDocument, false);

    plDeque<const plDocumentObject*> selection;
    selection.PushBack(GetPropertiesObject());
    pPropertyGrid->SetSelection(selection);

    QWidget* pWidget = new QWidget();
    pWidget->setObjectName("Group");
    pWidget->setLayout(new QVBoxLayout());
    pWidget->setContentsMargins(0, 0, 0, 0);
    pWidget->layout()->setContentsMargins(0, 0, 0, 0);
    pWidget->layout()->addWidget(new plQtAssetStatusIndicator((plAssetDocument*)GetDocument()));
    pWidget->layout()->addWidget(pPropertyGrid);

    pPropertyPanel->setWidget(pWidget, ads::CDockWidget::ForceNoScrollArea);

    m_pDockManager->addDockWidgetTab(ads::RightDockWidgetArea, pPropertyPanel);
  }

  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);

  GetDocument()->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plAudioEventAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plAudioEventAssetDocumentWindow::StructureEventHandler, this));
  GetDocument()->GetSelectionManager()->m_Events.AddEventHandler(plMakeDelegate(&plAudioEventAssetDocumentWindow::SelectionEventHandler, this));

  // poll the audition meter (only does work while something is auditioning)
  m_pMeterTimer = new QTimer(this);
  m_pMeterTimer->setInterval(50);
  connect(m_pMeterTimer, &QTimer::timeout, this, [this]()
    {
      const float fPeak = plAudioPreviewService::GetSingleton().GetMasterPeak();

      if (m_pMeterView != nullptr)
        m_pMeterView->SetPeak(fPeak);

      if (m_pLevelMeter != nullptr)
        m_pLevelMeter->SetPeak(fPeak);

      if (m_pMeterLabel != nullptr)
      {
        m_pMeterLabel->setText(fPeak <= 0.001f ? QString::fromUtf8("-\xE2\x88\x9E dB") : QString::asprintf("%.1f dB", plAudioMixerUtils::LinearToDb(fPeak)));
      } });
  m_pMeterTimer->start();

  FinishWindowCreation();

  QueueRefresh();
}

void plAudioEventAssetDocumentWindow::CreateEngineCentralWidget(QWidget* pCentralWidget, QVBoxLayout* pCentralLayout)
{
  using plEditorUi::Icon;

  // toolbar row: audition, edit buttons, the OUT meter
  {
    QWidget* pRow = new QWidget(pCentralWidget);
    QHBoxLayout* pRowLayout = new QHBoxLayout(pRow);
    pRowLayout->setContentsMargins(0, 0, 0, 0);
    pRowLayout->setSpacing(0);

    m_pToolStrip = new plQtEngineControlStrip(pRow);
    m_pToolStrip->setObjectName("AudioEventToolStrip");
    m_uiPlay = m_pToolStrip->AddIconButton(Icon::Play, "Auditions the event in the editor (respects per-track M mute and S solo)");
    m_uiStop = m_pToolStrip->AddIconButton(Icon::Stop, "Stops the audition");
    m_pToolStrip->AddSeparator();
    m_uiAddInstrument = m_pToolStrip->AddIconTextButton(Icon::Add, "Instrument", "Adds an instrument track (fill its playlist with '+ Add Files...' in the deck; double-click a track header to rename)");
    m_uiRemoveSelected = m_pToolStrip->AddIconTextButton(Icon::Remove, "Remove Selected", "Removes the selected instrument / parameter / automation");
    m_pToolStrip->AddSpacer();
    m_pToolStrip->AddLabel("OUT", -1.0f);

    m_pToolStrip->m_OnChanged = [this](plUInt32 uiControl, plInt32 iValue)
    {
      PL_IGNORE_UNUSED(iValue);

      if (uiControl == m_uiPlay)
        OnPlayEvent();
      else if (uiControl == m_uiStop)
        OnStopEvent();
      else if (uiControl == m_uiAddInstrument)
        OnAddInstrument();
      else if (uiControl == m_uiRemoveSelected)
        OnRemoveSelected();
    };

    pRowLayout->addWidget(m_pToolStrip, 1);

    m_pMeterView = new plQtAudioMeterView(pRow);
    pRowLayout->addWidget(m_pMeterView);

    pCentralLayout->addWidget(pRow);
  }

  QSplitter* pSplitter = new QSplitter(Qt::Vertical, pCentralWidget);
  pSplitter->setHandleWidth(1);
  pSplitter->setChildrenCollapsible(false);

  // parameter tabs and the timeline
  {
    m_pTimelineView = new plQtAudioEventTimelineView(pSplitter, GetDocument());
    m_pTimelineView->m_OnAddParameter = [this]()
    { OnAddParameter(); };
    pSplitter->addWidget(m_pTimelineView);
  }

  // deck: INSTRUMENT and PLAYBACK above, AUTOMATION and DETAILS below
  {
    QWidget* pDeck = new QWidget(pSplitter);
    QGridLayout* pDeckLayout = new QGridLayout(pDeck);
    pDeckLayout->setContentsMargins(0, 0, 0, 0);
    pDeckLayout->setSpacing(0);
    pDeckLayout->setColumnStretch(0, 1);
    pDeckLayout->setColumnStretch(1, 1);

    m_pDeckView = new plQtAudioEventDeckView(pDeck, GetDocument());
    m_pDeckView->m_OnInstrumentPicked = [this](const plUuid& instrumentGuid)
    {
      m_ActiveInstrumentGuid = instrumentGuid;
      if (const plDocumentObject* pObject = GetDocument()->GetObjectManager()->GetObject(instrumentGuid))
        GetDocument()->GetSelectionManager()->SetSelection(pObject);
    };
    m_pDeckView->m_OnAddFiles = [this]()
    { OnAddFilesToInstrument(); };
    m_pDeckView->m_OnAddClip = [this]()
    { OnAddClipToInstrument(); };
    m_pDeckView->m_OnAddSilence = [this]()
    { OnAddPlaylistEntry(); };
    m_pDeckView->m_OnPickEntryClip = [this](const plUuid& entryGuid)
    { PickEntryClip(entryGuid); };
    pDeckLayout->addWidget(m_pDeckView, 0, 0, 1, 2);

    m_pAutomationView = new plQtAudioEventAutomationView(pDeck, GetDocument());
    m_pAutomationView->m_OnAddAutomation = [this](plUInt8 uiTarget, const plString& sParameter)
    { OnAddAutomationFor(uiTarget, sParameter); };
    pDeckLayout->addWidget(m_pAutomationView, 1, 0);

    // DETAILS · selection: the property grid of the selected object
    QWidget* pDetails = new QWidget(pDeck);
    QVBoxLayout* pDetailsLayout = new QVBoxLayout(pDetails);
    pDetailsLayout->setContentsMargins(0, 0, 0, 0);
    pDetailsLayout->setSpacing(0);
    pDetailsLayout->addWidget(new plQtAudioSectionHeaderView(pDetails, "Details \xC2\xB7 selection", plAudioEngineUi::AudioColor()));
    pDetailsLayout->addWidget(new plQtPropertyGridWidget(pDetails, GetDocument()), 1);
    pDeckLayout->addWidget(pDetails, 1, 1);
    pDeckLayout->setRowStretch(1, 1);

    pSplitter->addWidget(pDeck);
  }

  pSplitter->setStretchFactor(0, 3);
  pSplitter->setStretchFactor(1, 2);
  pCentralLayout->addWidget(pSplitter, 1);

  m_pStatusView = new plQtAudioStatusRowView(pCentralWidget);
  m_pStatusView->SetRight("backend: MiniAudio");
  pCentralLayout->addWidget(m_pStatusView);
}

void plAudioEventAssetDocumentWindow::CreateQtCentralWidget(QWidget* pCentralWidget, QVBoxLayout* pCentralLayout)
{
  pCentralWidget->setStyleSheet(AudioToolStyle(m_Accent));

  {
    // edit button row
    {
      QWidget* pButtonRow = new QWidget(pCentralWidget);
      pButtonRow->setObjectName("AudioButtonRow");

      QHBoxLayout* pButtonLayout = new QHBoxLayout(pButtonRow);
      pButtonLayout->setContentsMargins(6, 5, 6, 5);
      pButtonLayout->setSpacing(6);

      auto addButton = [&](const char* szText, const char* szToolTip, void (plAudioEventAssetDocumentWindow::*pFunc)())
      {
        QToolButton* pButton = new QToolButton(pButtonRow);
        pButton->setText(QString::fromUtf8(szText));
        pButton->setToolTip(QString::fromUtf8(szToolTip));
        connect(pButton, &QToolButton::clicked, this, pFunc);
        pButtonLayout->addWidget(pButton);
        return pButton;
      };

      QToolButton* pPlay = addButton("\xE2\x96\xB6 Play", "Auditions the event in the editor (respects per-track M mute and S solo)", &plAudioEventAssetDocumentWindow::OnPlayEvent);
      pPlay->setObjectName("PlayButton");
      addButton("\xE2\x96\xA0 Stop", "Stops the audition", &plAudioEventAssetDocumentWindow::OnStopEvent);
      pButtonLayout->addSpacing(18);

      addButton("+ Instrument", "Adds an instrument track (fill its playlist with '+ Add Files...' in the deck; double-click a track header to rename)", &plAudioEventAssetDocumentWindow::OnAddInstrument);
      pButtonLayout->addSpacing(18);
      addButton("Remove Selected", "Removes the selected instrument / parameter / automation", &plAudioEventAssetDocumentWindow::OnRemoveSelected);

      pButtonLayout->addStretch();

      // output level meter (mockup's top-bar OUT meter)
      {
        QLabel* pOutLabel = new QLabel("OUT", pButtonRow);
        pOutLabel->setStyleSheet("color: #8e8e8e; font-size: 10px; font-weight: bold; letter-spacing: 1px;");
        pButtonLayout->addWidget(pOutLabel);

        m_pLevelMeter = new plQtAudioLevelMeterWidget(pButtonRow);
        pButtonLayout->addWidget(m_pLevelMeter);

        m_pMeterLabel = new QLabel("-\xE2\x88\x9E dB", pButtonRow);
        m_pMeterLabel->setStyleSheet("color: #cccccc; font-size: 10px; min-width: 52px;");
        pButtonLayout->addWidget(m_pMeterLabel);
      }

      pCentralLayout->addWidget(pButtonRow);
    }

    // parameter tab strip (Timeline | <parameters...> | + Parameter)
    {
      m_pTabsHost = new QWidget(pCentralWidget);
      m_pTabsHost->setObjectName("TabStrip");

      m_pTabsLayout = new QHBoxLayout(m_pTabsHost);
      m_pTabsLayout->setContentsMargins(8, 5, 8, 0);
      m_pTabsLayout->setSpacing(2);

      pCentralLayout->addWidget(m_pTabsHost);
    }

    QSplitter* pSplitter = new QSplitter(Qt::Vertical, pCentralWidget);

    // timeline in a scroll area
    {
      QScrollArea* pScroll = new QScrollArea(pSplitter);
      pScroll->setObjectName("AudioScroll");
      pScroll->setWidgetResizable(true);
      pScroll->setFrameShape(QFrame::NoFrame);

      m_pTimeline = new plQtAudioEventTimelineWidget(pScroll, GetDocument(), m_Accent);
      pScroll->setWidget(m_pTimeline);

      pSplitter->addWidget(pScroll);
    }

    // deck: instrument playlist | playback | automation | details
    {
      QWidget* pDeck = new QWidget(pSplitter);
      pDeck->setObjectName("Deck");

      QHBoxLayout* pDeckLayout = new QHBoxLayout(pDeck);
      pDeckLayout->setContentsMargins(0, 0, 0, 0);
      pDeckLayout->setSpacing(0);

      // --- column 1: instrument playlist
      {
        QWidget* pColumn = new QWidget(pDeck);
        pColumn->setObjectName("DeckColumn");

        QVBoxLayout* pColumnLayout = new QVBoxLayout(pColumn);
        pColumnLayout->setContentsMargins(0, 0, 0, 6);
        pColumnLayout->setSpacing(2);

        m_pInstrumentHeader = new QLabel("INSTRUMENT", pColumn);
        m_pInstrumentHeader->setObjectName("DeckHeader");
        pColumnLayout->addWidget(m_pInstrumentHeader);

        m_pInstrumentCombo = new QComboBox(pColumn);
        m_pInstrumentCombo->setToolTip("Selects the instrument edited below");
        connect(m_pInstrumentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int iIndex)
          {
            if (m_bUpdatingUi || iIndex < 0)
              return;

            const plStringBuilder sGuid = m_pInstrumentCombo->itemData(iIndex).toString().toUtf8().data();
            if (!plConversionUtils::IsStringUuid(sGuid))
              return;

            const plUuid guid = plConversionUtils::ConvertStringToUuid(sGuid);
            m_ActiveInstrumentGuid = guid;

            if (const plDocumentObject* pObject = GetDocument()->GetObjectManager()->GetObject(guid))
            {
              GetDocument()->GetSelectionManager()->SetSelection(pObject);
            } });

        QWidget* pComboWrap = new QWidget(pColumn);
        QHBoxLayout* pComboLayout = new QHBoxLayout(pComboWrap);
        pComboLayout->setContentsMargins(10, 0, 10, 4);
        pComboLayout->addWidget(m_pInstrumentCombo);
        pColumnLayout->addWidget(pComboWrap);

        QScrollArea* pScroll = new QScrollArea(pColumn);
        pScroll->setObjectName("AudioScroll");
        pScroll->setWidgetResizable(true);
        pScroll->setFrameShape(QFrame::NoFrame);

        m_pPlaylistHost = new QWidget(pScroll);
        m_pPlaylistLayout = new QVBoxLayout(m_pPlaylistHost);
        m_pPlaylistLayout->setContentsMargins(10, 2, 10, 2);
        m_pPlaylistLayout->setSpacing(2);
        m_pPlaylistLayout->addStretch();

        pScroll->setWidget(m_pPlaylistHost);
        pColumnLayout->addWidget(pScroll, 1);

        pDeckLayout->addWidget(pColumn, 4);
      }

      // --- column 2: playback
      {
        QWidget* pColumn = new QWidget(pDeck);
        pColumn->setObjectName("DeckColumn");

        QVBoxLayout* pColumnLayout = new QVBoxLayout(pColumn);
        pColumnLayout->setContentsMargins(0, 0, 0, 6);
        pColumnLayout->setSpacing(2);

        QLabel* pHeader = new QLabel("PLAYBACK", pColumn);
        pHeader->setObjectName("DeckHeader");
        pColumnLayout->addWidget(pHeader);

        QWidget* pBody = new QWidget(pColumn);
        QVBoxLayout* pBodyLayout = new QVBoxLayout(pBody);
        pBodyLayout->setContentsMargins(10, 0, 10, 0);
        pBodyLayout->setSpacing(2);

        auto addSubLabel = [&](const char* szText)
        {
          QLabel* pLabel = new QLabel(QString::fromUtf8(szText), pBody);
          pLabel->setObjectName("DeckSubLabel");
          pBodyLayout->addWidget(pLabel);
        };

        addSubLabel("Playlist mode");

        m_pModeCombo = new QComboBox(pBody);
        m_pModeCombo->addItem("Random");
        m_pModeCombo->addItem("Shuffle (no repeat)");
        m_pModeCombo->addItem("Sequential");
        connect(m_pModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int iIndex)
          {
            if (m_bUpdatingUi || iIndex < 0)
              return;
            SetInstrumentValue("PlaylistMode", static_cast<plInt64>(iIndex), "Set Playlist Mode"); });
        pBodyLayout->addWidget(m_pModeCombo);

        addSubLabel("Volume randomization");

        {
          QWidget* pRow = new QWidget(pBody);
          QHBoxLayout* pRowLayout = new QHBoxLayout(pRow);
          pRowLayout->setContentsMargins(0, 0, 0, 0);
          pRowLayout->setSpacing(8);

          m_pVolumeRandSlider = new QSlider(Qt::Horizontal, pRow);
          m_pVolumeRandSlider->setRange(0, 240); // 0 .. 24.0 dB
          connect(m_pVolumeRandSlider, &QSlider::valueChanged, this, [this](int iValue)
            { if (m_pVolumeRandLabel != nullptr) m_pVolumeRandLabel->setText(QString::asprintf("\xC2\xB1%.1f dB", iValue / 10.0)); });
          connect(m_pVolumeRandSlider, &QSlider::sliderReleased, this, [this]()
            {
              if (m_bUpdatingUi)
                return;
              SetInstrumentValue("VolumeRandomization", m_pVolumeRandSlider->value() / 10.0f, "Set Volume Randomization"); });
          pRowLayout->addWidget(m_pVolumeRandSlider, 1);

          m_pVolumeRandLabel = new QLabel("\xC2\xB1"
                                          "0.0 dB",
            pRow);
          m_pVolumeRandLabel->setObjectName("DeckValueLabel");
          pRowLayout->addWidget(m_pVolumeRandLabel);

          pBodyLayout->addWidget(pRow);
        }

        addSubLabel("Pitch randomization");

        {
          QWidget* pRow = new QWidget(pBody);
          QHBoxLayout* pRowLayout = new QHBoxLayout(pRow);
          pRowLayout->setContentsMargins(0, 0, 0, 0);
          pRowLayout->setSpacing(8);

          m_pPitchRandSlider = new QSlider(Qt::Horizontal, pRow);
          m_pPitchRandSlider->setRange(0, 240); // 0 .. 24.0 st
          connect(m_pPitchRandSlider, &QSlider::valueChanged, this, [this](int iValue)
            { if (m_pPitchRandLabel != nullptr) m_pPitchRandLabel->setText(QString::asprintf("\xC2\xB1%.1f st", iValue / 10.0)); });
          connect(m_pPitchRandSlider, &QSlider::sliderReleased, this, [this]()
            {
              if (m_bUpdatingUi)
                return;
              SetInstrumentValue("PitchRandomization", m_pPitchRandSlider->value() / 10.0f, "Set Pitch Randomization"); });
          pRowLayout->addWidget(m_pPitchRandSlider, 1);

          m_pPitchRandLabel = new QLabel("\xC2\xB1"
                                         "0.0 st",
            pRow);
          m_pPitchRandLabel->setObjectName("DeckValueLabel");
          pRowLayout->addWidget(m_pPitchRandLabel);

          pBodyLayout->addWidget(pRow);
        }

        {
          QWidget* pRow = new QWidget(pBody);
          QHBoxLayout* pRowLayout = new QHBoxLayout(pRow);
          pRowLayout->setContentsMargins(0, 8, 0, 0);
          pRowLayout->setSpacing(8);

          m_pLoopCheck = new QCheckBox("Loop", pRow);
          m_pLoopCheck->setToolTip("Re-triggers the playlist while the event is active");
          connect(m_pLoopCheck, &QCheckBox::toggled, this, [this](bool bChecked)
            {
              if (m_bUpdatingUi)
                return;
              SetInstrumentValue("Loop", bChecked, "Set Loop"); });
          pRowLayout->addWidget(m_pLoopCheck);

          QLabel* pDelayLabel = new QLabel("Start delay", pRow);
          pDelayLabel->setObjectName("DeckSubLabel");
          pRowLayout->addWidget(pDelayLabel);

          m_pStartDelaySpin = new QDoubleSpinBox(pRow);
          m_pStartDelaySpin->setRange(0.0, 300.0);
          m_pStartDelaySpin->setDecimals(2);
          m_pStartDelaySpin->setSuffix(" s");
          m_pStartDelaySpin->setKeyboardTracking(false);
          connect(m_pStartDelaySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double fValue)
            {
              if (m_bUpdatingUi)
                return;
              SetInstrumentValue("StartDelay", static_cast<float>(fValue), "Set Start Delay"); });
          pRowLayout->addWidget(m_pStartDelaySpin);

          pRowLayout->addStretch();
          pBodyLayout->addWidget(pRow);
        }

        {
          QWidget* pRow = new QWidget(pBody);
          QHBoxLayout* pRowLayout = new QHBoxLayout(pRow);
          pRowLayout->setContentsMargins(0, 2, 0, 0);
          pRowLayout->setSpacing(8);

          QLabel* pRetriggerLabel = new QLabel("Retrigger gap", pRow);
          pRetriggerLabel->setObjectName("DeckSubLabel");
          pRetriggerLabel->setToolTip("Random loop re-trigger delay (scatterer). Zero = gapless.");
          pRowLayout->addWidget(pRetriggerLabel);

          m_pMinRetriggerSpin = new QDoubleSpinBox(pRow);
          m_pMinRetriggerSpin->setRange(0.0, 300.0);
          m_pMinRetriggerSpin->setDecimals(2);
          m_pMinRetriggerSpin->setSuffix(" s");
          m_pMinRetriggerSpin->setKeyboardTracking(false);
          connect(m_pMinRetriggerSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double fValue)
            {
              if (m_bUpdatingUi)
                return;
              SetInstrumentValue("MinRetriggerDelay", static_cast<float>(fValue), "Set Retrigger Delay"); });
          pRowLayout->addWidget(m_pMinRetriggerSpin);

          QLabel* pToLabel = new QLabel("to", pRow);
          pToLabel->setObjectName("DeckSubLabel");
          pRowLayout->addWidget(pToLabel);

          m_pMaxRetriggerSpin = new QDoubleSpinBox(pRow);
          m_pMaxRetriggerSpin->setRange(0.0, 300.0);
          m_pMaxRetriggerSpin->setDecimals(2);
          m_pMaxRetriggerSpin->setSuffix(" s");
          m_pMaxRetriggerSpin->setKeyboardTracking(false);
          connect(m_pMaxRetriggerSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double fValue)
            {
              if (m_bUpdatingUi)
                return;
              SetInstrumentValue("MaxRetriggerDelay", static_cast<float>(fValue), "Set Retrigger Delay"); });
          pRowLayout->addWidget(m_pMaxRetriggerSpin);

          pRowLayout->addStretch();
          pBodyLayout->addWidget(pRow);
        }

        pBodyLayout->addStretch();
        pColumnLayout->addWidget(pBody, 1);

        pDeckLayout->addWidget(pColumn, 3);
      }

      // --- column 3: automation
      {
        QWidget* pColumn = new QWidget(pDeck);
        pColumn->setObjectName("DeckColumn");

        QVBoxLayout* pColumnLayout = new QVBoxLayout(pColumn);
        pColumnLayout->setContentsMargins(0, 0, 0, 6);
        pColumnLayout->setSpacing(2);

        m_pAutomationHeader = new QLabel("AUTOMATION", pColumn);
        m_pAutomationHeader->setObjectName("DeckHeader");
        pColumnLayout->addWidget(m_pAutomationHeader);

        QWidget* pBody = new QWidget(pColumn);
        QVBoxLayout* pBodyLayout = new QVBoxLayout(pBody);
        pBodyLayout->setContentsMargins(10, 0, 10, 0);
        pBodyLayout->setSpacing(2);

        m_pAutomationTargetLabel = new QLabel("", pBody);
        m_pAutomationTargetLabel->setObjectName("DeckSubLabel");
        pBodyLayout->addWidget(m_pAutomationTargetLabel);

        m_pCurvePreview = new plQtAudioCurvePreviewWidget(pBody, GetDocument(), m_Accent);
        pBodyLayout->addWidget(m_pCurvePreview, 1);

        QLabel* pAddLabel = new QLabel("Add target", pBody);
        pAddLabel->setObjectName("DeckSubLabel");
        pBodyLayout->addWidget(pAddLabel);

        m_pAddAutomationButton = new QToolButton(pBody);
        m_pAddAutomationButton->setText("+ automate property...");
        m_pAddAutomationButton->setPopupMode(QToolButton::InstantPopup);
        m_pAddAutomationButton->setToolTip("Adds an automation curve driven by an event parameter");
        pBodyLayout->addWidget(m_pAddAutomationButton);

        pColumnLayout->addWidget(pBody, 1);

        pDeckLayout->addWidget(pColumn, 3);
      }

      // --- column 4: details (selection property grid — curve points, parameter ranges, names)
      {
        QWidget* pColumn = new QWidget(pDeck);
        pColumn->setObjectName("DeckColumn");

        QVBoxLayout* pColumnLayout = new QVBoxLayout(pColumn);
        pColumnLayout->setContentsMargins(0, 0, 0, 0);
        pColumnLayout->setSpacing(0);

        QLabel* pHeader = new QLabel("DETAILS \xC2\xB7 selection", pColumn);
        pHeader->setObjectName("DeckHeader");
        pColumnLayout->addWidget(pHeader);

        plQtPropertyGridWidget* pDetailsGrid = new plQtPropertyGridWidget(pColumn, GetDocument());
        pColumnLayout->addWidget(pDetailsGrid, 1);

        pDeckLayout->addWidget(pColumn, 3);
      }
    }

    {
      QWidget* pDeckHost = pSplitter->widget(1);
      PL_IGNORE_UNUSED(pDeckHost);
    }

    pSplitter->setStretchFactor(0, 3);
    pSplitter->setStretchFactor(1, 2);
    pCentralLayout->addWidget(pSplitter, 1);

    // status bar strip
    {
      m_pStatusLabel = new QLabel(pCentralWidget);
      m_pStatusLabel->setObjectName("StatusBar");
      pCentralLayout->addWidget(m_pStatusLabel);
    }
  }
}

plAudioEventAssetDocumentWindow::~plAudioEventAssetDocumentWindow()
{
  // leave the shared preview device running, but silence anything this window started
  plAudioPreviewService::GetSingleton().StopAll();

  GetDocument()->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plAudioEventAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plAudioEventAssetDocumentWindow::StructureEventHandler, this));
  GetDocument()->GetSelectionManager()->m_Events.RemoveEventHandler(plMakeDelegate(&plAudioEventAssetDocumentWindow::SelectionEventHandler, this));
}

const plDocumentObject* plAudioEventAssetDocumentWindow::GetPropertiesObject() const
{
  const auto& children = GetDocument()->GetObjectManager()->GetRootObject()->GetChildren();
  return children.IsEmpty() ? nullptr : children[0];
}

const plDocumentObject* plAudioEventAssetDocumentWindow::GetSelectedObjectOfProperty(const char* szParentProperty) const
{
  const plDocumentObject* pSelected = GetDocument()->GetSelectionManager()->GetCurrentObject();

  while (pSelected != nullptr && pSelected != GetDocument()->GetObjectManager()->GetRootObject())
  {
    if (pSelected->GetParentProperty().IsEqual(szParentProperty))
      return pSelected;

    pSelected = pSelected->GetParent();
  }

  return nullptr;
}

const plDocumentObject* plAudioEventAssetDocumentWindow::GetActiveInstrument() const
{
  if (const plDocumentObject* pSelected = GetSelectedObjectOfProperty("Instruments"))
    return pSelected;

  if (m_ActiveInstrumentGuid.IsValid())
  {
    if (const plDocumentObject* pObject = GetDocument()->GetObjectManager()->GetObject(m_ActiveInstrumentGuid))
    {
      if (pObject->GetParentProperty().IsEqual("Instruments"))
        return pObject;
    }
  }

  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherChildrenOfProperty(GetPropertiesObject(), "Instruments", instruments);
  return instruments.IsEmpty() ? nullptr : instruments[0];
}

const plDocumentObject* plAudioEventAssetDocumentWindow::GetActiveAutomation() const
{
  if (const plDocumentObject* pSelected = GetSelectedObjectOfProperty("Automations"))
    return pSelected;

  plHybridArray<const plDocumentObject*, 8> automations;
  GatherChildrenOfProperty(GetPropertiesObject(), "Automations", automations);
  return automations.IsEmpty() ? nullptr : automations[0];
}

void plAudioEventAssetDocumentWindow::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  // a changed sound file must be re-measured for the timeline clip length (clip durations come from the curator, uncached)
  if (e.m_sProperty == "File")
  {
    if (m_pTimeline != nullptr)
      m_pTimeline->m_DurationCache.Clear();

    if (m_pTimelineView != nullptr)
      m_pTimelineView->ClearDurationCache();
  }

  QueueRefresh();
}

bool plAudioEventAssetDocumentWindow::HasSoloTracks() const
{
  if (m_pTimelineView != nullptr)
    return m_pTimelineView->HasSolo();

  return m_pTimeline != nullptr && !m_pTimeline->m_SoloTracks.IsEmpty();
}

bool plAudioEventAssetDocumentWindow::IsSoloTrack(const plUuid& instrumentGuid) const
{
  if (m_pTimelineView != nullptr)
    return m_pTimelineView->IsSolo(instrumentGuid);

  return m_pTimeline != nullptr && m_pTimeline->IsSolo(instrumentGuid);
}

void plAudioEventAssetDocumentWindow::PickEntryClip(const plUuid& entryGuid)
{
  const plDocumentObject* pEntry = GetDocument()->GetObjectManager()->GetObject(entryGuid);
  if (pEntry == nullptr)
    return;

  plString sPicked;
  if (!PickClip(pEntry->GetTypeAccessor().GetValue("Clip").ConvertTo<plString>(), sPicked))
    return;

  // looked up again: the asset browser runs its own event loop
  pEntry = GetDocument()->GetObjectManager()->GetObject(entryGuid);
  if (pEntry == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Set Playlist Clip");
  accessor.SetValueByName(pEntry, "Clip", sPicked).IgnoreResult();
  accessor.SetValueByName(pEntry, "File", plString()).IgnoreResult();
  accessor.FinishTransaction();
}

void plAudioEventAssetDocumentWindow::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  PL_IGNORE_UNUSED(e);
  QueueRefresh();
}

void plAudioEventAssetDocumentWindow::SelectionEventHandler(const plSelectionManagerEvent& e)
{
  PL_IGNORE_UNUSED(e);
  QueueRefresh();
}

void plAudioEventAssetDocumentWindow::QueueRefresh()
{
  if (m_bRefreshQueued)
    return;

  m_bRefreshQueued = true;

  QTimer::singleShot(0, this, [this]()
    {
      m_bRefreshQueued = false;

      if (m_pTimeline != nullptr)
      {
        m_pTimeline->updateGeometry();
        m_pTimeline->update();
      }

      // the names this event defines become dropdown choices on the audio components
      {
        plHybridArray<const plDocumentObject*, 8> parameters, instruments;
        GatherChildrenOfProperty(GetPropertiesObject(), "Parameters", parameters);
        GatherChildrenOfProperty(GetPropertiesObject(), "Instruments", instruments);

        for (const plDocumentObject* pParameter : parameters)
        {
          plAudioEnumRegistry::RegisterRtpcName(pParameter->GetTypeAccessor().GetValue("Name").ConvertTo<plString>());
        }

        for (const plDocumentObject* pInstrument : instruments)
        {
          plAudioEnumRegistry::RegisterSwitchStateName(pInstrument->GetTypeAccessor().GetValue("SwitchState").ConvertTo<plString>());
        }
      }

      if (m_bEngineViews)
      {
        const plDocumentObject* pInstrument = GetActiveInstrument();
        const plDocumentObject* pAutomation = GetActiveAutomation();

        m_pTimelineView->Refresh();
        m_pDeckView->SetInstrument(pInstrument != nullptr ? pInstrument->GetGuid() : plUuid());
        m_pAutomationView->SetAutomation(pAutomation != nullptr ? pAutomation->GetGuid() : plUuid());
      }
      else
      {
        RebuildParameterTabs();
        RebuildInstrumentCombo();
        RebuildPlaylistPanel();
        RefreshPlaybackControls();
        RefreshAutomationColumn();
      }

      plHybridArray<const plDocumentObject*, 8> instruments, parameters, automations;
      GatherChildrenOfProperty(GetPropertiesObject(), "Instruments", instruments);
      GatherChildrenOfProperty(GetPropertiesObject(), "Parameters", parameters);
      GatherChildrenOfProperty(GetPropertiesObject(), "Automations", automations);

      plUInt32 uiSamples = 0;
      for (const plDocumentObject* pInstrument : instruments)
      {
        plHybridArray<const plDocumentObject*, 8> playlist;
        GatherChildrenOfProperty(pInstrument, "Playlist", playlist);
        uiSamples += playlist.GetCount();
      }

      if (m_pStatusView != nullptr)
      {
        plStringBuilder sInstruments, sEntries, sParameters, sAutomations;
        sInstruments.SetFormat("{}", instruments.GetCount());
        sEntries.SetFormat("{}", uiSamples);
        sParameters.SetFormat("{}", parameters.GetCount());
        sAutomations.SetFormat("{}", automations.GetCount());

        const plQtAudioStatusRowView::Part parts[] = {{sInstruments, "instruments"}, {sEntries, "playlist entries"}, {sParameters, "parameters"}, {sAutomations, "automations"}};
        m_pStatusView->SetParts(parts);
      }

      if (m_pStatusLabel != nullptr)
      {
        m_pStatusLabel->setText(QString::asprintf("%u instruments \xC2\xB7 %u playlist entries \xC2\xB7 %u parameters \xC2\xB7 %u automations \xC2\xB7 backend: MiniAudio",
          instruments.GetCount(), uiSamples, parameters.GetCount(), automations.GetCount()));
      } });
}

void plAudioEventAssetDocumentWindow::RebuildParameterTabs()
{
  if (m_pTabsLayout == nullptr)
    return;

  while (QLayoutItem* pItem = m_pTabsLayout->takeAt(0))
  {
    if (pItem->widget() != nullptr)
      pItem->widget()->deleteLater();
    delete pItem;
  }

  const plDocumentObject* pSelected = GetDocument()->GetSelectionManager()->GetCurrentObject();
  const bool bParamSelected = (pSelected != nullptr) && pSelected->GetParentProperty().IsEqual("Parameters");

  auto addTab = [&](const QString& sText, bool bActive, const char* szObjectName, std::function<void()> onClick)
  {
    QToolButton* pTab = new QToolButton(m_pTabsHost);
    pTab->setText(sText);
    if (szObjectName != nullptr)
      pTab->setObjectName(szObjectName);
    pTab->setProperty("activeTab", bActive);
    connect(pTab, &QToolButton::clicked, this, [onClick]()
      { onClick(); });
    m_pTabsLayout->addWidget(pTab);
  };

  addTab("Timeline", !bParamSelected, nullptr, [this]()
    {
      if (const plDocumentObject* pProps = GetPropertiesObject())
        GetDocument()->GetSelectionManager()->SetSelection(pProps); });

  plHybridArray<const plDocumentObject*, 8> parameters;
  GatherChildrenOfProperty(GetPropertiesObject(), "Parameters", parameters);

  for (const plDocumentObject* pParameter : parameters)
  {
    plString sName = pParameter->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      sName = "<unnamed>";

    const bool bActive = (pSelected != nullptr) && (pSelected->GetGuid() == pParameter->GetGuid());
    const plUuid guid = pParameter->GetGuid();

    QString sLabel = QString::fromUtf8(sName.GetData());
    if (sName.IsEqual_NoCase("Distance"))
      sLabel += " (built-in)";

    addTab(sLabel, bActive, nullptr, [this, guid]()
      {
        if (const plDocumentObject* pObject = GetDocument()->GetObjectManager()->GetObject(guid))
          GetDocument()->GetSelectionManager()->SetSelection(pObject); });
  }

  addTab("+ Parameter", false, "AddTab", [this]()
    { OnAddParameter(); });

  m_pTabsLayout->addStretch();
}

void plAudioEventAssetDocumentWindow::RebuildInstrumentCombo()
{
  if (m_pInstrumentCombo == nullptr)
    return;

  m_bUpdatingUi = true;

  m_pInstrumentCombo->clear();

  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherChildrenOfProperty(GetPropertiesObject(), "Instruments", instruments);

  const plDocumentObject* pActive = GetActiveInstrument();
  int iActiveIndex = -1;

  for (plUInt32 i = 0; i < instruments.GetCount(); ++i)
  {
    plStringBuilder sName = instruments[i]->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      sName.SetFormat("Instrument {0}", i + 1);

    plStringBuilder sGuid;
    plConversionUtils::ToString(instruments[i]->GetGuid(), sGuid);

    m_pInstrumentCombo->addItem(QString::fromUtf8(sName.GetData()), QVariant(QString::fromUtf8(sGuid.GetData())));

    if (pActive != nullptr && instruments[i]->GetGuid() == pActive->GetGuid())
      iActiveIndex = static_cast<int>(i);
  }

  if (instruments.IsEmpty())
  {
    m_pInstrumentCombo->addItem("<no instruments>");
    m_pInstrumentCombo->setEnabled(false);
  }
  else
  {
    m_pInstrumentCombo->setEnabled(true);
    m_pInstrumentCombo->setCurrentIndex(plMath::Max(iActiveIndex, 0));
  }

  // header: INSTRUMENT — MULTI SOUND "NAME"
  if (m_pInstrumentHeader != nullptr)
  {
    if (pActive != nullptr)
    {
      plHybridArray<const plDocumentObject*, 8> playlist;
      GatherChildrenOfProperty(pActive, "Playlist", playlist);

      plStringBuilder sName = pActive->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
      if (sName.IsEmpty())
        sName = "INSTRUMENT";
      sName.ToUpper();

      plStringBuilder sHeader;
      sHeader.SetFormat("INSTRUMENT \xE2\x80\x94 {0} \"{1}\"", playlist.GetCount() > 1 ? "MULTI SOUND" : "SINGLE SOUND", sName);
      m_pInstrumentHeader->setText(QString::fromUtf8(sHeader.GetData()));
    }
    else
    {
      m_pInstrumentHeader->setText("INSTRUMENT");
    }
  }

  m_bUpdatingUi = false;
}

void plAudioEventAssetDocumentWindow::RebuildPlaylistPanel()
{
  if (m_pPlaylistLayout == nullptr)
    return;

  m_bUpdatingUi = true;

  while (QLayoutItem* pItem = m_pPlaylistLayout->takeAt(0))
  {
    if (pItem->widget() != nullptr)
      pItem->widget()->deleteLater();
    delete pItem;
  }

  const plDocumentObject* pInstrument = GetActiveInstrument();
  const plDocumentObject* pSelected = GetDocument()->GetSelectionManager()->GetCurrentObject();

  if (pInstrument == nullptr)
  {
    QLabel* pHint = new QLabel("No instrument.\n'+ Multi Sound...' builds one from sound files.", m_pPlaylistHost);
    pHint->setStyleSheet("color: #8e8e8e; padding: 12px;");
    m_pPlaylistLayout->addWidget(pHint);
    m_pPlaylistLayout->addStretch();
    m_bUpdatingUi = false;
    return;
  }

  plHybridArray<const plDocumentObject*, 8> playlist;
  GatherChildrenOfProperty(pInstrument, "Playlist", playlist);

  for (plUInt32 i = 0; i < playlist.GetCount(); ++i)
  {
    const plDocumentObject* pEntry = playlist[i];
    const plUuid entryGuid = pEntry->GetGuid();

    QFrame* pRow = new QFrame(m_pPlaylistHost);
    pRow->setObjectName("PlaylistRow");
    pRow->setProperty("rowSelected", pSelected != nullptr && pSelected->GetGuid() == entryGuid);

    QHBoxLayout* pRowLayout = new QHBoxLayout(pRow);
    pRowLayout->setContentsMargins(8, 2, 6, 2);
    pRowLayout->setSpacing(6);

    const plString sClip = pEntry->GetTypeAccessor().GetValue("Clip").ConvertTo<plString>();
    const plString sFile = pEntry->GetTypeAccessor().GetValue("File").ConvertTo<plString>();

    QToolButton* pFile = new QToolButton(pRow);
    pFile->setObjectName("RowFile");
    pFile->setProperty("silence", sClip.IsEmpty() && sFile.IsEmpty());
    pFile->setToolButtonStyle(Qt::ToolButtonTextOnly);
    pFile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    if (!sClip.IsEmpty())
    {
      plAudioClipAssetMetaInfo clipInfo;
      plString sClipName;
      if (plAudioClipAssetUtils::GetClipInfo(sClip, clipInfo, &sClipName))
      {
        pFile->setText(QString::fromUtf8(sClipName.GetData()));
        pFile->setToolTip(QString::fromUtf8(clipInfo.m_sSourceFile.GetData()));
      }
      else
      {
        pFile->setText("missing clip");
        pFile->setToolTip(QString::fromUtf8(sClip.GetData()));
      }
    }
    else if (sFile.IsEmpty())
    {
      pFile->setText("\xE2\x80\x94 silence \xE2\x80\x94");
      pFile->setToolTip("Weighted silence entry - pick a clip via '...'");
    }
    else
    {
      pFile->setText(QString::fromUtf8(plPathUtils::GetFileNameAndExtension(sFile.GetView()).GetStartPointer()));
      pFile->setToolTip(QString::fromUtf8(plStringBuilder(sFile, "\nEmbedded sound file - 'Extract Clips' turns it into an Audio Clip").GetData()));
    }

    connect(pFile, &QToolButton::clicked, this, [this, entryGuid]()
      {
        if (const plDocumentObject* pObject = GetDocument()->GetObjectManager()->GetObject(entryGuid))
          GetDocument()->GetSelectionManager()->SetSelection(pObject); });
    pRowLayout->addWidget(pFile, 1);

    QLabel* pWeightLabel = new QLabel("w", pRow);
    pWeightLabel->setObjectName("RowWeightLabel");
    pRowLayout->addWidget(pWeightLabel);

    QDoubleSpinBox* pWeight = new QDoubleSpinBox(pRow);
    pWeight->setRange(0.0, 100.0);
    pWeight->setSingleStep(0.1);
    pWeight->setDecimals(1);
    pWeight->setValue(pEntry->GetTypeAccessor().GetValue("Weight").ConvertTo<double>());
    pWeight->setToolTip("Random pick weight of this entry");
    pWeight->setKeyboardTracking(false);
    pWeight->setButtonSymbols(QAbstractSpinBox::NoButtons);
    connect(pWeight, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, entryGuid](double fValue)
      {
        if (m_bUpdatingUi)
          return;

        if (const plDocumentObject* pObject = GetDocument()->GetObjectManager()->GetObject(entryGuid))
        {
          plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
          accessor.StartTransaction("Set Weight");
          accessor.SetValueByName(pObject, "Weight", static_cast<float>(fValue)).IgnoreResult();
          accessor.FinishTransaction();
        } });
    pRowLayout->addWidget(pWeight);

    QToolButton* pBrowse = new QToolButton(pRow);
    pBrowse->setObjectName("RowButton");
    pBrowse->setText("...");
    pBrowse->setToolTip("Pick the Audio Clip of this entry");
    connect(pBrowse, &QToolButton::clicked, this, [this, entryGuid, sClip]()
      {
        plString sPicked;
        if (!PickClip(sClip, sPicked))
          return;

        if (const plDocumentObject* pObject = GetDocument()->GetObjectManager()->GetObject(entryGuid))
        {
          plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
          accessor.StartTransaction("Set Playlist Clip");
          accessor.SetValueByName(pObject, "Clip", sPicked).IgnoreResult();
          accessor.SetValueByName(pObject, "File", plString()).IgnoreResult();
          accessor.FinishTransaction();
        } });
    pRowLayout->addWidget(pBrowse);

    QToolButton* pRemove = new QToolButton(pRow);
    pRemove->setObjectName("RowButton");
    pRemove->setText("x");
    pRemove->setToolTip("Remove this entry");
    connect(pRemove, &QToolButton::clicked, this, [this, entryGuid]()
      {
        if (const plDocumentObject* pObject = GetDocument()->GetObjectManager()->GetObject(entryGuid))
        {
          plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
          accessor.StartTransaction("Remove Playlist Entry");
          if (accessor.RemoveObject(pObject).Failed())
          {
            accessor.CancelTransaction();
            return;
          }
          accessor.FinishTransaction();
        } });
    pRowLayout->addWidget(pRemove);

    m_pPlaylistLayout->addWidget(pRow);
  }

  // add row (the mockup's trailing add entry)
  {
    QWidget* pAddRow = new QWidget(m_pPlaylistHost);
    QHBoxLayout* pAddLayout = new QHBoxLayout(pAddRow);
    pAddLayout->setContentsMargins(0, 2, 0, 0);
    pAddLayout->setSpacing(4);

    QToolButton* pAddFiles = new QToolButton(pAddRow);
    pAddFiles->setObjectName("AddRow");
    pAddFiles->setText("+ Add Files...");
    pAddFiles->setToolTip("Appends sound files to this playlist as Audio Clips (the clip next to each file is reused or created)");
    pAddFiles->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    connect(pAddFiles, &QToolButton::clicked, this, &plAudioEventAssetDocumentWindow::OnAddFilesToInstrument);
    pAddLayout->addWidget(pAddFiles, 2);

    QToolButton* pAddClip = new QToolButton(pAddRow);
    pAddClip->setObjectName("AddRow");
    pAddClip->setText("+ Clip...");
    pAddClip->setToolTip("Appends an existing Audio Clip to this playlist");
    pAddClip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    connect(pAddClip, &QToolButton::clicked, this, &plAudioEventAssetDocumentWindow::OnAddClipToInstrument);
    pAddLayout->addWidget(pAddClip, 1);

    QToolButton* pAddEntry = new QToolButton(pAddRow);
    pAddEntry->setObjectName("AddRow");
    pAddEntry->setText("+ Silence");
    pAddEntry->setToolTip("Adds a weighted silence entry");
    pAddEntry->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    connect(pAddEntry, &QToolButton::clicked, this, &plAudioEventAssetDocumentWindow::OnAddPlaylistEntry);
    pAddLayout->addWidget(pAddEntry, 1);

    m_pPlaylistLayout->addWidget(pAddRow);
  }

  m_pPlaylistLayout->addStretch();

  m_bUpdatingUi = false;
}

void plAudioEventAssetDocumentWindow::RefreshPlaybackControls()
{
  const plDocumentObject* pInstrument = GetActiveInstrument();

  const bool bEnabled = pInstrument != nullptr;

  m_bUpdatingUi = true;

  for (QWidget* pWidget : std::initializer_list<QWidget*>{m_pModeCombo, m_pVolumeRandSlider, m_pPitchRandSlider, m_pLoopCheck, m_pStartDelaySpin, m_pMinRetriggerSpin, m_pMaxRetriggerSpin})
  {
    if (pWidget != nullptr)
      pWidget->setEnabled(bEnabled);
  }

  if (pInstrument != nullptr)
  {
    const auto& accessor = pInstrument->GetTypeAccessor();

    if (m_pModeCombo != nullptr)
      m_pModeCombo->setCurrentIndex(static_cast<int>(accessor.GetValue("PlaylistMode").ConvertTo<plInt64>()));

    const double fVolumeRand = accessor.GetValue("VolumeRandomization").ConvertTo<double>();
    if (m_pVolumeRandSlider != nullptr && !m_pVolumeRandSlider->isSliderDown())
      m_pVolumeRandSlider->setValue(static_cast<int>(fVolumeRand * 10.0));
    if (m_pVolumeRandLabel != nullptr)
      m_pVolumeRandLabel->setText(QString::asprintf("\xC2\xB1%.1f dB", fVolumeRand));

    const double fPitchRand = accessor.GetValue("PitchRandomization").ConvertTo<double>();
    if (m_pPitchRandSlider != nullptr && !m_pPitchRandSlider->isSliderDown())
      m_pPitchRandSlider->setValue(static_cast<int>(fPitchRand * 10.0));
    if (m_pPitchRandLabel != nullptr)
      m_pPitchRandLabel->setText(QString::asprintf("\xC2\xB1%.1f st", fPitchRand));

    if (m_pLoopCheck != nullptr)
      m_pLoopCheck->setChecked(accessor.GetValue("Loop").ConvertTo<bool>());

    if (m_pStartDelaySpin != nullptr && !m_pStartDelaySpin->hasFocus())
      m_pStartDelaySpin->setValue(accessor.GetValue("StartDelay").ConvertTo<double>());
    if (m_pMinRetriggerSpin != nullptr && !m_pMinRetriggerSpin->hasFocus())
      m_pMinRetriggerSpin->setValue(accessor.GetValue("MinRetriggerDelay").ConvertTo<double>());
    if (m_pMaxRetriggerSpin != nullptr && !m_pMaxRetriggerSpin->hasFocus())
      m_pMaxRetriggerSpin->setValue(accessor.GetValue("MaxRetriggerDelay").ConvertTo<double>());
  }

  m_bUpdatingUi = false;
}

void plAudioEventAssetDocumentWindow::RefreshAutomationColumn()
{
  const plDocumentObject* pAutomation = GetActiveAutomation();

  if (m_pAutomationHeader != nullptr)
  {
    if (pAutomation != nullptr)
    {
      plStringBuilder sParam = pAutomation->GetTypeAccessor().GetValue("Parameter").ConvertTo<plString>();
      if (sParam.IsEmpty())
        sParam = "?";
      sParam.ToUpper();

      plStringBuilder sHeader;
      sHeader.SetFormat("AUTOMATION ({0})", sParam);
      m_pAutomationHeader->setText(QString::fromUtf8(sHeader.GetData()));
    }
    else
    {
      m_pAutomationHeader->setText("AUTOMATION");
    }
  }

  if (m_pAutomationTargetLabel != nullptr)
  {
    if (pAutomation != nullptr)
    {
      const plInt64 iTarget = pAutomation->GetTypeAccessor().GetValue("Target").ConvertTo<plInt64>();
      m_pAutomationTargetLabel->setText(iTarget == plAudioEventAutomationTarget::EventPitch ? "Event pitch \xC2\xB7 curve" : "Event volume \xC2\xB7 curve");
    }
    else
    {
      m_pAutomationTargetLabel->setText("no automation \xC2\xB7 add one below");
    }
  }

  if (m_pCurvePreview != nullptr)
  {
    m_pCurvePreview->SetAutomation(pAutomation != nullptr ? pAutomation->GetGuid() : plUuid());
  }

  // rebuild the add-target menu from the current parameters
  if (m_pAddAutomationButton != nullptr)
  {
    QMenu* pMenu = new QMenu(m_pAddAutomationButton);

    plHybridArray<const plDocumentObject*, 8> parameters;
    GatherChildrenOfProperty(GetPropertiesObject(), "Parameters", parameters);

    if (parameters.IsEmpty())
    {
      QAction* pHint = pMenu->addAction("add a parameter first ('+ Parameter' tab)");
      pHint->setEnabled(false);
    }
    else
    {
      for (const plDocumentObject* pParameter : parameters)
      {
        const plString sName = pParameter->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
        if (sName.IsEmpty())
          continue;

        QAction* pVolume = pMenu->addAction(QString::asprintf("Volume by %s", sName.GetData()));
        connect(pVolume, &QAction::triggered, this, [this, sName]()
          { OnAddAutomationFor(plAudioEventAutomationTarget::EventVolume, sName); });

        QAction* pPitch = pMenu->addAction(QString::asprintf("Pitch by %s", sName.GetData()));
        connect(pPitch, &QAction::triggered, this, [this, sName]()
          { OnAddAutomationFor(plAudioEventAutomationTarget::EventPitch, sName); });
      }
    }

    QMenu* pOldMenu = m_pAddAutomationButton->menu();
    m_pAddAutomationButton->setMenu(pMenu);
    if (pOldMenu != nullptr)
      pOldMenu->deleteLater();
  }
}

bool plAudioEventAssetDocumentWindow::PickSoundFiles(plHybridArray<plString, 8>& out_files)
{
  out_files.Clear();

  const QStringList files = QFileDialog::getOpenFileNames(this, "Select Sound Files", QString(), "Sound Files (*.wav *.mp3)");

  for (const QString& sFile : files)
  {
    plStringBuilder sPath = sFile.toUtf8().data();
    sPath.MakeCleanPath();

    if (!plQtEditorApp::GetSingleton()->MakePathDataDirectoryRelative(sPath))
    {
      plLog::Warning("Sound file is outside all data directories and may not transform: '{0}'", sPath);
    }

    out_files.PushBack(sPath.GetView());
  }

  return !out_files.IsEmpty();
}

void plAudioEventAssetDocumentWindow::SetInstrumentValue(const char* szProperty, const plVariant& value, const char* szTransaction)
{
  const plDocumentObject* pInstrument = GetActiveInstrument();
  if (pInstrument == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction(szTransaction);
  accessor.SetValueByName(pInstrument, szProperty, value).IgnoreResult();
  accessor.FinishTransaction();
}

void plAudioEventAssetDocumentWindow::OnAddInstrument()
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Instrument");

  plUuid instrumentGuid;
  if (accessor.AddObjectByName(pProps, "Instruments", -1, plGetStaticRTTI<plAudioEventInstrument>(), instrumentGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  // start with one empty playlist entry so the instrument is immediately editable
  const plDocumentObject* pInstrument = GetDocument()->GetObjectManager()->GetObject(instrumentGuid);
  plUuid entryGuid;
  accessor.AddObjectByName(pInstrument, "Playlist", -1, plGetStaticRTTI<plAudioEventPlaylistEntry>(), entryGuid).IgnoreResult();

  accessor.FinishTransaction();

  m_ActiveInstrumentGuid = instrumentGuid;
  GetDocument()->GetSelectionManager()->SetSelection(pInstrument);
}

void plAudioEventAssetDocumentWindow::OnAddFilesToInstrument()
{
  const plDocumentObject* pInstrument = GetActiveInstrument();
  if (pInstrument == nullptr)
  {
    ShowTemporaryStatusBarMsg("Add an instrument first.");
    return;
  }

  plHybridArray<plString, 8> files;
  if (!PickSoundFiles(files))
    return;

  // the clip of each file, found or created before the playlist is edited
  const plUuid instrumentGuid = pInstrument->GetGuid();
  plHybridArray<plString, 8> clips;

  for (const plString& sFile : files)
  {
    plString sClipGuid;
    const plStatus res = plAudioClipAssetUtils::FindOrCreateClip(sFile, sClipGuid);
    if (res.Failed())
    {
      plLog::Error("Could not add '{0}': {1}", sFile, res.m_sMessage);
      continue;
    }

    clips.PushBack(sClipGuid);
  }

  // looked up again: creating and saving the clip documents can show dialogs, during which the playlist may change
  pInstrument = GetDocument()->GetObjectManager()->GetObject(instrumentGuid);
  if (pInstrument == nullptr || clips.IsEmpty())
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Playlist Files");

  for (const plString& sClipGuid : clips)
  {
    plUuid entryGuid;
    if (accessor.AddObjectByName(pInstrument, "Playlist", -1, plGetStaticRTTI<plAudioEventPlaylistEntry>(), entryGuid).Succeeded())
    {
      accessor.SetValueByName(GetDocument()->GetObjectManager()->GetObject(entryGuid), "Clip", sClipGuid).IgnoreResult();
    }
  }

  // unnamed instrument: derive a name from the first file, stripping the extension and
  // trailing digits/separators ("rifle_shot_01.wav" -> "rifle_shot")
  const plString sCurrentName = pInstrument->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
  if (sCurrentName.IsEmpty())
  {
    plStringBuilder sName = plPathUtils::GetFileName(files[0].GetView());
    while (!sName.IsEmpty())
    {
      const char cLast = sName.GetData()[sName.GetElementCount() - 1];
      if ((cLast >= '0' && cLast <= '9') || cLast == '_' || cLast == '-' || cLast == ' ')
      {
        sName.Shrink(0, 1);
      }
      else
      {
        break;
      }
    }

    if (!sName.IsEmpty())
    {
      accessor.SetValueByName(pInstrument, "Name", sName.GetView()).IgnoreResult();
    }
  }

  accessor.FinishTransaction();
}

bool plAudioEventAssetDocumentWindow::PickClip(plStringView sCurrentClip, plString& out_sClipGuid)
{
  plUuid current;
  if (!sCurrentClip.IsEmpty() && plConversionUtils::IsStringUuid(sCurrentClip))
    current = plConversionUtils::ConvertStringToUuid(sCurrentClip);

  plQtAssetBrowserDlg dlg(this, current, "CompatibleAsset_Audio_Clip", "Select Audio Clip");
  if (dlg.exec() == 0 || !dlg.GetSelectedAssetGuid().IsValid())
    return false;

  plStringBuilder sGuid;
  out_sClipGuid = plConversionUtils::ToString(dlg.GetSelectedAssetGuid(), sGuid);
  return true;
}

void plAudioEventAssetDocumentWindow::OnAddClipToInstrument()
{
  const plDocumentObject* pInstrument = GetActiveInstrument();
  if (pInstrument == nullptr)
  {
    ShowTemporaryStatusBarMsg("Add an instrument first.");
    return;
  }

  const plUuid instrumentGuid = pInstrument->GetGuid();

  plString sClipGuid;
  if (!PickClip(plStringView(), sClipGuid))
    return;

  pInstrument = GetDocument()->GetObjectManager()->GetObject(instrumentGuid);
  if (pInstrument == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Playlist Clip");

  plUuid entryGuid;
  if (accessor.AddObjectByName(pInstrument, "Playlist", -1, plGetStaticRTTI<plAudioEventPlaylistEntry>(), entryGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  accessor.SetValueByName(GetDocument()->GetObjectManager()->GetObject(entryGuid), "Clip", sClipGuid).IgnoreResult();
  accessor.FinishTransaction();
}

void plAudioEventAssetDocumentWindow::OnAddPlaylistEntry()
{
  const plDocumentObject* pInstrument = GetActiveInstrument();
  if (pInstrument == nullptr)
  {
    ShowTemporaryStatusBarMsg("Add an instrument first.");
    return;
  }

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Playlist Entry");

  plUuid entryGuid;
  if (accessor.AddObjectByName(pInstrument, "Playlist", -1, plGetStaticRTTI<plAudioEventPlaylistEntry>(), entryGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  accessor.FinishTransaction();
}

void plAudioEventAssetDocumentWindow::OnAddParameter()
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Parameter");

  plUuid parameterGuid;
  if (accessor.AddObjectByName(pProps, "Parameters", -1, plGetStaticRTTI<plAudioEventParameter>(), parameterGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  accessor.FinishTransaction();

  GetDocument()->GetSelectionManager()->SetSelection(GetDocument()->GetObjectManager()->GetObject(parameterGuid));
}

void plAudioEventAssetDocumentWindow::OnAddAutomationFor(plUInt8 uiTarget, const plString& sParameter)
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Automation");

  plUuid automationGuid;
  if (accessor.AddObjectByName(pProps, "Automations", -1, plGetStaticRTTI<plAudioEventAutomation>(), automationGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  const plDocumentObject* pAutomation = GetDocument()->GetObjectManager()->GetObject(automationGuid);
  accessor.SetValueByName(pAutomation, "Parameter", sParameter.GetView()).IgnoreResult();
  accessor.SetValueByName(pAutomation, "Target", static_cast<plInt64>(uiTarget)).IgnoreResult();

  // two default curve points (identity ramp over 0..1)
  plUuid pointGuid;
  if (accessor.AddObjectByName(pAutomation, "Points", -1, plGetStaticRTTI<plAudioEventCurvePoint>(), pointGuid).Succeeded())
  {
    accessor.SetValueByName(GetDocument()->GetObjectManager()->GetObject(pointGuid), "X", 0.0f).IgnoreResult();
    accessor.SetValueByName(GetDocument()->GetObjectManager()->GetObject(pointGuid), "Y", 1.0f).IgnoreResult();
  }
  if (accessor.AddObjectByName(pAutomation, "Points", -1, plGetStaticRTTI<plAudioEventCurvePoint>(), pointGuid).Succeeded())
  {
    accessor.SetValueByName(GetDocument()->GetObjectManager()->GetObject(pointGuid), "X", 1.0f).IgnoreResult();
    accessor.SetValueByName(GetDocument()->GetObjectManager()->GetObject(pointGuid), "Y", 1.0f).IgnoreResult();
  }

  accessor.FinishTransaction();

  GetDocument()->GetSelectionManager()->SetSelection(pAutomation);
}

void plAudioEventAssetDocumentWindow::OnPlayEvent()
{
  plAudioPreviewService& preview = plAudioPreviewService::GetSingleton();

  if (!preview.EnsureInit())
  {
    ShowTemporaryStatusBarMsg("Could not open an audio device for auditioning.");
    return;
  }

  // restart from silence
  preview.StopAll();

  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherChildrenOfProperty(GetPropertiesObject(), "Instruments", instruments);

  if (instruments.IsEmpty())
  {
    ShowTemporaryStatusBarMsg("Nothing to play - add an instrument first.");
    return;
  }

  const bool bAnySolo = HasSoloTracks();

  // route the preview into the event's mixer bus, so the Mixer Console's meters respond
  const plString sOutputBus = GetPropertiesObject() != nullptr ? GetPropertiesObject()->GetTypeAccessor().GetValue("OutputBus").ConvertTo<plString>() : plString();

  plUInt32 uiPlayed = 0;
  plUInt32 uiSkipped = 0;

  for (const plDocumentObject* pInstrument : instruments)
  {
    const auto& accessor = pInstrument->GetTypeAccessor();

    // disabled channels: persisted mute, or silenced because something else is solo'd
    if (accessor.GetValue("Mute").ConvertTo<bool>())
    {
      ++uiSkipped;
      continue;
    }

    if (bAnySolo && !IsSoloTrack(pInstrument->GetGuid()))
    {
      ++uiSkipped;
      continue;
    }

    plHybridArray<const plDocumentObject*, 8> playlist;
    GatherChildrenOfProperty(pInstrument, "Playlist", playlist);

    if (playlist.IsEmpty())
      continue;

    // pick an entry: weighted random (sequential instruments audition their first entry)
    const plInt64 iMode = accessor.GetValue("PlaylistMode").ConvertTo<plInt64>();
    const plDocumentObject* pEntry = playlist[0];

    if (iMode != plAudioEventPlaylistMode::Sequential)
    {
      float fTotalWeight = 0.0f;
      for (const plDocumentObject* pCandidate : playlist)
        fTotalWeight += plMath::Max(pCandidate->GetTypeAccessor().GetValue("Weight").ConvertTo<float>(), 0.0f);

      if (fTotalWeight > 0.0f)
      {
        float fPick = static_cast<float>(m_AuditionRng.DoubleZeroToOneExclusive()) * fTotalWeight;
        for (const plDocumentObject* pCandidate : playlist)
        {
          fPick -= plMath::Max(pCandidate->GetTypeAccessor().GetValue("Weight").ConvertTo<float>(), 0.0f);
          if (fPick <= 0.0f)
          {
            pEntry = pCandidate;
            break;
          }
        }
      }
    }

    // a clip auditions its source file (clip settings such as normalisation are not applied here)
    plString sFile;
    const plString sClip = pEntry->GetTypeAccessor().GetValue("Clip").ConvertTo<plString>();
    if (!sClip.IsEmpty())
    {
      plAudioClipAssetMetaInfo clipInfo;
      if (plAudioClipAssetUtils::GetClipInfo(sClip, clipInfo))
        sFile = clipInfo.m_sSourceFile;
    }
    else
    {
      sFile = pEntry->GetTypeAccessor().GetValue("File").ConvertTo<plString>();
    }

    if (sFile.IsEmpty()) // silence entry, or a clip without a known source file
      continue;

    plStringBuilder sAbsPath = sFile;
    if (!plQtEditorApp::GetSingleton()->MakeDataDirectoryRelativePathAbsolute(sAbsPath))
      continue;

    // randomized volume / pitch, exactly like the runtime instrument does
    const float fVolumeDb = accessor.GetValue("Volume").ConvertTo<float>();
    const float fVolumeRand = accessor.GetValue("VolumeRandomization").ConvertTo<float>();
    const float fPitchSt = accessor.GetValue("Pitch").ConvertTo<float>();
    const float fPitchRand = accessor.GetValue("PitchRandomization").ConvertTo<float>();

    const float fVolume = plAudioMixerUtils::DbToLinear(fVolumeDb + static_cast<float>(m_AuditionRng.DoubleMinMax(-fVolumeRand, fVolumeRand)));
    const float fPitch = plAudioMixerUtils::SemitonesToPitch(fPitchSt + static_cast<float>(m_AuditionRng.DoubleMinMax(-fPitchRand, fPitchRand)));

    const double fDelay = accessor.GetValue("StartDelay").ConvertTo<double>();

    // gapless single-entry loops keep looping; scatterers/multi-entry loops play one iteration here
    const bool bLoop = accessor.GetValue("Loop").ConvertTo<bool>() && playlist.GetCount() == 1 &&
                       accessor.GetValue("MinRetriggerDelay").ConvertTo<double>() <= 0.0 &&
                       accessor.GetValue("MaxRetriggerDelay").ConvertTo<double>() <= 0.0;

    if (preview.PlayFile(sAbsPath.GetData(), fVolume, fPitch, fDelay, bLoop, sOutputBus).Succeeded())
    {
      ++uiPlayed;
    }
  }

  if (uiPlayed == 0)
  {
    ShowTemporaryStatusBarMsg(uiSkipped > 0 ? "All channels are muted or silenced by solo." : "Nothing to play - the playlists have no sound files.");
  }
  else if (uiSkipped > 0)
  {
    plStringBuilder sMsg;
    sMsg.SetFormat("Auditioning {0} channel(s), {1} disabled.", uiPlayed, uiSkipped);
    ShowTemporaryStatusBarMsg(sMsg.GetView());
  }
}

void plAudioEventAssetDocumentWindow::OnStopEvent()
{
  plAudioPreviewService::GetSingleton().StopAll();
}

void plAudioEventAssetDocumentWindow::OnRemoveSelected()
{
  const plDocumentObject* pSelected = GetDocument()->GetSelectionManager()->GetCurrentObject();

  if (pSelected == nullptr || pSelected == GetPropertiesObject() || pSelected->GetParent() == GetDocument()->GetObjectManager()->GetRootObject())
  {
    ShowTemporaryStatusBarMsg("Select an instrument, parameter, automation or playlist entry to remove.");
    return;
  }

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Remove Selected");

  if (accessor.RemoveObject(pSelected).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  accessor.FinishTransaction();

  GetDocument()->GetSelectionManager()->SetSelection(GetPropertiesObject());
}
