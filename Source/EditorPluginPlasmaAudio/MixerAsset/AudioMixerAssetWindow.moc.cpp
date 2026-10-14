#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetStatusIndicator.moc.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerAssetWindow.moc.h>
#include <EditorPluginPlasmaAudio/Preview/AudioEnumRegistry.h>
#include <EditorPluginPlasmaAudio/Preview/AudioPreviewService.h>
#include <Foundation/Math/ColorScheme.h>
#include <GuiFoundation/ActionViews/MenuBarActionMapView.moc.h>
#include <GuiFoundation/ActionViews/ToolBarActionMapView.moc.h>
#include <GuiFoundation/DockPanels/DocumentPanel.moc.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

namespace
{
  constexpr int kStripWidth = 128;
  constexpr int kStripGap = 8;
  constexpr int kStripHeight = 430;
  constexpr int kHeaderHeight = 24;
  constexpr float kFaderMinDb = -60.0f;
  constexpr float kFaderMaxDb = 12.0f;

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

  void GatherChildrenOfProperty(const plDocumentObject* pParent, const char* szProperty, plHybridArray<const plDocumentObject*, 12>& out_children)
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

  void GatherChildrenOfProperty4(const plDocumentObject* pParent, const char* szProperty, plHybridArray<const plDocumentObject*, 4>& out_children)
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

  const char* EffectTypeName(plInt64 iType)
  {
    switch (iType)
    {
      case plAudioEffectType::LowPass:
        return "LowPass";
      case plAudioEffectType::HighPass:
        return "HighPass";
      case plAudioEffectType::Eq3:
        return "EQ3";
      case plAudioEffectType::Delay:
        return "Delay";
      case plAudioEffectType::Reverb:
        return "Reverb";
      case plAudioEffectType::Compressor:
        return "Compressor";
      case plAudioEffectType::Limiter:
        return "Limiter";
      default:
        return "?";
    }
  }

  QString ButtonRowStyle(const QColor& accent)
  {
    return QString(
      "QToolButton { background: #333333; border: 1px solid #3c3c3c; border-radius: 3px; padding: 3px 10px; color: #cccccc; }"
      "QToolButton:hover { border-color: %1; color: #ffffff; }"
      "QToolButton:pressed { background: #2a2a2a; }")
      .arg(accent.name());
  }

  QString PanelListStyle(const QColor& accent)
  {
    return QString(
      "QListWidget, QTreeWidget { background: #202020; border: none; color: #cccccc; font-size: 11px; outline: none; }"
      "QListWidget::item, QTreeWidget::item { padding: 3px 4px; }"
      "QListWidget::item:selected, QTreeWidget::item:selected { background: #2d7f69; color: #ffffff; }"
      "QListWidget::item:hover, QTreeWidget::item:hover { color: %1; }")
      .arg(accent.name());
  }

  QString DockHeaderStyle(const QColor& accent)
  {
    return QString("color: %1; font-size: 10px; font-weight: bold; letter-spacing: 1px; background: #191919; padding: 5px; border-bottom: 1px solid #303030;").arg(accent.name());
  }

  void SetGuidData(QTreeWidgetItem* pItem, const plUuid& guid)
  {
    plStringBuilder sGuid;
    plConversionUtils::ToString(guid, sGuid);
    pItem->setData(0, Qt::UserRole, QVariant(QString::fromUtf8(sGuid.GetData())));
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// plQtAudioMixerStripsWidget
//////////////////////////////////////////////////////////////////////////

plQtAudioMixerStripsWidget::plQtAudioMixerStripsWidget(QWidget* pParent, plDocument* pDocument, const QColor& accent)
  : QWidget(pParent)
  , m_pDocument(pDocument)
  , m_Accent(accent)
{
}

const plDocumentObject* plQtAudioMixerStripsWidget::GetPropertiesObject() const
{
  const auto& children = m_pDocument->GetObjectManager()->GetRootObject()->GetChildren();
  return children.IsEmpty() ? nullptr : children[0];
}

void plQtAudioMixerStripsWidget::GatherStrips(plHybridArray<const plDocumentObject*, 12>& out_buses, plHybridArray<const plDocumentObject*, 4>& out_vcas) const
{
  GatherChildrenOfProperty(GetPropertiesObject(), "Buses", out_buses);
  GatherChildrenOfProperty4(GetPropertiesObject(), "VCAs", out_vcas);
}

QSize plQtAudioMixerStripsWidget::sizeHint() const
{
  plHybridArray<const plDocumentObject*, 12> buses;
  plHybridArray<const plDocumentObject*, 4> vcas;
  GatherStrips(buses, vcas);

  const plUInt32 uiStrips = plMath::Max<plUInt32>(1, buses.GetCount() + vcas.GetCount());
  return QSize(12 + uiStrips * (kStripWidth + kStripGap), kStripHeight + 16);
}

void plQtAudioMixerStripsWidget::GetLiveMeter(plUInt32 uiBusIndex, float& out_fPeak, float& out_fRms) const
{
  out_fPeak = 0.0f;
  out_fRms = 0.0f;

  if (uiBusIndex == plInvalidIndex)
    return;

  plAudioPreviewService::GetSingleton().GetBusMeter(uiBusIndex, out_fPeak, out_fRms);
}

float plQtAudioMixerStripsWidget::PosToDb(const QRect& faderRect, int y)
{
  const float fT = plMath::Clamp(static_cast<float>(y - faderRect.top()) / static_cast<float>(plMath::Max(1, faderRect.height())), 0.0f, 1.0f);
  return kFaderMaxDb + fT * (kFaderMinDb - kFaderMaxDb);
}

int plQtAudioMixerStripsWidget::DbToPos(const QRect& faderRect, float fDb)
{
  const float fT = plMath::Clamp((kFaderMaxDb - fDb) / (kFaderMaxDb - kFaderMinDb), 0.0f, 1.0f);
  return faderRect.top() + static_cast<int>(fT * faderRect.height());
}

void plQtAudioMixerStripsWidget::DrawStrip(QPainter& p, const QRect& stripRect, const plDocumentObject* pObject, plUInt32 uiBusIndex, bool bVca, bool bMaster, bool bSelected)
{
  const auto& accessor = pObject->GetTypeAccessor();

  const QColor panelColor(32, 32, 32);
  const QColor lineColor(60, 60, 60);
  const QColor textColor(204, 204, 204);
  const QColor dimColor(142, 142, 142);

  const bool bAux = !bVca && accessor.GetValue("IsAux").ConvertTo<bool>();

  // frame
  p.setBrush(panelColor);
  QPen framePen(bSelected ? m_Accent : lineColor, bSelected ? 2 : 1);
  if (bVca)
    framePen.setStyle(Qt::DashLine);
  p.setPen(framePen);
  p.drawRoundedRect(stripRect, 5, 5);

  // header
  QColor headerColor = bMaster ? m_Accent : (bVca ? QColor(160, 160, 160) : (bAux ? QColor(170, 130, 210) : QColor(127, 178, 217)));
  {
    QPainterPath headerPath;
    headerPath.addRoundedRect(QRectF(stripRect.left(), stripRect.top(), stripRect.width(), kHeaderHeight), 5, 5);
    headerPath.addRect(QRectF(stripRect.left(), stripRect.top() + kHeaderHeight - 6, stripRect.width(), 6));
    p.fillPath(headerPath.simplified(), headerColor);
  }

  plString sName = accessor.GetValue("Name").ConvertTo<plString>();
  if (sName.IsEmpty())
    sName = bVca ? "VCA" : "Bus";

  QFont nameFont = font();
  nameFont.setBold(true);
  p.setFont(nameFont);
  p.setPen(QColor(16, 22, 20));

  QString sHeader = QString::fromUtf8(sName.GetData());
  if (bVca)
    sHeader = QString::fromUtf8("VCA \xC2\xB7 ") + sHeader;
  if (bAux)
    sHeader += QString::fromUtf8(" \xE2\xA4\xBA");
  p.drawText(QRect(stripRect.left() + 4, stripRect.top(), stripRect.width() - 8, kHeaderHeight), Qt::AlignCenter, sHeader);

  QFont smallFont = font();
  smallFont.setPointSizeF(7.5);

  int y = stripRect.top() + kHeaderHeight + 6;

  // inserts (buses) / controlled buses (VCAs)
  {
    p.setFont(smallFont);
    p.setPen(dimColor);
    p.drawText(QRect(stripRect.left() + 8, y, stripRect.width() - 16, 12), Qt::AlignLeft, bVca ? "CONTROLS" : "INSERTS");
    y += 14;

    plHybridArray<QString, 4> chips;
    bool bChipsActive = false;

    if (bVca)
    {
      // "Buses" is a string array value, not child objects
      const plInt32 iCount = pObject->GetTypeAccessor().GetCount("Buses");
      bChipsActive = iCount > 0;
      for (plInt32 i = 0; i < plMath::Min<plInt32>(iCount, 3); ++i)
      {
        const plString sBus = pObject->GetTypeAccessor().GetValue("Buses", i).ConvertTo<plString>();
        chips.PushBack(QString::fromUtf8(sBus.GetData()));
      }
      if (iCount > 3)
        chips.PushBack(QString::asprintf("+%d more", iCount - 3));
      if (iCount == 0)
        chips.PushBack("- none -");
    }
    else
    {
      plHybridArray<const plDocumentObject*, 12> effects;
      GatherChildrenOfProperty(pObject, "Effects", effects);
      bChipsActive = !effects.IsEmpty();

      for (plUInt32 i = 0; i < plMath::Min<plUInt32>(effects.GetCount(), 3); ++i)
      {
        const plInt64 iType = effects[i]->GetTypeAccessor().GetValue("Type").ConvertTo<plInt64>();
        chips.PushBack(QString::fromUtf8(EffectTypeName(iType)));
      }
      if (effects.GetCount() > 3)
        chips.PushBack(QString::asprintf("+%u more", effects.GetCount() - 3));
      if (effects.IsEmpty())
        chips.PushBack("+ effect");
    }

    for (const QString& sChip : chips)
    {
      const QRect chipRect(stripRect.left() + 8, y, stripRect.width() - 16, 15);
      p.setPen(QPen(lineColor, 1));
      p.setBrush(QColor(45, 45, 45));
      p.drawRoundedRect(chipRect, 3, 3);

      if (bChipsActive)
      {
        // accent bar like the mockup's active insert slots
        p.fillRect(QRect(chipRect.left(), chipRect.top() + 2, 3, chipRect.height() - 4), m_Accent);
      }

      p.setPen(bChipsActive ? textColor : dimColor);
      p.drawText(chipRect.adjusted(8, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter, sChip);
      y += 18;
    }
  }

  // sends (buses only) / environment (aux)
  if (!bVca)
  {
    y += 2;
    p.setPen(dimColor);
    p.drawText(QRect(stripRect.left() + 8, y, stripRect.width() - 16, 12), Qt::AlignLeft, bAux ? "ENVIRONMENT" : "SENDS");
    y += 14;

    if (bAux)
    {
      const plString sEnv = accessor.GetValue("Environment").ConvertTo<plString>();
      p.setPen(sEnv.IsEmpty() ? dimColor : textColor);
      p.drawText(QRect(stripRect.left() + 8, y, stripRect.width() - 16, 12), Qt::AlignLeft, sEnv.IsEmpty() ? "- none -" : QString::fromUtf8(sEnv.GetData()));
      y += 14;
    }
    else
    {
      plHybridArray<const plDocumentObject*, 12> sends;
      GatherChildrenOfProperty(pObject, "Sends", sends);

      if (sends.IsEmpty())
      {
        p.setPen(dimColor);
        p.drawText(QRect(stripRect.left() + 8, y, stripRect.width() - 16, 12), Qt::AlignLeft, "-");
        y += 14;
      }
      else
      {
        for (plUInt32 i = 0; i < plMath::Min<plUInt32>(sends.GetCount(), 2); ++i)
        {
          const plString sTarget = sends[i]->GetTypeAccessor().GetValue("TargetBus").ConvertTo<plString>();
          const double fLevel = sends[i]->GetTypeAccessor().GetValue("Level").ConvertTo<double>();
          p.setPen(textColor);
          p.drawText(QRect(stripRect.left() + 8, y, stripRect.width() - 16, 12), Qt::AlignLeft, QString::asprintf("%s  %.0f dB", sTarget.GetData(), fLevel));
          y += 14;
        }
      }
    }
  }
  else
  {
    y += 16;
  }

  // fader + groove + meter placeholder
  const double fVolumeDb = accessor.GetValue("Volume").ConvertTo<double>();
  const bool bDraggingThis = m_bDragging && pObject->GetGuid() == m_DragGuid;
  const float fShownDb = bDraggingThis ? m_fDragValueDb : static_cast<float>(fVolumeDb);

  const QRect faderRect(stripRect.left() + stripRect.width() / 2 - 20, y + 8, 12, stripRect.bottom() - y - 50);

  // scale marks
  p.setFont(smallFont);
  p.setPen(dimColor);
  for (float fMark : {12.0f, 0.0f, -12.0f, -36.0f, -60.0f})
  {
    const int iMarkY = DbToPos(faderRect, fMark);
    p.drawLine(faderRect.left() - 12, iMarkY, faderRect.left() - 4, iMarkY);
    p.drawText(QRect(faderRect.left() - 40, iMarkY - 6, 26, 12), Qt::AlignRight | Qt::AlignVCenter, fMark <= kFaderMinDb ? QString::fromUtf8("-\xE2\x88\x9E") : QString::asprintf("%+.0f", fMark));
  }

  // 0 dB line across the groove
  {
    const int iZeroY = DbToPos(faderRect, 0.0f);
    p.setPen(QPen(QColor(80, 80, 80), 1, Qt::DotLine));
    p.drawLine(faderRect.left() - 2, iZeroY, faderRect.right() + (bVca ? 2 : 26), iZeroY);
  }

  // groove
  p.setPen(QPen(QColor(10, 10, 10), 1));
  p.setBrush(QColor(18, 18, 18));
  p.drawRoundedRect(faderRect, 5, 5);

  // gain fill inside the groove up to the cap
  {
    const int iCapY = DbToPos(faderRect, fShownDb);
    QColor fillColor = bMaster ? m_Accent : QColor(63, 174, 124);
    fillColor.setAlpha(120);
    p.setPen(Qt::NoPen);
    p.setBrush(fillColor);
    p.drawRoundedRect(QRect(faderRect.left() + 2, iCapY, faderRect.width() - 4, faderRect.bottom() - iCapY - 1), 3, 3);
  }

  // live meter, fed by the editor preview engine (VCAs are pure gain and have no signal)
  if (!bVca)
  {
    const QRect meterRect(faderRect.right() + 14, faderRect.top(), 10, faderRect.height());
    p.setPen(QPen(QColor(10, 10, 10), 1));
    p.setBrush(QColor(14, 14, 14));
    p.drawRect(meterRect);

    float fPeak = 0.0f;
    float fRms = 0.0f;
    GetLiveMeter(uiBusIndex, fPeak, fRms);

    const QRect inner = meterRect.adjusted(1, 1, -1, -1);

    // RMS body with the classic green/amber/red scale, plus a peak line
    if (fRms > 0.0001f)
    {
      const int iHeight = static_cast<int>(inner.height() * plMath::Clamp(fRms, 0.0f, 1.0f));

      QLinearGradient gradient(inner.bottomLeft(), inner.topLeft());
      gradient.setColorAt(0.0, QColor(63, 174, 124));
      gradient.setColorAt(0.7, QColor(63, 174, 124));
      gradient.setColorAt(0.88, QColor(217, 195, 74));
      gradient.setColorAt(1.0, QColor(217, 93, 93));

      p.fillRect(QRect(inner.left(), inner.bottom() - iHeight, inner.width(), iHeight), gradient);
    }

    if (fPeak > 0.0001f)
    {
      const int iPeakY = inner.bottom() - static_cast<int>(inner.height() * plMath::Clamp(fPeak, 0.0f, 1.0f));
      p.setPen(QPen(fPeak >= 0.99f ? QColor(255, 120, 120) : QColor(220, 220, 220), 1));
      p.drawLine(inner.left(), iPeakY, inner.right(), iPeakY);
    }

    // idle tick marks so the meter still reads as a meter when silent
    if (fRms <= 0.0001f)
    {
      p.setPen(QColor(40, 40, 40));
      for (int my = meterRect.top() + 4; my < meterRect.bottom() - 2; my += 5)
      {
        p.drawLine(meterRect.left() + 2, my, meterRect.right() - 2, my);
      }
    }
  }

  // cap
  const int iCapY = DbToPos(faderRect, fShownDb);
  const QRect capRect(faderRect.left() - 6, iCapY - 7, faderRect.width() + 12, 14);

  QLinearGradient capGradient(capRect.topLeft(), capRect.bottomLeft());
  if (bDraggingThis)
  {
    capGradient.setColorAt(0.0, m_Accent.lighter(125));
    capGradient.setColorAt(1.0, m_Accent.darker(120));
  }
  else
  {
    capGradient.setColorAt(0.0, QColor(229, 229, 229));
    capGradient.setColorAt(0.45, QColor(154, 154, 154));
    capGradient.setColorAt(0.55, QColor(120, 120, 120));
    capGradient.setColorAt(1.0, QColor(154, 154, 154));
  }

  p.setPen(QPen(QColor(0, 0, 0), 1));
  p.setBrush(capGradient);
  p.drawRoundedRect(capRect, 3, 3);

  // center line on the cap
  p.setPen(QPen(QColor(40, 40, 40), 1));
  p.drawLine(capRect.left() + 3, capRect.center().y(), capRect.right() - 3, capRect.center().y());

  // dB label
  p.setFont(nameFont);
  p.setPen(textColor);
  p.drawText(QRect(stripRect.left(), stripRect.bottom() - 38, stripRect.width(), 14), Qt::AlignCenter, fShownDb <= kFaderMinDb ? QString::fromUtf8("-\xE2\x88\x9E dB") : QString::asprintf("%+.1f dB", fShownDb));

  // routing label
  p.setFont(smallFont);
  p.setPen(dimColor);
  QString sRouting;
  if (bVca)
  {
    sRouting = "options-menu group";
  }
  else if (bMaster)
  {
    sRouting = QString::fromUtf8("\xE2\x86\x92 output device");
  }
  else
  {
    const plString sParent = accessor.GetValue("Parent").ConvertTo<plString>();
    sRouting = QString::fromUtf8("\xE2\x86\x92 ") + (sParent.IsEmpty() ? QString("Master") : QString::fromUtf8(sParent.GetData()));
  }
  p.drawText(QRect(stripRect.left(), stripRect.bottom() - 20, stripRect.width(), 12), Qt::AlignCenter, sRouting);

  auto& hit = m_HitRects.ExpandAndGetRef();
  hit.m_Guid = pObject->GetGuid();
  hit.m_StripRect = stripRect;
  hit.m_FaderRect = faderRect.adjusted(-10, -10, 26, 10);
  hit.m_FaderTrack = faderRect;
}

void plQtAudioMixerStripsWidget::paintEvent(QPaintEvent* pEvent)
{
  PL_IGNORE_UNUSED(pEvent);

  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  p.fillRect(rect(), QColor(30, 30, 30));

  m_HitRects.Clear();

  plHybridArray<const plDocumentObject*, 12> buses;
  plHybridArray<const plDocumentObject*, 4> vcas;
  GatherStrips(buses, vcas);

  if (buses.IsEmpty() && vcas.IsEmpty())
  {
    p.setPen(QColor(142, 142, 142));
    p.drawText(rect(), Qt::AlignCenter, "No buses yet.\nUse '+ Bus' to create the mixer - the first bus is the Master bus.");
    return;
  }

  const plUuid selectedGuid = (m_pDocument->GetSelectionManager()->GetCurrentObject() != nullptr)
                                ? m_pDocument->GetSelectionManager()->GetCurrentObject()->GetGuid()
                                : plUuid();

  int x = 8;

  for (plUInt32 i = 0; i < buses.GetCount(); ++i)
  {
    const QRect stripRect(x, 8, kStripWidth, kStripHeight);
    DrawStrip(p, stripRect, buses[i], i, false, i == 0, buses[i]->GetGuid() == selectedGuid);
    x += kStripWidth + kStripGap;
  }

  if (!vcas.IsEmpty())
  {
    // subtle divider between buses and VCAs
    p.setPen(QPen(QColor(60, 60, 60), 1, Qt::DashLine));
    p.drawLine(x - kStripGap / 2, 12, x - kStripGap / 2, kStripHeight);
  }

  for (plUInt32 i = 0; i < vcas.GetCount(); ++i)
  {
    const QRect stripRect(x, 8, kStripWidth, kStripHeight);
    DrawStrip(p, stripRect, vcas[i], plInvalidIndex, true, false, vcas[i]->GetGuid() == selectedGuid);
    x += kStripWidth + kStripGap;
  }
}

void plQtAudioMixerStripsWidget::mousePressEvent(QMouseEvent* pEvent)
{
  if (pEvent->button() == Qt::LeftButton)
  {
    for (const auto& hit : m_HitRects)
    {
      if (!hit.m_StripRect.contains(pEvent->pos()))
        continue;

      if (const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(hit.m_Guid))
      {
        m_pDocument->GetSelectionManager()->SetSelection(pObject);
      }

      if (hit.m_FaderRect.contains(pEvent->pos()))
      {
        m_bDragging = true;
        m_DragGuid = hit.m_Guid;
        m_DragFaderTrack = hit.m_FaderTrack;
        m_fDragValueDb = PosToDb(m_DragFaderTrack, pEvent->pos().y());
      }

      update();
      return;
    }
  }

  QWidget::mousePressEvent(pEvent);
}

void plQtAudioMixerStripsWidget::mouseMoveEvent(QMouseEvent* pEvent)
{
  if (m_bDragging)
  {
    m_fDragValueDb = PosToDb(m_DragFaderTrack, pEvent->pos().y());
    update();
    return;
  }

  QWidget::mouseMoveEvent(pEvent);
}

void plQtAudioMixerStripsWidget::mouseReleaseEvent(QMouseEvent* pEvent)
{
  if (m_bDragging && pEvent->button() == Qt::LeftButton)
  {
    m_bDragging = false;

    if (const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(m_DragGuid))
    {
      // snap near common values
      float fValue = m_fDragValueDb;
      if (plMath::Abs(fValue) < 0.75f)
        fValue = 0.0f;
      fValue = plMath::RoundToMultiple(fValue, 0.5f);

      plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
      accessor.StartTransaction("Set Volume");
      accessor.SetValueByName(pObject, "Volume", fValue).IgnoreResult();
      accessor.FinishTransaction();
    }

    update();
    return;
  }

  QWidget::mouseReleaseEvent(pEvent);
}

//////////////////////////////////////////////////////////////////////////
// plAudioMixerAssetDocumentWindow
//////////////////////////////////////////////////////////////////////////

plAudioMixerAssetDocumentWindow::plAudioMixerAssetDocumentWindow(plDocument* pDocument)
  : plQtDocumentWindow(pDocument)
{
  m_pAssetDoc = static_cast<plAudioMixerAssetDocument*>(pDocument);
  m_Accent = AccentColor(pDocument);

  // Menu Bar
  {
    plQtMenuBarActionMapView* pMenuBar = static_cast<plQtMenuBarActionMapView*>(menuBar());
    plActionContext context;
    context.m_sMapping = "AudioMixerAssetMenuBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pMenuBar->SetActionContext(context);
  }

  // Tool Bar
  {
    plQtToolBarActionMapView* pToolBar = new plQtToolBarActionMapView("Toolbar", this);
    plActionContext context;
    context.m_sMapping = "AudioMixerAssetToolBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pToolBar->SetActionContext(context);
    pToolBar->setObjectName("AudioMixerAssetWindowToolBar");
    addToolBar(pToolBar);
  }

  // Central: add-button row + strips + status bar
  {
    QWidget* pCentralWidget = new QWidget(this);
    QVBoxLayout* pCentralLayout = new QVBoxLayout(pCentralWidget);
    pCentralLayout->setContentsMargins(0, 0, 0, 0);
    pCentralLayout->setSpacing(0);

    {
      QWidget* pButtonRow = new QWidget(pCentralWidget);
      pButtonRow->setStyleSheet(ButtonRowStyle(m_Accent));

      QHBoxLayout* pButtonLayout = new QHBoxLayout(pButtonRow);
      pButtonLayout->setContentsMargins(6, 5, 6, 5);
      pButtonLayout->setSpacing(6);

      auto addButton = [&](const char* szText, const char* szToolTip, std::function<void()> func)
      {
        QToolButton* pButton = new QToolButton(pButtonRow);
        pButton->setText(QString::fromUtf8(szText));
        pButton->setToolTip(QString::fromUtf8(szToolTip));
        connect(pButton, &QToolButton::clicked, this, [func]()
          { func(); });
        pButtonLayout->addWidget(pButton);
      };

      addButton("+ Bus", "Adds a mixer bus (the first bus is the Master bus)", [this]()
        { OnAddBus(false); });
      addButton("+ Aux Bus", "Adds an aux/return bus (reverb; set its Environment name to feed it from environment components)", [this]()
        { OnAddBus(true); });
      addButton("+ VCA", "Adds a VCA (options-menu volume group over a set of buses)", [this]()
        { OnAddVca(); });
      addButton("+ Snapshot", "Adds a blendable mixer snapshot", [this]()
        { OnAddSnapshot(); });
      pButtonLayout->addSpacing(18);
      addButton("Remove Selected", "Removes the selected bus / VCA / snapshot", [this]()
        { OnRemoveSelected(); });

      pButtonLayout->addStretch();

      pCentralLayout->addWidget(pButtonRow);
    }

    QScrollArea* pScroll = new QScrollArea(pCentralWidget);
    pScroll->setWidgetResizable(true);
    pScroll->setFrameShape(QFrame::NoFrame);

    m_pStrips = new plQtAudioMixerStripsWidget(pScroll, pDocument, m_Accent);
    pScroll->setWidget(m_pStrips);
    pCentralLayout->addWidget(pScroll);

    // profiler-style status bar
    {
      m_pStatusLabel = new QLabel(pCentralWidget);
      m_pStatusLabel->setStyleSheet("color: #8e8e8e; font-size: 10px; background: #181818; padding: 4px 10px; border-top: 1px solid #000000;");
      pCentralLayout->addWidget(m_pStatusLabel);
    }

    plQtDocumentPanel* pCentral = new plQtDocumentPanel(this, pDocument);
    pCentral->setObjectName("AudioMixerConsoleDockWidget");
    pCentral->setWindowTitle("Mixer Console");
    pCentral->setWidget(pCentralWidget, ads::CDockWidget::ForceNoScrollArea);
    m_pDockManager->setCentralWidget(pCentral);
  }

  // Left dock: routing tree
  {
    plQtDocumentPanel* pRoutingPanel = new plQtDocumentPanel(this, pDocument);
    pRoutingPanel->setObjectName("AudioMixerRoutingDockWidget");
    pRoutingPanel->setWindowTitle("Routing");
    pRoutingPanel->show();

    QWidget* pWidget = new QWidget();
    pWidget->setObjectName("Group");
    QVBoxLayout* pLayout = new QVBoxLayout(pWidget);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(0);

    QLabel* pHeader = new QLabel("  ROUTING", pWidget);
    pHeader->setStyleSheet(DockHeaderStyle(m_Accent));
    pLayout->addWidget(pHeader);

    m_pRoutingTree = new QTreeWidget(pWidget);
    m_pRoutingTree->setHeaderHidden(true);
    m_pRoutingTree->setStyleSheet(PanelListStyle(m_Accent));
    m_pRoutingTree->setIndentation(14);
    connect(m_pRoutingTree, &QTreeWidget::itemClicked, this, &plAudioMixerAssetDocumentWindow::OnRoutingItemClicked);
    pLayout->addWidget(m_pRoutingTree);

    pRoutingPanel->setWidget(pWidget, ads::CDockWidget::ForceNoScrollArea);
    m_pDockManager->addDockWidget(ads::LeftDockWidgetArea, pRoutingPanel);
  }

  // Right dock 1: snapshots
  {
    plQtDocumentPanel* pSnapshotPanel = new plQtDocumentPanel(this, pDocument);
    pSnapshotPanel->setObjectName("AudioMixerSnapshotsDockWidget");
    pSnapshotPanel->setWindowTitle("Snapshots");
    pSnapshotPanel->show();

    QWidget* pWidget = new QWidget();
    pWidget->setObjectName("Group");
    QVBoxLayout* pLayout = new QVBoxLayout(pWidget);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(0);

    QLabel* pHeader = new QLabel("  SNAPSHOTS", pWidget);
    pHeader->setStyleSheet(DockHeaderStyle(m_Accent));
    pLayout->addWidget(pHeader);

    m_pSnapshotList = new QListWidget(pWidget);
    m_pSnapshotList->setFrameShape(QFrame::NoFrame);
    m_pSnapshotList->setStyleSheet(PanelListStyle(m_Accent));
    connect(m_pSnapshotList, &QListWidget::itemClicked, this, &plAudioMixerAssetDocumentWindow::OnSnapshotItemClicked);
    pLayout->addWidget(m_pSnapshotList);

    pSnapshotPanel->setWidget(pWidget, ads::CDockWidget::ForceNoScrollArea);
    m_pDockManager->addDockWidgetTab(ads::RightDockWidgetArea, pSnapshotPanel);
  }

  // Right dock 2: properties of the current selection
  {
    plQtDocumentPanel* pPropertyPanel = new plQtDocumentPanel(this, pDocument);
    pPropertyPanel->setObjectName("AudioMixerAssetDockWidget");
    pPropertyPanel->setWindowTitle("Properties");
    pPropertyPanel->show();

    plQtPropertyGridWidget* pPropertyGrid = new plQtPropertyGridWidget(pPropertyPanel, pDocument);

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

  GetDocument()->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::StructureEventHandler, this));
  GetDocument()->GetSelectionManager()->m_Events.AddEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::SelectionEventHandler, this));

  // repaint the strips while anything is auditioning, so the meters animate
  m_pMeterTimer = new QTimer(this);
  m_pMeterTimer->setInterval(50);
  connect(m_pMeterTimer, &QTimer::timeout, this, [this]()
    {
      if (m_pStrips != nullptr && plAudioPreviewService::GetSingleton().HasMixer())
      {
        m_pStrips->update();
      } });
  m_pMeterTimer->start();

  FinishWindowCreation();

  QueueRefresh();
}

plAudioMixerAssetDocumentWindow::~plAudioMixerAssetDocumentWindow()
{
  // the preview graph mirrors this document, so it must not outlive the window
  plAudioPreviewService::GetSingleton().ClearMixer();

  GetDocument()->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::StructureEventHandler, this));
  GetDocument()->GetSelectionManager()->m_Events.RemoveEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::SelectionEventHandler, this));
}

const plDocumentObject* plAudioMixerAssetDocumentWindow::GetPropertiesObject() const
{
  const auto& children = GetDocument()->GetObjectManager()->GetRootObject()->GetChildren();
  return children.IsEmpty() ? nullptr : children[0];
}

void plAudioMixerAssetDocumentWindow::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  PL_IGNORE_UNUSED(e);
  QueueRefresh();
}

void plAudioMixerAssetDocumentWindow::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  PL_IGNORE_UNUSED(e);
  QueueRefresh();
}

void plAudioMixerAssetDocumentWindow::SelectionEventHandler(const plSelectionManagerEvent& e)
{
  PL_IGNORE_UNUSED(e);
  QueueRefresh();
}

void plAudioMixerAssetDocumentWindow::QueueRefresh()
{
  if (m_bRefreshQueued)
    return;

  m_bRefreshQueued = true;

  QTimer::singleShot(0, this, [this]()
    {
      m_bRefreshQueued = false;

      if (m_pStrips != nullptr)
      {
        m_pStrips->updateGeometry();
        m_pStrips->update();
      }

      RebuildSnapshotList();
      RebuildRoutingTree();

      // keep the preview graph in step with edits, so meters and audible bus volumes match the asset
      PushMixerToPreview();

      if (m_pStatusLabel != nullptr)
      {
        plHybridArray<const plDocumentObject*, 12> buses, snapshots;
        plHybridArray<const plDocumentObject*, 4> vcas;
        GatherChildrenOfProperty(GetPropertiesObject(), "Buses", buses);
        GatherChildrenOfProperty4(GetPropertiesObject(), "VCAs", vcas);
        GatherChildrenOfProperty(GetPropertiesObject(), "Snapshots", snapshots);

        plUInt32 uiEffects = 0;
        plUInt32 uiAux = 0;
        for (const plDocumentObject* pBus : buses)
        {
          plHybridArray<const plDocumentObject*, 12> effects;
          GatherChildrenOfProperty(pBus, "Effects", effects);
          uiEffects += effects.GetCount();
          if (pBus->GetTypeAccessor().GetValue("IsAux").ConvertTo<bool>())
            ++uiAux;
        }

        m_pStatusLabel->setText(QString::asprintf("%u buses (%u aux) \xC2\xB7 %u VCAs \xC2\xB7 %u snapshots \xC2\xB7 %u insert effects \xC2\xB7 backend: MiniAudio \xC2\xB7 live meters need a running engine",
          buses.GetCount(), uiAux, vcas.GetCount(), snapshots.GetCount(), uiEffects));
      } });
}

void plAudioMixerAssetDocumentWindow::PushMixerToPreview()
{
  const plAudioMixerAssetProperties* pProps = m_pAssetDoc != nullptr ? m_pAssetDoc->GetProperties() : nullptr;
  if (pProps == nullptr)
    return;

  plAudioMixerDesc desc;

  // an invalid mixer (dangling bus names) simply is not previewed
  if (plAudioMixerAssetProperties::BuildRuntimeDesc(*pProps, desc).Failed())
    return;

  plAudioPreviewService::GetSingleton().SetMixer(desc);

  // offer these names as dropdown choices on the audio components
  plAudioEnumRegistry::RegisterMixerNames(desc);
}

void plAudioMixerAssetDocumentWindow::RebuildRoutingTree()
{
  if (m_pRoutingTree == nullptr)
    return;

  m_pRoutingTree->clear();

  const plDocumentObject* pProps = GetPropertiesObject();

  plHybridArray<const plDocumentObject*, 12> buses, snapshots;
  plHybridArray<const plDocumentObject*, 4> vcas;
  GatherChildrenOfProperty(pProps, "Buses", buses);
  GatherChildrenOfProperty4(pProps, "VCAs", vcas);
  GatherChildrenOfProperty(pProps, "Snapshots", snapshots);

  auto makeGroup = [&](const char* szName) -> QTreeWidgetItem*
  {
    QTreeWidgetItem* pGroup = new QTreeWidgetItem(m_pRoutingTree, QStringList(QString::fromUtf8(szName)));
    pGroup->setFlags(Qt::ItemIsEnabled);
    QFont groupFont = m_pRoutingTree->font();
    groupFont.setBold(true);
    groupFont.setPointSizeF(7.5);
    pGroup->setFont(0, groupFont);
    pGroup->setForeground(0, QBrush(m_Accent));
    return pGroup;
  };

  // buses as a hierarchy (parent resolution by name; unknown parents land under the master bus)
  {
    QTreeWidgetItem* pGroup = makeGroup("BUSES");

    plHybridArray<QTreeWidgetItem*, 12> busItems;
    busItems.SetCount(buses.GetCount());

    auto findBusIndex = [&](plStringView sName) -> plUInt32
    {
      for (plUInt32 i = 0; i < buses.GetCount(); ++i)
      {
        const plString sBusName = buses[i]->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
        if (sBusName.IsEqual_NoCase(sName))
          return i;
      }
      return plInvalidIndex;
    };

    for (plUInt32 i = 0; i < buses.GetCount(); ++i)
    {
      plString sName = buses[i]->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
      if (sName.IsEmpty())
        sName = "<unnamed>";

      const bool bAux = buses[i]->GetTypeAccessor().GetValue("IsAux").ConvertTo<bool>();

      QTreeWidgetItem* pParentItem = pGroup;
      if (i > 0)
      {
        const plString sParent = buses[i]->GetTypeAccessor().GetValue("Parent").ConvertTo<plString>();
        const plUInt32 uiParent = sParent.IsEmpty() ? 0 : findBusIndex(sParent);
        if (uiParent != plInvalidIndex && uiParent < i && busItems[uiParent] != nullptr)
        {
          pParentItem = busItems[uiParent];
        }
        else if (busItems[0] != nullptr)
        {
          pParentItem = busItems[0];
        }
      }

      QString sLabel = QString::fromUtf8(sName.GetData());
      if (bAux)
        sLabel += QString::fromUtf8("  \xE2\xA4\xBA aux");

      QTreeWidgetItem* pItem = new QTreeWidgetItem(pParentItem, QStringList(sLabel));
      SetGuidData(pItem, buses[i]->GetGuid());
      busItems[i] = pItem;
    }
  }

  // VCAs
  {
    QTreeWidgetItem* pGroup = makeGroup("VCAS");
    for (const plDocumentObject* pVca : vcas)
    {
      plString sName = pVca->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
      if (sName.IsEmpty())
        sName = "<unnamed>";

      QTreeWidgetItem* pItem = new QTreeWidgetItem(pGroup, QStringList(QString::fromUtf8(sName.GetData())));
      SetGuidData(pItem, pVca->GetGuid());
    }
  }

  // snapshots
  {
    QTreeWidgetItem* pGroup = makeGroup("SNAPSHOTS");
    for (const plDocumentObject* pSnapshot : snapshots)
    {
      plString sName = pSnapshot->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
      if (sName.IsEmpty())
        sName = "<unnamed>";

      QTreeWidgetItem* pItem = new QTreeWidgetItem(pGroup, QStringList(QString::fromUtf8(sName.GetData())));
      SetGuidData(pItem, pSnapshot->GetGuid());
    }
  }

  m_pRoutingTree->expandAll();
}

void plAudioMixerAssetDocumentWindow::RebuildSnapshotList()
{
  if (m_pSnapshotList == nullptr)
    return;

  m_pSnapshotList->clear();

  plHybridArray<const plDocumentObject*, 12> snapshots;
  GatherChildrenOfProperty(GetPropertiesObject(), "Snapshots", snapshots);

  for (const plDocumentObject* pSnapshot : snapshots)
  {
    const auto& accessor = pSnapshot->GetTypeAccessor();

    plString sName = accessor.GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      sName = "<unnamed>";

    plHybridArray<const plDocumentObject*, 12> overrides;
    GatherChildrenOfProperty(pSnapshot, "Overrides", overrides);

    const plInt64 iPriority = accessor.GetValue("Priority").ConvertTo<plInt64>();
    const double fFadeIn = accessor.GetValue("FadeIn").ConvertTo<double>();
    const double fFadeOut = accessor.GetValue("FadeOut").ConvertTo<double>();

    QListWidgetItem* pItem = new QListWidgetItem(QString::asprintf("%s\n   %u overrides \xC2\xB7 in %.1fs / out %.1fs \xC2\xB7 prio %lld",
      sName.GetData(), overrides.GetCount(), fFadeIn, fFadeOut, static_cast<long long>(iPriority)));

    plStringBuilder sGuid;
    plConversionUtils::ToString(pSnapshot->GetGuid(), sGuid);
    pItem->setData(Qt::UserRole, QVariant(QString::fromUtf8(sGuid.GetData())));

    m_pSnapshotList->addItem(pItem);
  }

  if (snapshots.IsEmpty())
  {
    QListWidgetItem* pHint = new QListWidgetItem("No snapshots - '+ Snapshot' adds one.");
    pHint->setFlags(Qt::NoItemFlags);
    m_pSnapshotList->addItem(pHint);
  }
}

void plAudioMixerAssetDocumentWindow::SelectObjectByGuidString(const QVariant& itemData)
{
  if (!itemData.isValid())
    return;

  const plStringBuilder sGuid = itemData.toString().toUtf8().data();
  if (!plConversionUtils::IsStringUuid(sGuid))
    return;

  const plUuid guid = plConversionUtils::ConvertStringToUuid(sGuid);

  if (const plDocumentObject* pObject = GetDocument()->GetObjectManager()->GetObject(guid))
  {
    GetDocument()->GetSelectionManager()->SetSelection(pObject);
  }
}

void plAudioMixerAssetDocumentWindow::OnSnapshotItemClicked(QListWidgetItem* pItem)
{
  SelectObjectByGuidString(pItem->data(Qt::UserRole));
}

void plAudioMixerAssetDocumentWindow::OnRoutingItemClicked(QTreeWidgetItem* pItem, int iColumn)
{
  PL_IGNORE_UNUSED(iColumn);
  SelectObjectByGuidString(pItem->data(0, Qt::UserRole));
}

void plAudioMixerAssetDocumentWindow::OnAddBus(bool bAux)
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plHybridArray<const plDocumentObject*, 12> buses;
  GatherChildrenOfProperty(pProps, "Buses", buses);

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction(bAux ? "Add Aux Bus" : "Add Bus");

  plUuid busGuid;
  if (accessor.AddObjectByName(pProps, "Buses", -1, plGetStaticRTTI<plAudioMixerBus>(), busGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  const plDocumentObject* pBus = GetDocument()->GetObjectManager()->GetObject(busGuid);

  plStringBuilder sName;
  if (buses.IsEmpty())
  {
    sName = "Master";
  }
  else
  {
    sName.SetFormat(bAux ? "Reverb {0}" : "Bus {0}", buses.GetCount());
  }

  accessor.SetValueByName(pBus, "Name", sName.GetView()).IgnoreResult();

  if (bAux)
  {
    accessor.SetValueByName(pBus, "IsAux", true).IgnoreResult();
  }

  accessor.FinishTransaction();

  GetDocument()->GetSelectionManager()->SetSelection(pBus);
}

void plAudioMixerAssetDocumentWindow::OnAddVca()
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add VCA");

  plUuid vcaGuid;
  if (accessor.AddObjectByName(pProps, "VCAs", -1, plGetStaticRTTI<plAudioMixerVca>(), vcaGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  accessor.FinishTransaction();

  GetDocument()->GetSelectionManager()->SetSelection(GetDocument()->GetObjectManager()->GetObject(vcaGuid));
}

void plAudioMixerAssetDocumentWindow::OnAddSnapshot()
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Snapshot");

  plUuid snapshotGuid;
  if (accessor.AddObjectByName(pProps, "Snapshots", -1, plGetStaticRTTI<plAudioMixerSnapshot>(), snapshotGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  accessor.FinishTransaction();

  GetDocument()->GetSelectionManager()->SetSelection(GetDocument()->GetObjectManager()->GetObject(snapshotGuid));
}

void plAudioMixerAssetDocumentWindow::OnRemoveSelected()
{
  const plDocumentObject* pSelected = GetDocument()->GetSelectionManager()->GetCurrentObject();

  if (pSelected == nullptr || pSelected == GetPropertiesObject() || pSelected->GetParent() == GetDocument()->GetObjectManager()->GetRootObject())
  {
    ShowTemporaryStatusBarMsg("Select a bus, VCA or snapshot to remove.");
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
