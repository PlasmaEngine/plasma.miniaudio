#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/Profiler/AudioProfilerPanel.moc.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Math/ColorScheme.h>
#include <Foundation/Types/VariantType.h>
#include <GuiFoundation/UIServices/UIServices.moc.h>
#include <PlasmaAudioPlugin/Codecs/AudioCodec.h>

#include <QApplication>
#include <QGridLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLineEdit>
#include <QLinearGradient>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>

PL_IMPLEMENT_SINGLETON(plQtAudioProfilerPanel);

namespace
{
  constexpr int kPollIntervalMs = 250;
  constexpr plInt32 kSnapshotHz = 10;
  constexpr plUInt32 kMaxRows = 200;

  /// A request without an answer after this long no longer blocks the next one.
  const plTime kRequestTimeout = plTime::MakeFromSeconds(2.0);

  enum Column
  {
    ColEvent,
    ColEntity,
    ColState,
    ColPrio,
    ColAudibility,
    ColDist,
    ColBus,
    ColCodec,
    ColCount
  };

  enum Detail
  {
    DetEntity,
    DetState,
    DetPriority,
    DetAudibility,
    DetDistance,
    DetBus,
    DetCodec,
    DetAge,
    DetOcclusion,
    DetCount
  };

  const char* s_szDetailNames[DetCount] = {"Entity", "State", "Priority", "Audibility", "Distance", "Bus", "Codec", "Age", "Occlusion"};

  enum MemoryColumn
  {
    MemResource,
    MemType,
    MemLoadMode,
    MemCodec,
    MemResident,
    MemStreams,
    MemShare,
    MemCount
  };

  enum LogColumn
  {
    LogTime,
    LogType,
    LogText,
    LogCount
  };

  /// The log keeps at most this many lines; older ones are dropped.
  constexpr plUInt32 kMaxLogLines = 2000;

  /// Type filter groups of the event log, one pill each.
  enum LogCategory
  {
    LogCatLifecycle,
    LogCatVoices,
    LogCatWarnings,
    LogCatSystem,
    LogCatCount
  };

  const char* s_szLogCategoryNames[LogCatCount] = {"lifecycle", "voices", "warnings", "system"};
  const char* s_szLogCategoryTips[LogCatCount] = {
    "Instance starts, stops and ends",
    "Voices culled at start, virtualised and realised",
    "Stolen instances, rejected starts, killed voices, missing clips, lost devices",
    "Clip load waits, device recovery, mixer, bank and language changes",
  };

  LogCategory GetLogCategory(plUInt8 uiType)
  {
    const auto type = static_cast<plAudioLogEventType::Enum>(uiType);
    if (plAudioLogEventType::IsWarning(type))
      return LogCatWarnings;

    switch (type)
    {
      case plAudioLogEventType::Start:
      case plAudioLogEventType::Stop:
      case plAudioLogEventType::Finished:
        return LogCatLifecycle;
      case plAudioLogEventType::CulledAtStart:
      case plAudioLogEventType::Virtualised:
      case plAudioLogEventType::Realised:
        return LogCatVoices;
      default:
        return LogCatSystem;
    }
  }

  const QColor kRealColor(0x0b, 0xde, 0xb3);
  const QColor kVirtualColor(0xaa, 0xbb, 0xd0);
  const QColor kStoppingColor(0xe8, 0xb0, 0x4a);
  const QColor kOffColor(0xf2, 0x8a, 0xa6);

  QString MiddleDot() { return QString(" ") + QChar(0x00B7) + QString(" "); }

  const char* StateName(plUInt8 uiState)
  {
    switch (uiState)
    {
      case plAudioVoiceSnapshotEntry::Real:
        return "real";
      case plAudioVoiceSnapshotEntry::Virtual:
        return "virtual";
      default:
        return "stopping";
    }
  }

  QColor StateColor(plUInt8 uiState)
  {
    switch (uiState)
    {
      case plAudioVoiceSnapshotEntry::Real:
        return kRealColor;
      case plAudioVoiceSnapshotEntry::Virtual:
        return kVirtualColor;
      default:
        return kStoppingColor;
    }
  }

  QString CodecName(plUInt8 uiCodec)
  {
    switch (uiCodec)
    {
      case plAudioClipCodec::Pcm16:
        return "PCM";
      case plAudioClipCodec::ImaAdpcm:
        return "ADPCM";
      case plAudioClipCodec::Opus:
        return "Opus";
      default:
        return "?";
    }
  }

  QString LoadModeName(plUInt8 uiLoadMode)
  {
    switch (uiLoadMode)
    {
      case plAudioClipLoadMode::DecompressOnLoad:
        return "Decompress on load";
      case plAudioClipLoadMode::CompressedInMemory:
        return "Compressed in memory";
      case plAudioClipLoadMode::Streamed:
        return "Streamed";
      default:
        return "?";
    }
  }

  QString CodecText(const plAudioVoiceSnapshotEntry& voice)
  {
    if (voice.m_uiCodec == plAudioVoiceSnapshotEntry::Embedded)
      return "Embedded";

    QString sCodec = CodecName(voice.m_uiCodec);

    if (voice.m_uiLoadMode == plAudioClipLoadMode::Streamed)
      sCodec += " stream";
    else if (voice.m_uiLoadMode == plAudioClipLoadMode::CompressedInMemory)
      sCodec += " compressed";

    return sCodec;
  }

  /// The event's file name without extension when the name is a path, else the name as it is.
  QString ShortEventName(const plString& sEvent)
  {
    plStringView sView = sEvent.GetView();
    if (sView.FindLastSubString("/") != nullptr || sView.FindLastSubString("\\") != nullptr)
    {
      return plMakeQString(sView.GetFileName());
    }
    return plMakeQString(sView);
  }

  QString EntityText(const plAudioVoiceSnapshotEntry& voice)
  {
    if (voice.m_uiEntityId == 0)
      return "-";
    return QString("#%1").arg(voice.m_uiEntityId);
  }

  QString FormatBytes(plUInt64 uiBytes)
  {
    if (uiBytes >= 1024 * 1024)
      return QString::asprintf("%.1f MB", uiBytes / (1024.0 * 1024.0));
    if (uiBytes >= 1024)
      return QString::asprintf("%.1f KB", uiBytes / 1024.0);
    return QString::asprintf("%u B", static_cast<plUInt32>(uiBytes));
  }

  /// Draws the audibility (Qt::UserRole, clamped to [0, 1]) as a horizontal bar.
  class plQtAudibilityBarDelegate : public QStyledItemDelegate
  {
  public:
    plQtAudibilityBarDelegate(QColor accent, QObject* pParent)
      : QStyledItemDelegate(pParent)
      , m_Accent(accent)
    {
    }

    void paint(QPainter* pPainter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
      QStyleOptionViewItem opt(option);
      initStyleOption(&opt, index);
      opt.text.clear();

      const QWidget* pWidget = opt.widget;
      QStyle* pStyle = pWidget != nullptr ? pWidget->style() : QApplication::style();
      pStyle->drawControl(QStyle::CE_ItemViewItem, &opt, pPainter, pWidget);

      const float fValue = plMath::Clamp(index.data(Qt::UserRole).toFloat(), 0.0f, 1.0f);

      const QRect inner = option.rect.adjusted(8, 0, -8, 0);
      const QRect bar(inner.left(), inner.center().y() - 3, inner.width(), 6);

      pPainter->save();
      pPainter->setRenderHint(QPainter::Antialiasing);
      pPainter->setPen(Qt::NoPen);
      pPainter->setBrush(QColor(0x33, 0x33, 0x33));
      pPainter->drawRoundedRect(bar, 3, 3);

      const int iFill = static_cast<int>(bar.width() * fValue + 0.5f);
      if (iFill > 0)
      {
        pPainter->setBrush(m_Accent);
        pPainter->drawRoundedRect(QRect(bar.left(), bar.top(), iFill, bar.height()), 3, 3);
      }
      pPainter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
      QSize size = QStyledItemDelegate::sizeHint(option, index);
      size.setWidth(plMath::Max(size.width(), 90));
      return size;
    }

  private:
    QColor m_Accent;
  };

  // Theme tokens as in the other audio tools: chrome #1e1e1e, panels #202020, headers #191919, controls #333333,
  // borders #3c3c3c, text #cccccc, dim #8e8e8e.
  QString ProfilerStyle(const QColor& accent)
  {
    return QString(
      "QWidget#AudioProfilerRoot { background: #1e1e1e; }"
      "QWidget#ApTitleRow { background: #202020; border-bottom: 1px solid #303030; }"
      "QWidget#ApTitleRow QLabel { color: #8e8e8e; }"
      "QWidget#ApTitleRow QLabel#ApTitle { color: #cccccc; font-weight: bold; }"
      "QToolButton#ApPause { background: transparent; border: 1px solid #3c3c3c; border-radius: 3px; padding: 2px 10px; color: #cccccc; }"
      "QToolButton#ApPause:hover { border-color: %1; color: #ffffff; }"
      "QToolButton#ApPause:checked { border-color: %1; color: %1; }"
      "QWidget#ApTabRow { background: #202020; border-bottom: 1px solid #303030; }"
      "QTabBar::tab { background: transparent; color: #8e8e8e; padding: 5px 14px; border: 1px solid transparent; border-bottom: none; "
      "border-top-left-radius: 4px; border-top-right-radius: 4px; font-weight: bold; }"
      "QTabBar::tab:selected { color: %1; background: #1a1a1a; border-color: #3c3c3c; }"
      "QTabBar::tab:disabled { color: #555555; }"
      "QWidget#ApToolbar { background: #1e1e1e; border-bottom: 1px solid #303030; }"
      "QWidget#ApToolbar QLabel { color: #8e8e8e; }"
      "QToolButton#ApPill { background: transparent; border: 1px solid #3c3c3c; border-radius: 9px; padding: 1px 10px; color: #6e6e6e; }"
      "QToolButton#ApPill:hover { border-color: %1; }"
      "QToolButton#ApPill:checked { background: #2a2a2a; border-color: %1; color: #ffffff; }"
      "QLineEdit#ApFilter { background: #333333; border: 1px solid #3c3c3c; border-radius: 3px; padding: 2px 6px; color: #cccccc; }"
      "QTableWidget#ApTable { background: #1a1a1a; alternate-background-color: #1d1d1d; border: none; color: #cccccc; gridline-color: #262626; "
      "selection-background-color: rgba(%2, %3, %4, 45); selection-color: #ffffff; }"
      "QTableWidget#ApTable QHeaderView::section { background: #191919; color: #8e8e8e; border: none; border-bottom: 1px solid #303030; padding: 3px 8px; }"
      "QWidget#ApDetails { background: #202020; }"
      "QWidget#ApDetails QLabel { color: #cccccc; }"
      "QWidget#ApDetails QLabel#ApDetailsTitle { color: %1; font-weight: bold; }"
      "QWidget#ApDetails QLabel#ApDetailKey { color: #8e8e8e; }"
      "QWidget#ApDetails QLabel#ApDetailsHint { color: #8e8e8e; }"
      "QWidget#ApStatusRow { background: #191919; border-top: 1px solid #303030; }"
      "QWidget#ApStatusRow QLabel { color: #8e8e8e; }"
      "QToolButton#ApButton { background: transparent; border: 1px solid #3c3c3c; border-radius: 3px; padding: 2px 10px; color: #cccccc; }"
      "QToolButton#ApButton:hover { border-color: %1; color: #ffffff; }"
      "QWidget#ApToolbar QLabel#ApSnapPill { background: #2a2a2a; border: 1px solid %1; border-radius: 9px; padding: 1px 10px; color: #ffffff; }"
      "QWidget#ApToolbar QLabel#ApDim { color: #6e6e6e; }"
      "QScrollArea#ApStripsArea { background: #1a1a1a; border: none; }"
      "QWidget#ApTotalsRow { background: #202020; border-top: 1px solid #303030; }"
      "QWidget#ApTotalsRow QLabel { color: #8e8e8e; }")
      .arg(accent.name())
      .arg(accent.red())
      .arg(accent.green())
      .arg(accent.blue());
  }

  QTableWidgetItem* EnsureItem(QTableWidget* pTable, int iRow, int iColumn)
  {
    QTableWidgetItem* pItem = pTable->item(iRow, iColumn);
    if (pItem == nullptr)
    {
      pItem = new QTableWidgetItem();
      pItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
      pTable->setItem(iRow, iColumn, pItem);
    }
    return pItem;
  }

  /// Level in dBFS of a linear amplitude, floored at -120.
  float ToDbfs(float fLinear)
  {
    return 20.0f * plMath::Log10(plMath::Max(fLinear, 1e-6f));
  }

  QString FormatDbfs(float fLinear)
  {
    if (fLinear < 1e-6f)
      return "-inf";
    return QString::asprintf("%.1f", ToDbfs(fLinear));
  }

  /// Position of a linear level on a meter scale of -60..0 dBFS, as 0..1.
  float MeterFraction(float fLinear)
  {
    return plMath::Clamp((ToDbfs(fLinear) + 60.0f) / 60.0f, 0.0f, 1.0f);
  }
} // namespace

/// Paints one strip per mixer bus: name, a vertical meter (RMS fill and a peak line on a -60..0 dBFS scale), peak and
/// RMS in dBFS and the bus gain. Aux buses have a tinted border and an "aux" tag. Without buses it paints
/// "No mixer loaded".
class plQtAudioBusStripsWidget : public QWidget
{
public:
  plQtAudioBusStripsWidget(const QColor& accent, QWidget* pParent)
    : QWidget(pParent)
    , m_Accent(accent)
  {
  }

  void SetBuses(const plDynamicArray<plAudioBusSnapshotEntry>& buses)
  {
    m_Buses = buses;
    setMinimumSize(sizeHint());
    update();
  }

  QSize sizeHint() const override
  {
    const int iStrips = static_cast<int>(m_Buses.GetCount());
    return QSize(Margin * 2 + plMath::Max(iStrips, 1) * (StripWidth + Gap), Margin * 2 + StripHeight);
  }

protected:
  void paintEvent(QPaintEvent*) override
  {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    if (m_Buses.IsEmpty())
    {
      painter.setPen(QColor(0x8e, 0x8e, 0x8e));
      painter.drawText(rect(), Qt::AlignCenter, "No mixer loaded");
      return;
    }

    const QColor amber(0xe8, 0xb0, 0x4a);
    const QColor red(0xf2, 0x5a, 0x5a);
    const QColor aux(0xaa, 0xbb, 0xd0);

    QFont smallFont = font();
    smallFont.setPointSizeF(smallFont.pointSizeF() * 0.9);

    for (plUInt32 i = 0; i < m_Buses.GetCount(); ++i)
    {
      const plAudioBusSnapshotEntry& bus = m_Buses[i];
      const QRect strip = StripRect(static_cast<int>(i));

      painter.setPen(QPen(bus.m_bIsAux ? aux.darker(160) : QColor(0x3c, 0x3c, 0x3c), 1));
      painter.setBrush(QColor(0x20, 0x20, 0x20));
      painter.drawRoundedRect(QRectF(strip).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);

      // name
      painter.setFont(font());
      painter.setPen(QColor(0xcc, 0xcc, 0xcc));
      const QRect nameRect(strip.left() + 6, strip.top() + 8, strip.width() - 12, 16);
      painter.drawText(nameRect, Qt::AlignCenter, painter.fontMetrics().elidedText(plMakeQString(bus.m_sName), Qt::ElideRight, nameRect.width()));

      // meter: background, RMS fill, peak line
      const QRect meter(strip.center().x() - 7, nameRect.bottom() + 7, 14, MeterHeight);
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(0x33, 0x33, 0x33));
      painter.drawRoundedRect(meter, 2, 2);

      const int iRms = static_cast<int>(MeterFraction(bus.m_fRms) * meter.height() + 0.5f);
      if (iRms > 0)
      {
        QLinearGradient gradient(meter.bottomLeft(), meter.topLeft());
        gradient.setColorAt(0.0, m_Accent);
        gradient.setColorAt(0.7, m_Accent);
        gradient.setColorAt(0.88, amber);
        gradient.setColorAt(1.0, red);
        painter.setBrush(gradient);
        painter.drawRect(QRect(meter.left(), meter.bottom() + 1 - iRms, meter.width(), iRms));
      }

      if (bus.m_fPeak > 0.0f)
      {
        const int iPeakY = meter.bottom() + 1 - static_cast<int>(MeterFraction(bus.m_fPeak) * meter.height() + 0.5f);
        painter.setPen(QPen(bus.m_fPeak >= 1.0f ? red : QColor(0xee, 0xee, 0xee), 2));
        painter.drawLine(meter.left() - 3, iPeakY, meter.right() + 3, iPeakY);
      }

      // readouts
      painter.setFont(smallFont);
      int iY = meter.bottom() + 6;

      painter.setPen(QColor(0xcc, 0xcc, 0xcc));
      painter.drawText(QRect(strip.left(), iY, strip.width(), 14), Qt::AlignCenter, FormatDbfs(bus.m_fPeak) + " dBFS");
      iY += 14;

      painter.setPen(QColor(0x8e, 0x8e, 0x8e));
      painter.drawText(QRect(strip.left(), iY, strip.width(), 14), Qt::AlignCenter, "RMS " + FormatDbfs(bus.m_fRms));
      iY += 14;

      painter.drawText(QRect(strip.left(), iY, strip.width(), 14), Qt::AlignCenter, QString::asprintf("gain %+.1f dB", bus.m_fGainDb));
      iY += 14;

      if (bus.m_bIsAux)
      {
        painter.setPen(aux);
        painter.drawText(QRect(strip.left(), iY, strip.width(), 14), Qt::AlignCenter, "aux");
      }
    }
  }

  bool event(QEvent* pEvent) override
  {
    if (pEvent->type() == QEvent::ToolTip)
    {
      const QHelpEvent* pHelp = static_cast<const QHelpEvent*>(pEvent);

      for (plUInt32 i = 0; i < m_Buses.GetCount(); ++i)
      {
        if (!StripRect(static_cast<int>(i)).contains(pHelp->pos()))
          continue;

        const plAudioBusSnapshotEntry& bus = m_Buses[i];
        QString sTip = QString("<b>%1</b>%2<br>Peak %3 dBFS<br>RMS %4 dBFS<br>Gain %5 dB")
                         .arg(plMakeQString(bus.m_sName).toHtmlEscaped())
                         .arg(bus.m_bIsAux ? QString(" (aux)") : QString())
                         .arg(FormatDbfs(bus.m_fPeak))
                         .arg(FormatDbfs(bus.m_fRms))
                         .arg(QString::asprintf("%+.1f", bus.m_fGainDb));

        if (bus.m_uiParent < m_Buses.GetCount())
        {
          sTip += QString("<br>Parent %1").arg(plMakeQString(m_Buses[bus.m_uiParent].m_sName).toHtmlEscaped());
        }

        QToolTip::showText(pHelp->globalPos(), sTip, this);
        return true;
      }

      QToolTip::hideText();
      pEvent->ignore();
      return true;
    }

    return QWidget::event(pEvent);
  }

private:
  static constexpr int Margin = 14;
  static constexpr int StripWidth = 86;
  static constexpr int Gap = 10;
  static constexpr int MeterHeight = 150;
  static constexpr int StripHeight = 8 + 16 + 7 + MeterHeight + 6 + 4 * 14 + 8;

  QRect StripRect(int iStrip) const { return QRect(Margin + iStrip * (StripWidth + Gap), Margin, StripWidth, StripHeight); }

  QColor m_Accent;
  plDynamicArray<plAudioBusSnapshotEntry> m_Buses;
};

plQtAudioProfilerPanel::plQtAudioProfilerPanel()
  : plQtApplicationPanel("AudioProfilerPanel")
  , m_SingletonRegistrar(this)
{
  setWindowTitle("Audio Profiler");
  setIcon(plQtUiServices::GetCachedIconResource(":/AssetIcons/AudioMixer.svg"));

  m_Accent = plToQtColor(plColorGammaUB(plColorScheme::GetCategoryColor("Audio", plColorScheme::CategoryColorUsage::BorderColor)));

  if (plAudioEngineUi::UseEngineViews())
  {
    m_pView = new plQtAudioProfilerView(this);
    m_pView->m_OnPauseToggled = [this](bool bPaused)
    { OnPauseToggled(bPaused); };
    setWidget(m_pView, ads::CDockWidget::ForceNoScrollArea);
  }
  else
  {
    CreateQtContent();
  }

  m_pTimer = new QTimer(this);
  m_pTimer->setInterval(kPollIntervalMs);
  connect(m_pTimer, &QTimer::timeout, this, &plQtAudioProfilerPanel::OnTick);
  m_pTimer->start();

  connect(this, &ads::CDockWidget::viewToggled, this, [this](bool) { OnTick(); });

  plEditorEngineProcessConnection::s_Events.AddEventHandler(plMakeDelegate(&plQtAudioProfilerPanel::EngineProcessMsgHandler, this));

  UpdateLinkIndicator();
  UpdateStatusLine();
}

void plQtAudioProfilerPanel::CreateQtContent()
{
  QWidget* pRoot = new QWidget(this);
  pRoot->setObjectName("AudioProfilerRoot");
  pRoot->setStyleSheet(ProfilerStyle(m_Accent));

  QVBoxLayout* pRootLayout = new QVBoxLayout(pRoot);
  pRootLayout->setContentsMargins(0, 0, 0, 0);
  pRootLayout->setSpacing(0);

  // title row: connection dot, title, link text, Pause
  {
    QWidget* pRow = new QWidget(pRoot);
    pRow->setObjectName("ApTitleRow");
    pRow->setAttribute(Qt::WA_StyledBackground);

    QHBoxLayout* pLayout = new QHBoxLayout(pRow);
    pLayout->setContentsMargins(10, 5, 8, 5);
    pLayout->setSpacing(10);

    m_pLinkDot = new QLabel(pRow);
    m_pLinkDot->setFixedSize(8, 8);
    pLayout->addWidget(m_pLinkDot);

    QLabel* pTitle = new QLabel("Audio Profiler", pRow);
    pTitle->setObjectName("ApTitle");
    pLayout->addWidget(pTitle);

    m_pLinkText = new QLabel(pRow);
    pLayout->addWidget(m_pLinkText);

    pLayout->addStretch(1);

    m_pPauseButton = new QToolButton(pRow);
    m_pPauseButton->setObjectName("ApPause");
    m_pPauseButton->setText("Pause");
    m_pPauseButton->setCheckable(true);
    m_pPauseButton->setToolTip("Freeze the view and stop polling the engine process");
    connect(m_pPauseButton, &QToolButton::toggled, this, &plQtAudioProfilerPanel::OnPauseToggled);
    pLayout->addWidget(m_pPauseButton);

    pRootLayout->addWidget(pRow);
  }

  // tab bar; the tab index is the page index of m_pPages
  {
    QWidget* pRow = new QWidget(pRoot);
    pRow->setObjectName("ApTabRow");
    pRow->setAttribute(Qt::WA_StyledBackground);

    QHBoxLayout* pLayout = new QHBoxLayout(pRow);
    pLayout->setContentsMargins(10, 6, 10, 0);
    pLayout->setSpacing(0);

    m_pTabs = new QTabBar(pRow);
    m_pTabs->setDrawBase(false);
    m_pTabs->setExpanding(false);

    for (const char* szTab : {"Voices", "Buses", "Memory", "Event log"})
    {
      m_pTabs->addTab(szTab);
    }

    pLayout->addWidget(m_pTabs);
    pLayout->addStretch(1);

    pRootLayout->addWidget(pRow);
  }

  m_pPages = new QStackedWidget(pRoot);

  // Voices page: voice table and details of the selected voice
  QSplitter* pSplitter = new QSplitter(Qt::Horizontal, m_pPages);
  pSplitter->setChildrenCollapsible(false);
  pSplitter->setHandleWidth(1);

  // voice list: toolbar + table
  {
    QWidget* pLeft = new QWidget(pSplitter);

    QVBoxLayout* pLeftLayout = new QVBoxLayout(pLeft);
    pLeftLayout->setContentsMargins(0, 0, 0, 0);
    pLeftLayout->setSpacing(0);

    QWidget* pToolbar = new QWidget(pLeft);
    pToolbar->setObjectName("ApToolbar");
    pToolbar->setAttribute(Qt::WA_StyledBackground);

    QHBoxLayout* pToolbarLayout = new QHBoxLayout(pToolbar);
    pToolbarLayout->setContentsMargins(10, 6, 10, 6);
    pToolbarLayout->setSpacing(6);

    pToolbarLayout->addWidget(new QLabel("State", pToolbar));

    for (plUInt8 uiState = 0; uiState < 3; ++uiState)
    {
      QToolButton* pPill = new QToolButton(pToolbar);
      pPill->setObjectName("ApPill");
      pPill->setText(StateName(uiState));
      pPill->setCheckable(true);
      pPill->setChecked(true);
      pPill->setToolTip(QString("Show %1 voices").arg(StateName(uiState)));
      connect(pPill, &QToolButton::toggled, this, &plQtAudioProfilerPanel::OnFilterChanged);
      pToolbarLayout->addWidget(pPill);
      m_pStateFilter[uiState] = pPill;
    }

    pToolbarLayout->addStretch(1);

    m_pEventFilter = new QLineEdit(pToolbar);
    m_pEventFilter->setObjectName("ApFilter");
    m_pEventFilter->setPlaceholderText("filter event...");
    m_pEventFilter->setClearButtonEnabled(true);
    m_pEventFilter->setFixedWidth(180);
    connect(m_pEventFilter, &QLineEdit::textChanged, this, &plQtAudioProfilerPanel::OnFilterChanged);
    pToolbarLayout->addWidget(m_pEventFilter);

    pLeftLayout->addWidget(pToolbar);

    m_pTable = new QTableWidget(0, ColCount, pLeft);
    m_pTable->setObjectName("ApTable");
    m_pTable->setHorizontalHeaderLabels({"Event", "Entity", "State", "Prio", "Audibility", "Dist", "Bus", "Codec"});
    m_pTable->verticalHeader()->setVisible(false);
    m_pTable->verticalHeader()->setDefaultSectionSize(22);
    m_pTable->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_pTable->horizontalHeader()->setHighlightSections(false);
    m_pTable->horizontalHeader()->setSectionResizeMode(ColEvent, QHeaderView::Stretch);
    m_pTable->setColumnWidth(ColEntity, 80);
    m_pTable->setColumnWidth(ColState, 70);
    m_pTable->setColumnWidth(ColPrio, 50);
    m_pTable->setColumnWidth(ColAudibility, 110);
    m_pTable->setColumnWidth(ColDist, 70);
    m_pTable->setColumnWidth(ColBus, 110);
    m_pTable->setColumnWidth(ColCodec, 110);
    m_pTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTable->setAlternatingRowColors(true);
    m_pTable->setShowGrid(false);
    m_pTable->setWordWrap(false);
    m_pTable->setItemDelegateForColumn(ColAudibility, new plQtAudibilityBarDelegate(m_Accent, m_pTable));
    connect(m_pTable, &QTableWidget::itemSelectionChanged, this, &plQtAudioProfilerPanel::OnSelectionChanged);

    pLeftLayout->addWidget(m_pTable, 1);
  }

  // details of the selected voice
  {
    QWidget* pDetails = new QWidget(pSplitter);
    pDetails->setObjectName("ApDetails");
    pDetails->setAttribute(Qt::WA_StyledBackground);
    pDetails->setMinimumWidth(240);

    QVBoxLayout* pLayout = new QVBoxLayout(pDetails);
    pLayout->setContentsMargins(12, 10, 12, 10);
    pLayout->setSpacing(8);

    m_pDetailsTitle = new QLabel("VOICE", pDetails);
    m_pDetailsTitle->setObjectName("ApDetailsTitle");
    m_pDetailsTitle->setWordWrap(true);
    pLayout->addWidget(m_pDetailsTitle);

    m_pDetailsHint = new QLabel("Select a voice.", pDetails);
    m_pDetailsHint->setObjectName("ApDetailsHint");
    pLayout->addWidget(m_pDetailsHint);

    m_pDetailsGrid = new QWidget(pDetails);
    QGridLayout* pGrid = new QGridLayout(m_pDetailsGrid);
    pGrid->setContentsMargins(0, 0, 0, 0);
    pGrid->setHorizontalSpacing(12);
    pGrid->setVerticalSpacing(4);
    pGrid->setColumnStretch(1, 1);

    for (int i = 0; i < DetCount; ++i)
    {
      QLabel* pKey = new QLabel(s_szDetailNames[i], m_pDetailsGrid);
      pKey->setObjectName("ApDetailKey");
      pGrid->addWidget(pKey, i, 0);

      m_pDetailValues[i] = new QLabel(m_pDetailsGrid);
      m_pDetailValues[i]->setTextInteractionFlags(Qt::TextSelectableByMouse);
      pGrid->addWidget(m_pDetailValues[i], i, 1);
    }

    m_pDetailsGrid->setVisible(false);
    pLayout->addWidget(m_pDetailsGrid);
    pLayout->addStretch(1);
  }

  pSplitter->setStretchFactor(0, 1);
  pSplitter->setStretchFactor(1, 0);
  pSplitter->setSizes({700, 260});
  m_pPages->addWidget(pSplitter);
  m_pPages->addWidget(CreateBusesPage(m_pPages));
  m_pPages->addWidget(CreateMemoryPage(m_pPages));
  m_pPages->addWidget(CreateLogPage(m_pPages));
  pRootLayout->addWidget(m_pPages, 1);

  connect(m_pTabs, &QTabBar::currentChanged, this, &plQtAudioProfilerPanel::OnTabChanged);

  // status line
  {
    QWidget* pRow = new QWidget(pRoot);
    pRow->setObjectName("ApStatusRow");
    pRow->setAttribute(Qt::WA_StyledBackground);

    QHBoxLayout* pLayout = new QHBoxLayout(pRow);
    pLayout->setContentsMargins(10, 4, 10, 4);

    m_pStatusLine = new QLabel(pRow);
    pLayout->addWidget(m_pStatusLine, 1);

    pRootLayout->addWidget(pRow);
  }

  setWidget(pRoot);
}

plQtAudioProfilerPanel::~plQtAudioProfilerPanel()
{
  plEditorEngineProcessConnection::s_Events.RemoveEventHandler(plMakeDelegate(&plQtAudioProfilerPanel::EngineProcessMsgHandler, this));

  if (m_bSnapshotsEnabled)
  {
    SendSnapshotRate(0);
  }
}

plEditorEngineProcessConnection* plQtAudioProfilerPanel::GetLiveConnection()
{
  plEditorEngineProcessConnection* pConnection = plEditorEngineProcessConnection::GetSingleton();
  if (pConnection == nullptr || pConnection->IsProcessCrashed() || !pConnection->IsEngineSetup())
    return nullptr;

  return pConnection;
}

void plQtAudioProfilerPanel::SendSnapshotRate(plInt32 iHz)
{
  plEditorEngineProcessConnection* pConnection = GetLiveConnection();
  if (pConnection == nullptr)
    return;

  plChangeCVarMsgToEngine msg;
  msg.m_sCVarName = "PlasmaAudio.ProfilerSnapshotHz";
  msg.m_NewValue = iHz;
  pConnection->SendMessage(&msg);
}

void plQtAudioProfilerPanel::OnTick()
{
  plEditorEngineProcessConnection* pConnection = GetLiveConnection();

  // the engine process produces snapshots only while the panel is open
  const bool bWantSnapshots = pConnection != nullptr && !isClosed();
  if (bWantSnapshots != m_bSnapshotsEnabled)
  {
    SendSnapshotRate(bWantSnapshots ? kSnapshotHz : 0);
    m_bSnapshotsEnabled = bWantSnapshots;
  }

  if (pConnection == nullptr)
  {
    m_bRequestPending = false;

    if (m_LinkState != LinkState::NoEngine)
    {
      m_LinkState = LinkState::NoEngine;
      UpdateLinkIndicator();
    }
    return;
  }

  if (m_bPaused || isClosed() || !isVisible())
    return;

  if (m_bRequestPending && plTime::Now() - m_LastRequest < kRequestTimeout)
    return;

  plStatsSnapshotMsgToEngine msg;
  msg.m_bIncludeStats = true;
  msg.m_bIncludeMemory = false;

  if (pConnection->SendMessage(&msg))
  {
    m_bRequestPending = true;
    m_LastRequest = plTime::Now();
  }
}

void plQtAudioProfilerPanel::EngineProcessMsgHandler(const plEditorEngineProcessConnection::Event& e)
{
  switch (e.m_Type)
  {
    case plEditorEngineProcessConnection::Event::Type::ProcessMessage:
    {
      if (const auto* pMsg = plDynamicCast<const plStatsSnapshotResultMsgToEditor*>(e.m_pMsg))
      {
        m_bRequestPending = false;

        if (!m_bPaused && pMsg->m_Stats.IsValid())
        {
          ApplyStats(pMsg->m_Stats);
        }
      }
    }
    break;

    case plEditorEngineProcessConnection::Event::Type::ProcessStarted:
    case plEditorEngineProcessConnection::Event::Type::ProcessRestarted:
    case plEditorEngineProcessConnection::Event::Type::ProcessShutdown:
    case plEditorEngineProcessConnection::Event::Type::ProcessCrashed:
    {
      // a new engine process starts with snapshots off; the next tick enables them again
      m_bSnapshotsEnabled = false;
      m_bRequestPending = false;

      // the event log of a new engine process counts from 0 again
      m_bLogSequenceKnown = false;
      m_LinkState = LinkState::NoEngine;
      UpdateLinkIndicator();
    }
    break;

    default:
      break;
  }
}

void plQtAudioProfilerPanel::ApplyStats(const plVariant& stats)
{
  LinkState newState = LinkState::NoData;

  if (stats.IsA<plVariantDictionary>())
  {
    const plVariantDictionary& dict = stats.Get<plVariantDictionary>();

    const plVariant* pValue = nullptr;
    if (dict.TryGetValue("PlasmaAudio/VoiceSnapshot", pValue) && pValue->IsA<plDataBuffer>())
    {
      const plDataBuffer& buffer = pValue->Get<plDataBuffer>();
      plRawMemoryStreamReader reader(buffer);

      if (m_Snapshot.Read(reader).Succeeded())
      {
        m_bHasSnapshot = true;
        newState = LinkState::Live;
      }
    }
  }

  if (newState != m_LinkState)
  {
    m_LinkState = newState;
    UpdateLinkIndicator();
  }

  if (newState == LinkState::Live && m_pView != nullptr)
  {
    plHybridArray<LogLine, 32> newLines;
    CollectNewLogLines(newLines);
    m_pView->SetSnapshot(m_Snapshot, newLines);
    return;
  }

  if (newState == LinkState::Live)
  {
    RebuildTable();
    AppendLogEntries();

    // the buses and memory tabs are rebuilt when they are shown
    switch (m_pTabs->currentIndex())
    {
      case 1:
        RebuildBuses();
        break;
      case 2:
        RebuildMemory();
        break;
      default:
        break;
    }

    UpdateTabTexts();
    UpdateStatusLine();
  }
}

QWidget* plQtAudioProfilerPanel::CreateBusesPage(QWidget* pParent)
{
  QWidget* pPage = new QWidget(pParent);

  QVBoxLayout* pLayout = new QVBoxLayout(pPage);
  pLayout->setContentsMargins(0, 0, 0, 0);
  pLayout->setSpacing(0);

  // toolbar: active snapshot pills, master level
  {
    QWidget* pToolbar = new QWidget(pPage);
    pToolbar->setObjectName("ApToolbar");
    pToolbar->setAttribute(Qt::WA_StyledBackground);

    QHBoxLayout* pToolbarLayout = new QHBoxLayout(pToolbar);
    pToolbarLayout->setContentsMargins(10, 6, 10, 6);
    pToolbarLayout->setSpacing(6);

    pToolbarLayout->addWidget(new QLabel("Snapshots", pToolbar));

    m_pSnapshotPills = new QHBoxLayout();
    m_pSnapshotPills->setContentsMargins(0, 0, 0, 0);
    m_pSnapshotPills->setSpacing(6);
    pToolbarLayout->addLayout(m_pSnapshotPills);

    m_pNoSnapshots = new QLabel("none active", pToolbar);
    m_pNoSnapshots->setObjectName("ApDim");
    pToolbarLayout->addWidget(m_pNoSnapshots);

    pToolbarLayout->addStretch(1);

    m_pMasterLevel = new QLabel(pToolbar);
    m_pMasterLevel->setTextFormat(Qt::RichText);
    pToolbarLayout->addWidget(m_pMasterLevel);

    pLayout->addWidget(pToolbar);
  }

  QScrollArea* pScroll = new QScrollArea(pPage);
  pScroll->setObjectName("ApStripsArea");
  pScroll->setWidgetResizable(true);
  pScroll->setFrameShape(QFrame::NoFrame);

  m_pStrips = new plQtAudioBusStripsWidget(m_Accent, pScroll);
  pScroll->setWidget(m_pStrips);
  pLayout->addWidget(pScroll, 1);

  return pPage;
}

QWidget* plQtAudioProfilerPanel::CreateMemoryPage(QWidget* pParent)
{
  QWidget* pPage = new QWidget(pParent);

  QVBoxLayout* pLayout = new QVBoxLayout(pPage);
  pLayout->setContentsMargins(0, 0, 0, 0);
  pLayout->setSpacing(0);

  m_pMemoryTable = new QTableWidget(0, MemCount, pPage);
  m_pMemoryTable->setObjectName("ApTable");
  m_pMemoryTable->setHorizontalHeaderLabels({"Resource", "Type", "Load mode", "Codec", "Resident", "Streams", "Share"});
  m_pMemoryTable->verticalHeader()->setVisible(false);
  m_pMemoryTable->verticalHeader()->setDefaultSectionSize(22);
  m_pMemoryTable->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  m_pMemoryTable->horizontalHeader()->setHighlightSections(false);
  m_pMemoryTable->horizontalHeader()->setSectionResizeMode(MemResource, QHeaderView::Stretch);
  m_pMemoryTable->setColumnWidth(MemType, 60);
  m_pMemoryTable->setColumnWidth(MemLoadMode, 140);
  m_pMemoryTable->setColumnWidth(MemCodec, 120);
  m_pMemoryTable->setColumnWidth(MemResident, 90);
  m_pMemoryTable->setColumnWidth(MemStreams, 60);
  m_pMemoryTable->setColumnWidth(MemShare, 120);
  m_pMemoryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_pMemoryTable->setSelectionMode(QAbstractItemView::SingleSelection);
  m_pMemoryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_pMemoryTable->setAlternatingRowColors(true);
  m_pMemoryTable->setShowGrid(false);
  m_pMemoryTable->setWordWrap(false);
  m_pMemoryTable->setItemDelegateForColumn(MemShare, new plQtAudibilityBarDelegate(m_Accent, m_pMemoryTable));
  pLayout->addWidget(m_pMemoryTable, 1);

  QWidget* pTotals = new QWidget(pPage);
  pTotals->setObjectName("ApTotalsRow");
  pTotals->setAttribute(Qt::WA_StyledBackground);

  QHBoxLayout* pTotalsLayout = new QHBoxLayout(pTotals);
  pTotalsLayout->setContentsMargins(10, 4, 10, 4);

  m_pMemoryTotals = new QLabel(pTotals);
  pTotalsLayout->addWidget(m_pMemoryTotals, 1);

  pLayout->addWidget(pTotals);

  return pPage;
}

QWidget* plQtAudioProfilerPanel::CreateLogPage(QWidget* pParent)
{
  QWidget* pPage = new QWidget(pParent);

  QVBoxLayout* pLayout = new QVBoxLayout(pPage);
  pLayout->setContentsMargins(0, 0, 0, 0);
  pLayout->setSpacing(0);

  // toolbar: type pills, text filter, Clear
  {
    QWidget* pToolbar = new QWidget(pPage);
    pToolbar->setObjectName("ApToolbar");
    pToolbar->setAttribute(Qt::WA_StyledBackground);

    QHBoxLayout* pToolbarLayout = new QHBoxLayout(pToolbar);
    pToolbarLayout->setContentsMargins(10, 6, 10, 6);
    pToolbarLayout->setSpacing(6);

    pToolbarLayout->addWidget(new QLabel("Type", pToolbar));

    for (plUInt32 i = 0; i < LogCatCount; ++i)
    {
      QToolButton* pPill = new QToolButton(pToolbar);
      pPill->setObjectName("ApPill");
      pPill->setText(s_szLogCategoryNames[i]);
      pPill->setCheckable(true);
      pPill->setChecked(true);
      pPill->setToolTip(s_szLogCategoryTips[i]);
      connect(pPill, &QToolButton::toggled, this, &plQtAudioProfilerPanel::OnLogFilterChanged);
      pToolbarLayout->addWidget(pPill);
      m_pLogCategory[i] = pPill;
    }

    pToolbarLayout->addStretch(1);

    m_pLogFilter = new QLineEdit(pToolbar);
    m_pLogFilter->setObjectName("ApFilter");
    m_pLogFilter->setPlaceholderText("filter text...");
    m_pLogFilter->setClearButtonEnabled(true);
    m_pLogFilter->setFixedWidth(180);
    connect(m_pLogFilter, &QLineEdit::textChanged, this, &plQtAudioProfilerPanel::OnLogFilterChanged);
    pToolbarLayout->addWidget(m_pLogFilter);

    QToolButton* pClear = new QToolButton(pToolbar);
    pClear->setObjectName("ApButton");
    pClear->setText("Clear");
    pClear->setToolTip("Remove all lines from the log");
    connect(pClear, &QToolButton::clicked, this, &plQtAudioProfilerPanel::OnLogClear);
    pToolbarLayout->addWidget(pClear);

    pLayout->addWidget(pToolbar);
  }

  m_pLogTable = new QTableWidget(0, LogCount, pPage);
  m_pLogTable->setObjectName("ApTable");
  m_pLogTable->setHorizontalHeaderLabels({"Time", "Type", "Event"});
  m_pLogTable->verticalHeader()->setVisible(false);
  m_pLogTable->verticalHeader()->setDefaultSectionSize(20);
  m_pLogTable->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  m_pLogTable->horizontalHeader()->setHighlightSections(false);
  m_pLogTable->horizontalHeader()->setSectionResizeMode(LogText, QHeaderView::Stretch);
  m_pLogTable->setColumnWidth(LogTime, 80);
  m_pLogTable->setColumnWidth(LogType, 100);
  m_pLogTable->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_pLogTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_pLogTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_pLogTable->setShowGrid(false);
  m_pLogTable->setWordWrap(false);

  QFont monoFont("Consolas");
  monoFont.setStyleHint(QFont::Monospace);
  m_pLogTable->setFont(monoFont);

  pLayout->addWidget(m_pLogTable, 1);

  return pPage;
}

void plQtAudioProfilerPanel::RebuildBuses()
{
  const plAudioProfilingSnapshot& s = m_Snapshot;

  m_pStrips->SetBuses(s.m_Buses);

  // snapshot pills, rebuilt only when the active snapshots or their rounded intensities change
  QString sKey;
  for (const plAudioActiveSnapshotEntry& active : s.m_ActiveSnapshots)
  {
    sKey += plMakeQString(active.m_sName) + QString::asprintf(" %.2f;", active.m_fIntensity);
  }

  if (sKey != m_sSnapshotPillsKey)
  {
    m_sSnapshotPillsKey = sKey;

    while (QLayoutItem* pItem = m_pSnapshotPills->takeAt(0))
    {
      delete pItem->widget();
      delete pItem;
    }

    for (const plAudioActiveSnapshotEntry& active : s.m_ActiveSnapshots)
    {
      QLabel* pPill = new QLabel(QString("%1 (%2)").arg(plMakeQString(active.m_sName)).arg(QString::asprintf("%.2f", active.m_fIntensity)), m_pNoSnapshots->parentWidget());
      pPill->setObjectName("ApSnapPill");
      pPill->setToolTip(QString::asprintf("Intensity %.2f", active.m_fIntensity));
      m_pSnapshotPills->addWidget(pPill);
    }
  }

  m_pNoSnapshots->setVisible(s.m_ActiveSnapshots.IsEmpty());
  m_pNoSnapshots->setText(s.m_Buses.IsEmpty() ? QString("-") : QString("none active"));

  // the master bus is the one without a parent
  const plAudioBusSnapshotEntry* pMaster = nullptr;
  for (const plAudioBusSnapshotEntry& bus : s.m_Buses)
  {
    if (bus.m_uiParent == plAudioBusSnapshotEntry::NoParent)
    {
      pMaster = &bus;
      break;
    }
  }

  if (pMaster == nullptr)
  {
    m_pMasterLevel->setText("No mixer loaded");
    m_pMasterLevel->setToolTip(QString());
    return;
  }

  m_pMasterLevel->setText(QString("%1 peak <b style=\"color:#cccccc\">%2 dBFS</b>%3RMS <b style=\"color:#cccccc\">%4 dBFS</b>")
                            .arg(plMakeQString(pMaster->m_sName).toHtmlEscaped())
                            .arg(FormatDbfs(pMaster->m_fPeak))
                            .arg(MiddleDot())
                            .arg(FormatDbfs(pMaster->m_fRms)));
  m_pMasterLevel->setToolTip(QString("Mixer: %1").arg(plMakeQString(s.m_sMixer)));
}

void plQtAudioProfilerPanel::RebuildMemory()
{
  const plAudioProfilingSnapshot& s = m_Snapshot;

  struct Row
  {
    QString m_sName;
    QString m_sTooltip;
    QString m_sType;
    QString m_sLoadMode;
    QString m_sCodec;
    QString m_sStreams;
    plUInt64 m_uiResident = 0;
    bool m_bHasResident = true;
    QColor m_TextColor;
  };

  plUInt64 uiEventBytes = 0;
  for (const plAudioEventMemorySnapshotEntry& event : s.m_Events)
  {
    uiEventBytes += event.m_uiSampleBytes;
  }

  const plUInt64 uiTotal = s.m_uiClipResidentBytes + uiEventBytes + s.m_uiStreamRingBytes;

  plDynamicArray<Row> rows;
  rows.Reserve(s.m_Clips.GetCount() + s.m_Events.GetCount() + s.m_Banks.GetCount() + 1);

  for (const plAudioClipMemorySnapshotEntry& clip : s.m_Clips)
  {
    Row& row = rows.ExpandAndGetRef();
    row.m_sName = ShortEventName(clip.m_sName);
    row.m_sTooltip = plMakeQString(clip.m_sName);
    row.m_sType = "Clip";
    row.m_sLoadMode = LoadModeName(clip.m_uiLoadMode);
    row.m_sCodec = CodecName(clip.m_uiCodec);
    row.m_sStreams = clip.m_uiLoadMode == plAudioClipLoadMode::Streamed ? QString::number(clip.m_uiStreams) : QString("-");
    row.m_uiResident = clip.m_uiResidentBytes;
  }

  for (const plAudioEventMemorySnapshotEntry& event : s.m_Events)
  {
    Row& row = rows.ExpandAndGetRef();
    row.m_sName = ShortEventName(event.m_sName);
    row.m_sTooltip = plMakeQString(event.m_sName);
    row.m_sType = "Event";
    row.m_sLoadMode = event.m_uiSampleBytes > 0 ? QString("Embedded samples") : QString("-");
    row.m_sCodec = "-";
    row.m_sStreams = "-";
    row.m_uiResident = event.m_uiSampleBytes;
  }

  if (s.m_uiStreamRingBytes > 0 || s.m_uiStreams > 0)
  {
    Row& row = rows.ExpandAndGetRef();
    row.m_sName = "Stream buffers";
    row.m_sTooltip = QString("Rings of the open and pooled disk streams (%1 open, %2 pooled)").arg(s.m_uiStreams).arg(s.m_uiPooledStreams);
    row.m_sType = "Streams";
    row.m_sLoadMode = "-";
    row.m_sCodec = "-";
    row.m_sStreams = QString::number(s.m_uiStreams);
    row.m_uiResident = s.m_uiStreamRingBytes;
  }

  for (const plAudioBankMemorySnapshotEntry& bank : s.m_Banks)
  {
    Row& row = rows.ExpandAndGetRef();
    row.m_sName = ShortEventName(bank.m_sName);
    row.m_sTooltip = plMakeQString(bank.m_sName);
    row.m_sType = "Bank";
    row.m_sCodec = QString("%1 events, %2 clips").arg(bank.m_uiEvents).arg(bank.m_uiClips);
    row.m_sStreams = "-";
    row.m_bHasResident = false;

    switch (bank.m_uiState)
    {
      case plAudioBankMemorySnapshotEntry::Loaded:
        row.m_sLoadMode = "loaded";
        break;
      case plAudioBankMemorySnapshotEntry::Missing:
        row.m_sLoadMode = "missing";
        row.m_TextColor = kStoppingColor;
        break;
      default:
        row.m_sLoadMode = "loading";
        break;
    }
  }

  // largest first; banks (no resident bytes of their own) last
  rows.Sort([](const Row& a, const Row& b)
    {
      if (a.m_bHasResident != b.m_bHasResident)
        return a.m_bHasResident;
      if (a.m_uiResident != b.m_uiResident)
        return a.m_uiResident > b.m_uiResident;
      return a.m_sName < b.m_sName;
    });

  m_pMemoryTable->setUpdatesEnabled(false);
  m_pMemoryTable->setRowCount(static_cast<int>(rows.GetCount()));

  for (plUInt32 uiRow = 0; uiRow < rows.GetCount(); ++uiRow)
  {
    const Row& row = rows[uiRow];
    const int iRow = static_cast<int>(uiRow);

    QTableWidgetItem* pName = EnsureItem(m_pMemoryTable, iRow, MemResource);
    pName->setText(row.m_sName);
    pName->setToolTip(row.m_sTooltip);

    EnsureItem(m_pMemoryTable, iRow, MemType)->setText(row.m_sType);
    EnsureItem(m_pMemoryTable, iRow, MemLoadMode)->setText(row.m_sLoadMode);
    EnsureItem(m_pMemoryTable, iRow, MemCodec)->setText(row.m_sCodec);

    QTableWidgetItem* pResident = EnsureItem(m_pMemoryTable, iRow, MemResident);
    pResident->setText(row.m_bHasResident ? FormatBytes(row.m_uiResident) : QString("-"));
    pResident->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

    QTableWidgetItem* pStreams = EnsureItem(m_pMemoryTable, iRow, MemStreams);
    pStreams->setText(row.m_sStreams);
    pStreams->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

    const float fShare = (row.m_bHasResident && uiTotal > 0) ? static_cast<float>(static_cast<double>(row.m_uiResident) / static_cast<double>(uiTotal)) : 0.0f;
    QTableWidgetItem* pShare = EnsureItem(m_pMemoryTable, iRow, MemShare);
    pShare->setData(Qt::UserRole, fShare);
    pShare->setToolTip(QString::asprintf("%.1f%%", fShare * 100.0f));

    for (int iColumn = 0; iColumn < MemCount; ++iColumn)
    {
      QTableWidgetItem* pItem = m_pMemoryTable->item(iRow, iColumn);
      if (row.m_TextColor.isValid())
        pItem->setForeground(row.m_TextColor);
      else
        pItem->setData(Qt::ForegroundRole, QVariant());
    }
  }

  m_pMemoryTable->setUpdatesEnabled(true);

  const QString sDot = MiddleDot();
  QString sTotals;
  sTotals += QString("Clips %1 (%2)").arg(FormatBytes(s.m_uiClipResidentBytes)).arg(s.m_Clips.GetCount());
  sTotals += sDot + QString("Event samples %1 (%2)").arg(FormatBytes(uiEventBytes)).arg(s.m_Events.GetCount());
  sTotals += sDot + QString("Stream rings %1").arg(FormatBytes(s.m_uiStreamRingBytes));
  sTotals += sDot + QString("Streams %1 open, %2 pooled").arg(s.m_uiStreams).arg(s.m_uiPooledStreams);
  sTotals += sDot + QString("Banks %1").arg(s.m_Banks.GetCount());
  sTotals += sDot + QString("Total %1").arg(FormatBytes(uiTotal));
  m_pMemoryTotals->setText(sTotals);
}

void plQtAudioProfilerPanel::CollectNewLogLines(plHybridArray<LogLine, 32>& out_lines)
{
  const plAudioProfilingSnapshot& s = m_Snapshot;

  const plUInt64 uiFirst = s.m_uiEventLogFirstSequence;
  const plUInt64 uiEnd = uiFirst + s.m_EventLog.GetCount();

  // the sequence went back: the engine log started over
  if (m_bLogSequenceKnown && uiEnd < m_uiLogNextSequence)
  {
    m_bLogSequenceKnown = false;
  }

  plUInt64 uiFrom = uiFirst;

  if (m_bLogSequenceKnown)
  {
    if (uiFirst > m_uiLogNextSequence)
    {
      LogLine& gap = out_lines.ExpandAndGetRef();
      gap.m_uiType = LogGapType;
      gap.m_uiLost = uiFirst - m_uiLogNextSequence;
      gap.m_fTime = s.m_EventLog.IsEmpty() ? 0.0 : s.m_EventLog[0].m_fTime;
    }

    uiFrom = plMath::Max(uiFirst, m_uiLogNextSequence);
  }

  for (plUInt64 uiSequence = uiFrom; uiSequence < uiEnd; ++uiSequence)
  {
    const plAudioEventLogSnapshotEntry& entry = s.m_EventLog[static_cast<plUInt32>(uiSequence - uiFirst)];
    LogLine& line = out_lines.ExpandAndGetRef();
    line.m_fTime = entry.m_fTime;
    line.m_uiType = entry.m_uiType;
    line.m_sName = entry.m_sName;
    line.m_sDetail = entry.m_sDetail;
  }

  m_uiLogNextSequence = m_bLogSequenceKnown ? plMath::Max(m_uiLogNextSequence, uiEnd) : uiEnd;
  m_bLogSequenceKnown = true;
}

void plQtAudioProfilerPanel::AppendLogEntries()
{
  plHybridArray<LogLine, 32> newLines;
  CollectNewLogLines(newLines);

  if (newLines.IsEmpty())
    return;

  // follow the newest line unless the user scrolled up
  QScrollBar* pScrollBar = m_pLogTable->verticalScrollBar();
  const bool bFollow = pScrollBar->value() >= pScrollBar->maximum() - 2;

  m_pLogTable->setUpdatesEnabled(false);

  for (const LogLine& line : newLines)
  {
    if (line.m_uiType != LogGapType)
    {
      ++m_uiLogReceived;
    }

    m_LogLines.PushBack(line);
    if (m_LogLines.GetCount() > kMaxLogLines)
    {
      m_LogLines.PopFront();
    }

    if (IsLogLineVisible(line))
    {
      AddLogRow(line);
    }
  }

  m_pLogTable->setUpdatesEnabled(true);

  if (bFollow)
  {
    m_pLogTable->scrollToBottom();
  }
}

bool plQtAudioProfilerPanel::IsLogLineVisible(const LogLine& line) const
{
  if (line.m_uiType == LogGapType)
    return true;

  if (!m_pLogCategory[GetLogCategory(line.m_uiType)]->isChecked())
    return false;

  const QString sFilter = m_pLogFilter->text().trimmed();
  if (sFilter.isEmpty())
    return true;

  return plMakeQString(line.m_sName).contains(sFilter, Qt::CaseInsensitive) || plMakeQString(line.m_sDetail).contains(sFilter, Qt::CaseInsensitive) ||
         QString(plAudioLogEventType::GetName(static_cast<plAudioLogEventType::Enum>(line.m_uiType))).contains(sFilter, Qt::CaseInsensitive);
}

void plQtAudioProfilerPanel::AddLogRow(const LogLine& line)
{
  if (m_pLogTable->rowCount() >= static_cast<int>(kMaxLogLines))
  {
    m_pLogTable->removeRow(0);
  }

  const int iRow = m_pLogTable->rowCount();
  m_pLogTable->insertRow(iRow);

  QTableWidgetItem* pTime = EnsureItem(m_pLogTable, iRow, LogTime);
  QTableWidgetItem* pType = EnsureItem(m_pLogTable, iRow, LogType);
  QTableWidgetItem* pText = EnsureItem(m_pLogTable, iRow, LogText);

  pTime->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

  if (line.m_uiType == LogGapType)
  {
    pTime->setText(QString());
    pType->setText("lost");
    pText->setText(QString("%1 entries lost (paused, or dropped before they were polled)").arg(line.m_uiLost));

    for (QTableWidgetItem* pItem : {pTime, pType, pText})
    {
      pItem->setForeground(QColor(0x6e, 0x6e, 0x6e));
    }
    return;
  }

  const auto type = static_cast<plAudioLogEventType::Enum>(line.m_uiType);

  pTime->setText(QString::asprintf("%.2f s", line.m_fTime));
  pType->setText(plAudioLogEventType::GetName(type));

  QString sText = ShortEventName(line.m_sName);
  if (!line.m_sDetail.IsEmpty())
  {
    sText += "   " + plMakeQString(line.m_sDetail);
  }
  pText->setText(sText);

  QString sTip = plMakeQString(line.m_sName);
  if (!line.m_sDetail.IsEmpty())
  {
    sTip += "\n" + plMakeQString(line.m_sDetail);
  }
  pText->setToolTip(sTip);

  const QColor color = plAudioLogEventType::IsWarning(type) ? kStoppingColor : QColor(0xcc, 0xcc, 0xcc);
  pType->setForeground(plAudioLogEventType::IsWarning(type) ? kStoppingColor : QColor(0x8e, 0x8e, 0x8e));
  pTime->setForeground(QColor(0x8e, 0x8e, 0x8e));
  pText->setForeground(color);
}

void plQtAudioProfilerPanel::RebuildLogTable()
{
  m_pLogTable->setUpdatesEnabled(false);
  m_pLogTable->setRowCount(0);

  for (const LogLine& line : m_LogLines)
  {
    if (IsLogLineVisible(line))
    {
      AddLogRow(line);
    }
  }

  m_pLogTable->setUpdatesEnabled(true);
  m_pLogTable->scrollToBottom();
}

void plQtAudioProfilerPanel::UpdateTabTexts()
{
  m_pTabs->setTabText(1, m_Snapshot.m_Buses.IsEmpty() ? QString("Buses") : QString("Buses  %1").arg(m_Snapshot.m_Buses.GetCount()));

  // clip data and stream rings plus the embedded samples of the events, as the memory tab totals them
  plUInt64 uiMemory = m_Snapshot.m_uiMemoryBytes;
  for (const plAudioEventMemorySnapshotEntry& event : m_Snapshot.m_Events)
  {
    uiMemory += event.m_uiSampleBytes;
  }
  m_pTabs->setTabText(2, QString("Memory  %1").arg(FormatBytes(uiMemory)));

  m_pTabs->setTabText(3, QString("Event log  %1").arg(m_uiLogReceived));
}

void plQtAudioProfilerPanel::OnTabChanged(int iTab)
{
  m_pPages->setCurrentIndex(iTab);

  if (!m_bHasSnapshot)
    return;

  if (iTab == 1)
  {
    RebuildBuses();
  }
  else if (iTab == 2)
  {
    RebuildMemory();
  }
}

void plQtAudioProfilerPanel::OnLogFilterChanged()
{
  RebuildLogTable();
}

void plQtAudioProfilerPanel::OnLogClear()
{
  // the sequence position is kept, so cleared entries are not received again
  m_LogLines.Clear();
  m_uiLogReceived = 0;
  m_pLogTable->setRowCount(0);
  UpdateTabTexts();
}

void plQtAudioProfilerPanel::RebuildTable()
{
  const QString sFilter = m_pEventFilter->text().trimmed();

  // real voices first, then by audibility; the id keeps the order stable
  plDynamicArray<const plAudioVoiceSnapshotEntry*> rows;
  rows.Reserve(m_Snapshot.m_Voices.GetCount());

  for (const plAudioVoiceSnapshotEntry& voice : m_Snapshot.m_Voices)
  {
    const plUInt8 uiState = plMath::Min<plUInt8>(voice.m_uiState, 2);
    if (!m_pStateFilter[uiState]->isChecked())
      continue;

    if (!sFilter.isEmpty() && !plMakeQString(voice.m_sEvent).contains(sFilter, Qt::CaseInsensitive))
      continue;

    rows.PushBack(&voice);
  }

  rows.Sort([](const plAudioVoiceSnapshotEntry* a, const plAudioVoiceSnapshotEntry* b)
    {
      const int iRankA = a->m_uiState == plAudioVoiceSnapshotEntry::Real ? 0 : 1;
      const int iRankB = b->m_uiState == plAudioVoiceSnapshotEntry::Real ? 0 : 1;
      if (iRankA != iRankB)
        return iRankA < iRankB;
      if (a->m_fAudibility != b->m_fAudibility)
        return a->m_fAudibility > b->m_fAudibility;
      return a->m_uiId < b->m_uiId;
    });

  if (rows.GetCount() > kMaxRows)
  {
    rows.SetCount(kMaxRows);
  }

  m_bUpdatingSelection = true;

  m_pTable->setUpdatesEnabled(false);
  m_pTable->clearSelection();
  m_pTable->setRowCount(static_cast<int>(rows.GetCount()));
  m_RowIds.SetCount(rows.GetCount());

  int iSelectedRow = -1;

  for (plUInt32 uiRow = 0; uiRow < rows.GetCount(); ++uiRow)
  {
    const plAudioVoiceSnapshotEntry& voice = *rows[uiRow];
    const int iRow = static_cast<int>(uiRow);
    m_RowIds[uiRow] = voice.m_uiId;

    QTableWidgetItem* pEvent = EnsureItem(m_pTable, iRow, ColEvent);
    pEvent->setText(ShortEventName(voice.m_sEvent));
    pEvent->setToolTip(plMakeQString(voice.m_sEvent));

    EnsureItem(m_pTable, iRow, ColEntity)->setText(EntityText(voice));

    QTableWidgetItem* pState = EnsureItem(m_pTable, iRow, ColState);
    pState->setText(StateName(voice.m_uiState));
    pState->setForeground(StateColor(voice.m_uiState));

    QTableWidgetItem* pPrio = EnsureItem(m_pTable, iRow, ColPrio);
    pPrio->setText(QString::number(voice.m_uiPriority));
    pPrio->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

    QTableWidgetItem* pAudibility = EnsureItem(m_pTable, iRow, ColAudibility);
    pAudibility->setData(Qt::UserRole, voice.m_fAudibility);
    pAudibility->setToolTip(QString::asprintf("%.3f", voice.m_fAudibility));

    QTableWidgetItem* pDist = EnsureItem(m_pTable, iRow, ColDist);
    pDist->setText(QString::asprintf("%.1f m", voice.m_fDistance));
    pDist->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

    EnsureItem(m_pTable, iRow, ColBus)->setText(plMakeQString(voice.m_sBus));
    EnsureItem(m_pTable, iRow, ColCodec)->setText(CodecText(voice));

    if (m_bHasSelection && voice.m_uiId == m_uiSelectedId)
    {
      iSelectedRow = iRow;
    }
  }

  if (iSelectedRow >= 0)
  {
    m_pTable->selectRow(iSelectedRow);
  }

  m_pTable->setUpdatesEnabled(true);
  m_bUpdatingSelection = false;

  // the selected voice keeps its last values once it ended or is filtered out
  if (m_bHasSelection)
  {
    m_bSelectedVoiceEnded = true;
    for (const plAudioVoiceSnapshotEntry& voice : m_Snapshot.m_Voices)
    {
      if (voice.m_uiId == m_uiSelectedId)
      {
        m_SelectedVoice = voice;
        m_bSelectedVoiceEnded = false;
        break;
      }
    }
  }

  m_pTabs->setTabText(0, QString("Voices  %1").arg(m_Snapshot.m_Voices.GetCount()));
  UpdateDetails();
}

void plQtAudioProfilerPanel::UpdateDetails()
{
  m_pDetailsGrid->setVisible(m_bHasSelection);
  m_pDetailsHint->setVisible(!m_bHasSelection);

  if (!m_bHasSelection)
  {
    m_pDetailsTitle->setText("VOICE");
    return;
  }

  const plAudioVoiceSnapshotEntry& voice = m_SelectedVoice;

  m_pDetailsTitle->setText(ShortEventName(voice.m_sEvent));
  m_pDetailsTitle->setToolTip(plMakeQString(voice.m_sEvent));

  m_pDetailValues[DetEntity]->setText(EntityText(voice));
  m_pDetailValues[DetState]->setText(m_bSelectedVoiceEnded ? QString("ended") : QString(StateName(voice.m_uiState)));
  m_pDetailValues[DetState]->setStyleSheet(QString("color: %1;").arg(m_bSelectedVoiceEnded ? QString("#8e8e8e") : StateColor(voice.m_uiState).name()));
  m_pDetailValues[DetPriority]->setText(QString::number(voice.m_uiPriority));
  m_pDetailValues[DetAudibility]->setText(QString::asprintf("%.3f", voice.m_fAudibility));
  m_pDetailValues[DetDistance]->setText(QString::asprintf("%.2f m", voice.m_fDistance));
  m_pDetailValues[DetBus]->setText(plMakeQString(voice.m_sBus));
  m_pDetailValues[DetCodec]->setText(CodecText(voice));
  m_pDetailValues[DetAge]->setText(QString::asprintf("%.2f s", voice.m_fAge));
  m_pDetailValues[DetOcclusion]->setText(QString::asprintf("%.2f", voice.m_fOcclusion));
}

void plQtAudioProfilerPanel::UpdateStatusLine()
{
  // the engine view draws its own status line
  if (m_pView != nullptr)
    return;

  const QString sDot = MiddleDot();

  if (!m_bHasSnapshot)
  {
    m_pStatusLine->setText(QString("Real -") + sDot + "Virtual -" + sDot + "Instances -" + sDot + "DSP -" + sDot + "Audio thread -" + sDot + "Streams -" + sDot + "Memory -");
    return;
  }

  const plAudioProfilingSnapshot& s = m_Snapshot;

  QString sText;
  sText += QString("Real %1/%2").arg(s.m_uiRealVoices).arg(s.m_uiMaxReal);
  sText += sDot + QString("Virtual %1/%2").arg(s.m_uiVirtualVoices).arg(s.m_uiMaxVirtual);
  sText += sDot + QString("Instances %1").arg(s.m_uiInstances);
  sText += sDot + QString::asprintf("DSP %.1f%%", s.m_fDspLoad * 100.0f);
  sText += sDot + QString::asprintf("Audio thread %.2f ms", s.m_fUpdateMs);
  sText += sDot + QString("Streams %1").arg(s.m_uiStreams);
  sText += sDot + QString("Memory ") + FormatBytes(s.m_uiMemoryBytes);

  m_pStatusLine->setText(sText);
}

void plQtAudioProfilerPanel::UpdateLinkIndicator()
{
  if (m_pView != nullptr)
  {
    m_pView->SetLinkState(m_LinkState == LinkState::Live ? plQtAudioProfilerView::LinkState::Live : (m_LinkState == LinkState::NoData ? plQtAudioProfilerView::LinkState::NoData : plQtAudioProfilerView::LinkState::NoEngine));
    return;
  }

  QColor dotColor = kOffColor;
  QString sText;

  switch (m_LinkState)
  {
    case LinkState::NoEngine:
      sText = "Engine process not running";
      break;
    case LinkState::NoData:
      dotColor = kStoppingColor;
      sText = "Engine process" + MiddleDot() + "no PlasmaAudio data";
      break;
    case LinkState::Live:
      dotColor = m_Accent;
      sText = "Engine process";
      break;
  }

  if (m_bPaused)
  {
    sText += MiddleDot() + "paused";
  }

  m_pLinkDot->setStyleSheet(QString("background: %1; border-radius: 4px;").arg(dotColor.name()));
  m_pLinkText->setText(sText);
}

void plQtAudioProfilerPanel::OnPauseToggled(bool bPaused)
{
  m_bPaused = bPaused;
  UpdateLinkIndicator();

  if (!m_bPaused)
  {
    OnTick();
  }
}

void plQtAudioProfilerPanel::OnFilterChanged()
{
  if (m_bHasSnapshot)
  {
    RebuildTable();
  }
}

void plQtAudioProfilerPanel::OnSelectionChanged()
{
  if (m_bUpdatingSelection)
    return;

  const QList<QTableWidgetItem*> selected = m_pTable->selectedItems();
  if (selected.isEmpty())
  {
    m_bHasSelection = false;
    UpdateDetails();
    return;
  }

  const int iRow = selected.front()->row();
  if (iRow < 0 || static_cast<plUInt32>(iRow) >= m_RowIds.GetCount())
    return;

  m_uiSelectedId = m_RowIds[iRow];
  m_bHasSelection = true;
  m_bSelectedVoiceEnded = true;

  for (const plAudioVoiceSnapshotEntry& voice : m_Snapshot.m_Voices)
  {
    if (voice.m_uiId == m_uiSelectedId)
    {
      m_SelectedVoice = voice;
      m_bSelectedVoiceEnded = false;
      break;
    }
  }

  UpdateDetails();
}
