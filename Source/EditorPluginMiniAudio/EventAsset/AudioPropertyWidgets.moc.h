#pragma once

#include <EditorPluginMiniAudio/EditorPluginMiniAudioDLL.h>
#include <Foundation/Reflection/Reflection.h>
#include <GuiFoundation/PropertyGrid/PropertyBaseWidget.moc.h>

class plQtDoubleSpinBox;
class QHBoxLayout;
class QVBoxLayout;

/// \brief Marks a float property as an audio distance-attenuation exponent, so the property grid
/// shows the resulting rolloff curve inline and lets it be dragged.
class plAudioRolloffCurveWidgetAttribute : public plTypeWidgetAttribute
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioRolloffCurveWidgetAttribute, plTypeWidgetAttribute);
};

/// \brief Draws the attenuation curve for the current rolloff exponent (and the sibling
/// MinDistance/MaxDistance of the same object). Dragging inside the curve edits the exponent.
class plQtAudioRolloffCurveWidget : public QWidget
{
  Q_OBJECT

public:
  plQtAudioRolloffCurveWidget(QWidget* pParent);

  void SetValues(double fRolloff, double fMinDistance, double fMaxDistance);

  virtual QSize sizeHint() const override { return QSize(120, 68); }

Q_SIGNALS:
  void RolloffDragged(double fRolloff);

protected:
  virtual void paintEvent(QPaintEvent* pEvent) override;
  virtual void mousePressEvent(QMouseEvent* pEvent) override;
  virtual void mouseMoveEvent(QMouseEvent* pEvent) override;
  virtual void mouseReleaseEvent(QMouseEvent* pEvent) override;

private:
  void UpdateFromMouse(const QPoint& pos);

  double m_fRolloff = 1.0;
  double m_fMinDistance = 1.0;
  double m_fMaxDistance = 100.0;
  bool m_bDragging = false;
};

/// \brief Property widget: spin box + inline attenuation curve.
class plQtAudioRolloffPropertyWidget : public plQtStandardPropertyWidget
{
  Q_OBJECT

public:
  plQtAudioRolloffPropertyWidget();

protected:
  virtual void OnInit() override;
  virtual void InternalSetValue(const plVariant& value) override;

private Q_SLOTS:
  void onSpinValueChanged(double fValue);
  void onCurveDragged(double fValue);

private:
  void RefreshCurve();

  QVBoxLayout* m_pLayout = nullptr;
  plQtDoubleSpinBox* m_pSpin = nullptr;
  plQtAudioRolloffCurveWidget* m_pCurve = nullptr;
  bool m_bInSetValue = false;
};
