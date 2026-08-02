#include <EditorPluginMiniAudio/EditorPluginMiniAudioPCH.h>

#include <EditorPluginMiniAudio/EventAsset/AudioPropertyWidgets.moc.h>

#include <Foundation/Math/ColorScheme.h>
#include <GuiFoundation/Widgets/DoubleSpinBox.moc.h>

#include <QBoxLayout>
#include <QMouseEvent>
#include <QPainter>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioRolloffCurveWidgetAttribute, 1, plRTTIDefaultAllocator<plAudioRolloffCurveWidgetAttribute>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

// //////////////////////////////////////////////////////////////////////
// plQtAudioRolloffCurveWidget
// //////////////////////////////////////////////////////////////////////

plQtAudioRolloffCurveWidget::plQtAudioRolloffCurveWidget(QWidget* pParent)
  : QWidget(pParent)
{
  setMinimumHeight(60);
  setCursor(Qt::SizeVerCursor);
  setToolTip("Distance attenuation. Drag up/down to change the rolloff exponent.");
}

void plQtAudioRolloffCurveWidget::SetValues(double fRolloff, double fMinDistance, double fMaxDistance)
{
  m_fRolloff = fRolloff;
  m_fMinDistance = fMinDistance;
  m_fMaxDistance = plMath::Max(fMaxDistance, fMinDistance + 0.01);
  update();
}

void plQtAudioRolloffCurveWidget::paintEvent(QPaintEvent* pEvent)
{
  PL_IGNORE_UNUSED(pEvent);

  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  const QRect area = rect().adjusted(0, 0, -1, -1);
  const QColor accent = plToQtColor(plColorGammaUB(plColorScheme::GetCategoryColor("Audio", plColorScheme::CategoryColorUsage::BorderColor)));

  p.setPen(QPen(QColor(60, 60, 60), 1));
  p.setBrush(QColor(20, 20, 20));
  p.drawRect(area);

  const QRectF plot = QRectF(area).adjusted(6, 6, -6, -6);

  // grid
  p.setPen(QPen(QColor(45, 45, 45), 1, Qt::DotLine));
  for (int i = 1; i < 4; ++i)
  {
    const double fY = plot.top() + plot.height() * i / 4.0;
    p.drawLine(QPointF(plot.left(), fY), QPointF(plot.right(), fY));
  }

  // miniaudio's inverse distance model: gain = min / (min + rolloff * (d - min))
  QPolygonF curve;
  const int iSteps = 48;
  for (int i = 0; i <= iSteps; ++i)
  {
    const double fT = static_cast<double>(i) / iSteps;
    const double fDistance = m_fMinDistance + fT * (m_fMaxDistance - m_fMinDistance);
    const double fGain = plMath::Clamp(m_fMinDistance / plMath::Max(m_fMinDistance + m_fRolloff * (fDistance - m_fMinDistance), 0.0001), 0.0, 1.0);

    curve << QPointF(plot.left() + fT * plot.width(), plot.bottom() - fGain * plot.height());
  }

  p.setPen(QPen(accent, 2));
  p.drawPolyline(curve);

  // labels
  QFont smallFont = font();
  smallFont.setPointSizeF(7.0);
  p.setFont(smallFont);
  p.setPen(QColor(142, 142, 142));
  p.drawText(QRectF(plot.left(), plot.bottom() - 12, plot.width() * 0.5, 12), Qt::AlignLeft | Qt::AlignBottom, QString::asprintf("%.0fm", m_fMinDistance));
  p.drawText(QRectF(plot.center().x(), plot.bottom() - 12, plot.width() * 0.5, 12), Qt::AlignRight | Qt::AlignBottom, QString::asprintf("%.0fm", m_fMaxDistance));
  p.drawText(QRectF(plot.left(), plot.top(), plot.width(), 12), Qt::AlignRight | Qt::AlignTop, QString::asprintf("rolloff %.2f", m_fRolloff));
}

void plQtAudioRolloffCurveWidget::UpdateFromMouse(const QPoint& pos)
{
  // dragging up = shallower curve (smaller exponent), down = steeper
  const double fT = plMath::Clamp(static_cast<double>(pos.y()) / plMath::Max(1, height()), 0.0, 1.0);
  const double fRolloff = plMath::Lerp(0.05, 8.0, fT);

  m_fRolloff = fRolloff;
  update();

  Q_EMIT RolloffDragged(fRolloff);
}

void plQtAudioRolloffCurveWidget::mousePressEvent(QMouseEvent* pEvent)
{
  if (pEvent->button() == Qt::LeftButton)
  {
    m_bDragging = true;
    UpdateFromMouse(pEvent->pos());
  }
}

void plQtAudioRolloffCurveWidget::mouseMoveEvent(QMouseEvent* pEvent)
{
  if (m_bDragging)
  {
    UpdateFromMouse(pEvent->pos());
  }
}

void plQtAudioRolloffCurveWidget::mouseReleaseEvent(QMouseEvent* pEvent)
{
  PL_IGNORE_UNUSED(pEvent);
  m_bDragging = false;
}

// //////////////////////////////////////////////////////////////////////
// plQtAudioRolloffPropertyWidget
// //////////////////////////////////////////////////////////////////////

plQtAudioRolloffPropertyWidget::plQtAudioRolloffPropertyWidget()
{
  m_pLayout = new QVBoxLayout(this);
  m_pLayout->setContentsMargins(0, 0, 0, 0);
  m_pLayout->setSpacing(2);
  setLayout(m_pLayout);

  m_pSpin = new plQtDoubleSpinBox(this);
  m_pSpin->setMinimum(0.001);
  m_pSpin->setMaximum(1000.0);
  m_pSpin->setSingleStep(0.1);
  m_pSpin->setDecimals(3);
  m_pSpin->setAccelerated(true);
  m_pLayout->addWidget(m_pSpin);

  m_pCurve = new plQtAudioRolloffCurveWidget(this);
  m_pLayout->addWidget(m_pCurve);

  connect(m_pSpin, &QDoubleSpinBox::editingFinished, this, [this]()
    { onSpinValueChanged(m_pSpin->value()); });
  connect(m_pCurve, &plQtAudioRolloffCurveWidget::RolloffDragged, this, &plQtAudioRolloffPropertyWidget::onCurveDragged);
}

void plQtAudioRolloffPropertyWidget::OnInit()
{
  RefreshCurve();
}

void plQtAudioRolloffPropertyWidget::InternalSetValue(const plVariant& value)
{
  m_bInSetValue = true;

  const double fValue = value.IsValid() ? value.ConvertTo<double>() : 1.0;
  m_pSpin->setValue(fValue);

  m_bInSetValue = false;

  RefreshCurve();
}

void plQtAudioRolloffPropertyWidget::RefreshCurve()
{
  if (m_pCurve == nullptr)
    return;

  double fMinDistance = 1.0;
  double fMaxDistance = 100.0;

  // read the sibling distances of the same object so the curve shows the real range
  if (!m_Items.IsEmpty() && m_Items[0].m_pObject != nullptr)
  {
    const auto& accessor = m_Items[0].m_pObject->GetTypeAccessor();
    fMinDistance = accessor.GetValue("MinDistance").ConvertTo<double>();
    fMaxDistance = accessor.GetValue("MaxDistance").ConvertTo<double>();
  }

  m_pCurve->SetValues(m_pSpin->value(), fMinDistance, fMaxDistance);
}

void plQtAudioRolloffPropertyWidget::onSpinValueChanged(double fValue)
{
  if (m_bInSetValue)
    return;

  BroadcastValueChanged(static_cast<float>(fValue));
  RefreshCurve();
}

void plQtAudioRolloffPropertyWidget::onCurveDragged(double fValue)
{
  if (m_bInSetValue)
    return;

  m_bInSetValue = true;
  m_pSpin->setValue(fValue);
  m_bInSetValue = false;

  BroadcastValueChanged(static_cast<float>(fValue));
}
