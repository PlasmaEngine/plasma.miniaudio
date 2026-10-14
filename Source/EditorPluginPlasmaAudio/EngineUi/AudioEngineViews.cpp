#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineViews.h>
#include <EditorUi/Drawings.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>

#include <QtMath>

namespace
{
  const plHashedString s_sDrawing = plMakeHashedString("Audio.Drawing");

  /// Height of a control strip with one row, so a meter beside one lines up.
  constexpr float s_fStripHeight = 4.0f * 2.0f + plEditorUi::ControlHeight;
  constexpr float s_fMeterWidth = 140.0f;
  constexpr float s_fReadoutWidth = 64.0f;

  /// A document that only shows the view's drawing.
  void BuildDrawingDocument(plQtEngineUiWidget& ref_view, plColorGammaUB background)
  {
    plUiPanel* pRoot = plEditorUi::Panel();
    pRoot->m_Color = background;
    plEditorUi::DrawList(pRoot, "Audio.Drawing", s_sDrawing.GetView());
    ref_view.SetDocument(plEditorUi::Finish(pRoot));
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// plQtAudioSectionHeaderView

plQtAudioSectionHeaderView::plQtAudioSectionHeaderView(QWidget* pParent, plStringView sTitle, plColorGammaUB color)
  : plQtEngineUiWidget(pParent, "AudioSectionHeader")
  , m_sTitle(sTitle)
  , m_Color(color)
{
  setMinimumSize(0, 0);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void plQtAudioSectionHeaderView::SetTitle(plStringView sTitle)
{
  if (m_sTitle == sTitle)
    return;

  m_sTitle = sTitle;
  m_bDrawDirty = true;
}

void plQtAudioSectionHeaderView::SetCount(plStringView sCount)
{
  if (m_sCount == sCount)
    return;

  m_sCount = sCount;
  m_bDrawDirty = true;
}

QSize plQtAudioSectionHeaderView::sizeHint() const
{
  return QSize(200, qCeil(plAudioEngineUi::SectionHeight * GetUiScale()));
}

QSize plQtAudioSectionHeaderView::minimumSizeHint() const
{
  return QSize(40, qCeil(plAudioEngineUi::SectionHeight * GetUiScale()));
}

void plQtAudioSectionHeaderView::OnUiUpdate()
{
  if (!IsReady())
    return;

  if (m_bRebuild)
  {
    m_bRebuild = false;
    BuildDrawingDocument(*this, plEditorUi::Surface());
    m_bDrawDirty = true;
  }

  const plVec2 size = GetSurface()->GetLogicalSize();
  if (!m_bDrawDirty && size == m_vDrawnSize)
    return;

  m_bDrawDirty = false;
  m_vDrawnSize = size;
  m_Writer.Clear();

  plAudioEngineUi::SectionHeader(m_Writer, plRectFloat(0, 0, size.x, size.y), m_sTitle, m_Color, m_sCount);
  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sDrawing);
}

//////////////////////////////////////////////////////////////////////////
// plQtAudioStatusRowView

plQtAudioStatusRowView::plQtAudioStatusRowView(QWidget* pParent)
  : plQtEngineUiWidget(pParent, "AudioStatusRow")
{
  setMinimumSize(0, 0);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void plQtAudioStatusRowView::SetParts(plArrayPtr<const Part> parts)
{
  m_Parts = parts;
  m_bDrawDirty = true;
}

void plQtAudioStatusRowView::SetRight(plStringView sText)
{
  if (m_sRight == sText)
    return;

  m_sRight = sText;
  m_bDrawDirty = true;
}

QSize plQtAudioStatusRowView::sizeHint() const
{
  return QSize(200, qCeil((plEditorUi::StatusRowHeight + 1.0f) * GetUiScale()));
}

QSize plQtAudioStatusRowView::minimumSizeHint() const
{
  return QSize(40, qCeil((plEditorUi::StatusRowHeight + 1.0f) * GetUiScale()));
}

void plQtAudioStatusRowView::OnUiUpdate()
{
  using namespace plEditorUi;

  if (!IsReady())
    return;

  if (m_bRebuild)
  {
    m_bRebuild = false;
    BuildDrawingDocument(*this, Background());
    m_bDrawDirty = true;
  }

  const plVec2 size = GetSurface()->GetLogicalSize();
  if (!m_bDrawDirty && size == m_vDrawnSize)
    return;

  m_bDrawDirty = false;
  m_vDrawnSize = size;
  m_Writer.Clear();

  m_Writer.Rect(plRectFloat(0, 0, size.x, 1.0f), Line());

  const float fRightWidth = m_sRight.IsEmpty() ? 0.0f : MeasureText(m_sRight, FontSize - 1) + 8.0f;
  if (!m_sRight.IsEmpty())
    m_Writer.Text(plRectFloat(size.x - fRightWidth - 8.0f, 1.0f, fRightWidth, size.y - 1.0f), m_sRight, Dim(), FontSize - 1, plUiTextHAlign::Right);

  // value in the code face, label after it, 12 px between the counts
  float x = 8.0f;
  const float fLimit = size.x - fRightWidth - 16.0f;
  for (const Part& part : m_Parts)
  {
    const float fValue = MeasureCodeText(part.m_sValue, FontSize - 2) + 4.0f;
    const float fLabel = MeasureText(part.m_sLabel, FontSize - 1);
    if (x + fValue > fLimit)
      break;

    m_Writer.Text(plRectFloat(x, 1.0f, fValue, size.y - 1.0f), part.m_sValue, Muted(), FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    m_Writer.Text(plRectFloat(x + fValue, 1.0f, plMath::Min(fLabel + 2.0f, plMath::Max(fLimit - x - fValue, 0.0f)), size.y - 1.0f), part.m_sLabel, Dim(), FontSize - 1);
    x += fValue + fLabel + 12.0f;
  }

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sDrawing);
}

//////////////////////////////////////////////////////////////////////////
// plQtAudioMeterView

plQtAudioMeterView::plQtAudioMeterView(QWidget* pParent)
  : plQtEngineUiWidget(pParent, "AudioMeter")
{
  setMinimumSize(0, 0);
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  setToolTip("Audition output level");
}

void plQtAudioMeterView::SetPeak(float fLinearPeak)
{
  const float fPeak = plMath::Clamp(fLinearPeak, 0.0f, 1.5f);
  const float fHold = plMath::Max(fPeak, m_fPeakHold * 0.94f);

  if (fPeak == m_fPeak && fHold == m_fPeakHold)
    return;

  m_fPeak = fPeak;
  m_fPeakHold = fHold < 1e-4f ? 0.0f : fHold;
  m_bDrawDirty = true;
}

QSize plQtAudioMeterView::sizeHint() const
{
  return QSize(qCeil((s_fMeterWidth + s_fReadoutWidth + 8.0f) * GetUiScale()), qCeil(s_fStripHeight * GetUiScale()));
}

QSize plQtAudioMeterView::minimumSizeHint() const
{
  return sizeHint();
}

void plQtAudioMeterView::OnUiUpdate()
{
  using namespace plEditorUi;

  if (!IsReady())
    return;

  if (m_bRebuild)
  {
    m_bRebuild = false;
    BuildDrawingDocument(*this, Surface());
    m_bDrawDirty = true;
  }

  const plVec2 size = GetSurface()->GetLogicalSize();
  if (!m_bDrawDirty && size == m_vDrawnSize)
    return;

  m_bDrawDirty = false;
  m_vDrawnSize = size;
  m_Writer.Clear();

  const plRectFloat meter(0.0f, plMath::Round((size.y - 12.0f) * 0.5f), s_fMeterWidth, 12.0f);
  plEditorUiDraw::LevelMeter(m_Writer, meter, plEditorUiDraw::LinearToDb(m_fPeak), plEditorUiDraw::LinearToDb(m_fPeakHold), false);

  plStringBuilder sReadout;
  if (m_fPeakHold <= 0.001f)
    sReadout = "-\xE2\x88\x9E dB";
  else
    sReadout.SetFormat("{} dB", plArgF(plEditorUiDraw::LinearToDb(m_fPeakHold), 1));

  m_Writer.Text(plRectFloat(s_fMeterWidth + 4.0f, 0.0f, s_fReadoutWidth, size.y), sReadout, Muted(), FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sDrawing);
}
