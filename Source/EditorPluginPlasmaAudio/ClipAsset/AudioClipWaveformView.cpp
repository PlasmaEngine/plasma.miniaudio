#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/EditorApp/EditorApp.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAssetWindow.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipWaveformView.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorUi/Surface.h>
#include <EditorUi/TimeRuler.h>
#include <EditorUi/Widgets.h>

namespace
{
  constexpr float s_fTabRowTop = 2.0f;
  constexpr float s_fTabRowHeight = plEditorUiTabs::Height + 4.0f;
  constexpr float s_fStatsHeight = 28.0f;
  constexpr float s_fPlotInset = 8.0f;
  constexpr float s_fMarkerGrab = 6.0f;

  /// Waveform bars: width and distance from one bar to the next.
  constexpr float s_fBarWidth = 3.0f;
  constexpr float s_fBarPitch = 5.0f;

  const plHashedString s_sWaveEntry = plMakeHashedString("Clip.Wave");
  constexpr const char* s_szTabs = "Clip.Tabs";
  constexpr const char* s_szAddTab = "Clip.AddTab";

  /// Width of the dock tabs as plEditorUiTabs lays them out, so the "+" button follows the last one.
  float GetTabsWidth(plArrayPtr<const plString> labels)
  {
    float fWidth = 0.0f;
    for (const plString& sLabel : labels)
    {
      plStringBuilder sUpper(sLabel);
      sUpper.ToUpper();
      fWidth += 20.0f + plEditorUi::MeasureText(sUpper, plEditorUi::FontSize - 2);
    }
    return fWidth;
  }

  /// Two triangles for a rect, appended to a batch.
  void AddQuad(plDynamicArray<plVec2>& ref_points, float x, float y, float w, float h)
  {
    ref_points.PushBack(plVec2(x, y));
    ref_points.PushBack(plVec2(x + w, y));
    ref_points.PushBack(plVec2(x + w, y + h));
    ref_points.PushBack(plVec2(x, y));
    ref_points.PushBack(plVec2(x + w, y + h));
    ref_points.PushBack(plVec2(x, y + h));
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// plEditorUiAudioClipWaveform

plEditorUiAudioClipWaveform::plEditorUiAudioClipWaveform()
  : plEditorUiContent("AudioClipWaveform")
{
  if (IsReady())
  {
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioClipWaveform::OnPointer, this));

    m_pTabs = PL_DEFAULT_NEW(plEditorUiTabs, *GetSurface(), s_szTabs);
    m_TabSubscription = m_pTabs->m_CurrentChanged.AddEventHandler(plMakeDelegate(&plEditorUiAudioClipWaveform::OnTabChanged, this));
  }
}

plEditorUiAudioClipWaveform::~plEditorUiAudioClipWaveform()
{
  if (IsReady())
  {
    m_pTabs->m_CurrentChanged.RemoveEventHandler(m_TabSubscription);
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
  }
}

void plEditorUiAudioClipWaveform::SetPreview(const plSharedPtr<plAudioClipPreview>& pPreview)
{
  m_pPreview = pPreview;
  m_Drag = Drag::None;
  m_bDrawDirty = true;
}

void plEditorUiAudioClipWaveform::SetMessage(plStringView sMessage)
{
  if (m_sMessage == sMessage)
    return;

  m_sMessage = sMessage;
  m_bDrawDirty = true;
}

void plEditorUiAudioClipWaveform::SetPlayCursor(plInt64 iFrame)
{
  if (m_iPlayCursor == iFrame)
    return;

  m_iPlayCursor = iFrame;
  m_bDrawDirty = true;
}

void plEditorUiAudioClipWaveform::SetTabs(plArrayPtr<const plString> labels, plUInt32 uiCurrent)
{
  m_TabLabels = labels;
  m_uiCurrentTab = uiCurrent;
  m_bRebuild = true;
}

void plEditorUiAudioClipWaveform::SetStats(plArrayPtr<const Stat> stats)
{
  m_Stats = stats;
  m_bDrawDirty = true;
}

plRectFloat plEditorUiAudioClipWaveform::GetWaveRect() const
{
  const plVec2 size = IsReady() ? GetSurface()->GetLogicalSize() : plVec2(1.0f);
  const float y = s_fTabRowTop + s_fTabRowHeight;
  return plRectFloat(0.0f, y, size.x, plMath::Max(size.y - y - s_fStatsHeight, 1.0f));
}

plRectFloat plEditorUiAudioClipWaveform::GetPlotRect() const
{
  const plRectFloat wave = GetWaveRect();
  const float y = wave.y + plEditorUiTimeRuler::Height + 4.0f;
  return plRectFloat(wave.x + s_fPlotInset, y, plMath::Max(wave.width - 2.0f * s_fPlotInset, 1.0f), plMath::Max(wave.y + wave.height - 6.0f - y, 1.0f));
}

float plEditorUiAudioClipWaveform::FrameToX(plUInt64 uiFrame) const
{
  const plRectFloat plot = GetPlotRect();
  const plUInt64 uiFrames = m_pPreview != nullptr ? m_pPreview->m_Header.m_uiFrames : 0;
  if (uiFrames == 0)
    return plot.x;

  return plot.x + static_cast<float>(static_cast<double>(uiFrame) / uiFrames * plot.width);
}

plUInt64 plEditorUiAudioClipWaveform::XToFrame(float x) const
{
  const plRectFloat plot = GetPlotRect();
  const plUInt64 uiFrames = m_pPreview != nullptr ? m_pPreview->m_Header.m_uiFrames : 0;
  if (uiFrames == 0)
    return 0;

  const double fRel = plMath::Clamp(static_cast<double>(x - plot.x) / plot.width, 0.0, 1.0);
  return static_cast<plUInt64>(fRel * uiFrames + 0.5);
}

bool plEditorUiAudioClipWaveform::GetLoop(plUInt64& out_uiStart, plUInt64& out_uiEnd) const
{
  if (m_Drag != Drag::None)
  {
    out_uiStart = m_uiDragStart;
    out_uiEnd = m_uiDragEnd;
    return out_uiEnd > out_uiStart;
  }

  if (m_pPreview == nullptr || !m_pPreview->m_bSuccess || !m_pPreview->m_Header.m_bHasLoop)
    return false;

  out_uiStart = m_pPreview->m_Header.m_uiLoopStart;
  out_uiEnd = m_pPreview->m_Header.m_uiLoopEnd;
  return true;
}

void plEditorUiAudioClipWaveform::UpdateBars(float fPlotWidth)
{
  if (m_pPreview == nullptr || !m_pPreview->m_bSuccess)
  {
    m_Bars.Clear();
    m_uiBarCount = 0;
    m_pBarsPreview = nullptr;
    return;
  }

  // keyed on the pointer and the build hash, so a new preview allocated at a freed one's address is not mistaken for it
  if (m_pBarsPreview == m_pPreview.Borrow() && m_uiBarsHash == m_pPreview->m_uiHash && m_fBarsWidth == fPlotWidth)
    return;

  m_pBarsPreview = m_pPreview.Borrow();
  m_uiBarsHash = m_pPreview->m_uiHash;
  m_fBarsWidth = fPlotWidth;

  const plAudioClipHeader& header = m_pPreview->m_Header;
  const plUInt32 uiChannels = header.m_uiChannels;
  const plUInt64 uiFrames = header.m_uiFrames;

  m_uiBarCount = fPlotWidth > 0.0f ? static_cast<plUInt32>((fPlotWidth + s_fBarPitch - s_fBarWidth) / s_fBarPitch) : 0;
  m_Bars.SetCount(m_uiBarCount * uiChannels);

  if (m_uiBarCount == 0 || uiChannels == 0 || uiFrames == 0)
    return;

  const plDynamicArray<plInt16>& pcm = m_pPreview->m_Pcm;
  const bool bHasPcm = pcm.GetCount() >= uiFrames * uiChannels;
  const plUInt32 uiBuckets = header.m_uiPeakBuckets;

  for (plUInt32 b = 0; b < m_uiBarCount; ++b)
  {
    for (plUInt32 c = 0; c < uiChannels; ++c)
    {
      float fPeak = 0.0f;

      if (bHasPcm)
      {
        const plUInt64 uiBegin = uiFrames * b / m_uiBarCount;
        const plUInt64 uiEnd = plMath::Max(uiFrames * (b + 1) / m_uiBarCount, uiBegin + 1);

        plInt32 iPeak = 0;
        for (plUInt64 f = uiBegin; f < uiEnd && f < uiFrames; ++f)
        {
          iPeak = plMath::Max(iPeak, plMath::Abs(static_cast<plInt32>(pcm[static_cast<plUInt32>(f * uiChannels + c)])));
        }
        fPeak = iPeak / 32768.0f;
      }
      else if (uiBuckets > 0 && header.m_Peaks.GetCount() >= uiBuckets * uiChannels * 2)
      {
        // no decoded audio: the stored min/max peaks
        const plUInt32 uiB0 = static_cast<plUInt32>(static_cast<plUInt64>(uiBuckets) * b / m_uiBarCount);
        const plUInt32 uiB1 = plMath::Max(uiB0 + 1, static_cast<plUInt32>(static_cast<plUInt64>(uiBuckets) * (b + 1) / m_uiBarCount));

        plInt32 iPeak = 0;
        for (plUInt32 k = uiB0; k < uiB1 && k < uiBuckets; ++k)
        {
          iPeak = plMath::Max(iPeak, plMath::Abs(static_cast<plInt32>(header.m_Peaks[(k * uiChannels + c) * 2 + 0])));
          iPeak = plMath::Max(iPeak, plMath::Abs(static_cast<plInt32>(header.m_Peaks[(k * uiChannels + c) * 2 + 1])));
        }
        fPeak = iPeak / 127.0f;
      }

      m_Bars[c * m_uiBarCount + b] = plMath::Clamp(fPeak, 0.0f, 1.0f);
    }
  }
}

void plEditorUiAudioClipWaveform::OnUiUpdate()
{
  if (!IsReady())
    return;

  if (GetSurface()->GetLogicalSize() != m_vBuiltSize)
    m_bRebuild = true;

  if (m_bRebuild)
    BuildDocument();

  if (m_bDrawDirty)
    Draw();
}

void plEditorUiAudioClipWaveform::BuildDocument()
{
  using namespace plEditorUi;

  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();
  plBlackboard& board = GetSurface()->GetBlackboard();

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = Surface();

  // everything painted lies under the tabs and the "+" button
  DrawList(pRoot, "Clip.Wave", s_sWaveEntry.GetView());

  plHybridArray<plEditorUiTab, 8> tabs;
  for (const plString& sLabel : m_TabLabels)
    tabs.ExpandAndGetRef().m_sLabel = sLabel;

  const float fTabsWidth = plMath::Min(GetTabsWidth(m_TabLabels), m_vBuiltSize.x - 40.0f);

  plUiPanel* pTabHost = Panel(pRoot);
  plAudioEngineUi::Place(pTabHost, plRectFloat(6.0f, s_fTabRowTop, plMath::Max(fTabsWidth, 1.0f), plEditorUiTabs::Height));
  pTabHost->m_Color = plColorGammaUB(0, 0, 0, 0);

  m_pTabs->SetTabs(tabs);
  m_pTabs->SetCurrent(plMath::Min(m_uiCurrentTab, tabs.GetCount() > 0 ? tabs.GetCount() - 1 : 0));
  m_pTabs->Build(pTabHost, plMath::Max(fTabsWidth, 1.0f));

  for (plUInt32 i = 0; i < tabs.GetCount(); ++i)
  {
    plStringBuilder sName;
    sName.SetFormat("{}.tab.{}", s_szTabs, i);
    SetToolTip(board, sName, "Default settings and per platform profile overrides. Right-click an override to remove it.");
  }

  plUiButton* pAdd = IconButton(pRoot, Icon::Add, s_szAddTab);
  plAudioEngineUi::Place(pAdd, plRectFloat(6.0f + fTabsWidth + 4.0f, s_fTabRowTop + 3.0f, 20.0f, 20.0f));
  SetToolTip(board, s_szAddTab, "Adds a codec and quality override for a platform profile");

  SetDocument(Finish(pRoot));
  m_bDrawDirty = true;
}

void plEditorUiAudioClipWaveform::Draw()
{
  using namespace plEditorUi;

  m_bDrawDirty = false;
  m_Writer.Clear();

  const plVec2 size = GetSurface()->GetLogicalSize();
  const plRectFloat wave = GetWaveRect();
  const plRectFloat plot = GetPlotRect();

  // hairline under the tab row
  m_Writer.Rect(plRectFloat(0.0f, wave.y - 1.0f, size.x, 1.0f), Line());
  m_Writer.Rect(wave, Background());

  const bool bValid = m_pPreview != nullptr && m_pPreview->m_bSuccess && m_pPreview->m_Header.m_uiFrames > 0 && m_pPreview->m_Header.m_uiSampleRate > 0;
  const plRectFloat rulerRect(wave.x, wave.y, wave.width, plEditorUiTimeRuler::Height);

  if (!bValid)
  {
    // the ruler stays, without a scale
    m_Writer.Rect(rulerRect, Surface());
    m_Writer.Rect(plRectFloat(rulerRect.x, rulerRect.y + rulerRect.height - 1.0f, rulerRect.width, 1.0f), Line());

    plStringView sText = (m_pPreview != nullptr && !m_pPreview->m_sError.IsEmpty()) ? m_pPreview->m_sError.GetView() : m_sMessage.GetView();
    const float cy = plot.y + plot.height * 0.5f;
    plAudioEngineUi::Icon(m_Writer, plRectFloat(plot.x + plot.width * 0.5f - 11.0f, cy - 30.0f, 22.0f, 22.0f), IconPath(Icon::Volume), Dim());
    m_Writer.Text(plRectFloat(plot.x, cy, plot.width, 20.0f), sText, Muted(), FontSize - 1, plUiTextHAlign::Center);
  }
  else
  {
    const plAudioClipHeader& header = m_pPreview->m_Header;
    const plUInt32 uiChannels = header.m_uiChannels;
    const double fDuration = static_cast<double>(header.m_uiFrames) / header.m_uiSampleRate;

    // the shared time ruler; the clip spans the plot
    plEditorUiTimeView view;
    view.m_fPixelsPerSecond = plot.width / fDuration;
    view.m_fStart = -(plot.x - wave.x) / view.m_fPixelsPerSecond;

    plEditorUiTimeRulerOptions options;
    options.m_fDuration = fDuration;
    options.m_fPlayhead = m_iPlayCursor >= 0 ? static_cast<double>(static_cast<plUInt64>(m_iPlayCursor) % header.m_uiFrames) / header.m_uiSampleRate : -1.0;
    plEditorUiTimeRuler::Draw(m_Writer, rulerRect, view, options);

    // loop region: a neutral fill under the bars (an opaque blend, translucent fills blend in linear space)
    plUInt64 uiLoopStart = 0;
    plUInt64 uiLoopEnd = 0;
    const bool bHasLoop = GetLoop(uiLoopStart, uiLoopEnd);
    const float fCanvasTop = rulerRect.y + rulerRect.height;
    const float fCanvasBottom = wave.y + wave.height;

    if (bHasLoop)
    {
      const float x0 = FrameToX(uiLoopStart);
      const float x1 = FrameToX(uiLoopEnd);
      m_Writer.Rect(plRectFloat(x0, fCanvasTop, plMath::Max(x1 - x0, 1.0f), fCanvasBottom - fCanvasTop), plAudioEngineUi::Mix(Background(), Foreground(), 0.06f));
    }

    // lanes: a centre line each, a hairline between them, bars in the Audio colour
    UpdateBars(plot.width);

    const float fLaneHeight = plot.height / plMath::Max<plUInt32>(uiChannels, 1);
    const plColorGammaUB grid = plAudioEngineUi::Mix(Background(), Raised(), 0.6f);

    plDynamicArray<plVec2> bars;
    bars.Reserve(m_uiBarCount * uiChannels * 6);

    for (plUInt32 c = 0; c < uiChannels; ++c)
    {
      const float fTop = plot.y + c * fLaneHeight;
      const float fMid = fTop + fLaneHeight * 0.5f;
      const float fHalf = plMath::Max(fLaneHeight * 0.5f - 6.0f, 1.0f);

      m_Writer.Rect(plRectFloat(wave.x, plMath::Round(fMid), wave.width, 1.0f), grid);

      if (c > 0)
        m_Writer.Rect(plRectFloat(wave.x, plMath::Round(fTop), wave.width, 1.0f), Line());

      if (m_Bars.GetCount() < m_uiBarCount * uiChannels)
        continue;

      for (plUInt32 b = 0; b < m_uiBarCount; ++b)
      {
        const float fBarHalf = plMath::Max(0.5f, m_Bars[c * m_uiBarCount + b] * fHalf);
        AddQuad(bars, plot.x + b * s_fBarPitch, fMid - fBarHalf, s_fBarWidth, fBarHalf * 2.0f);
      }
    }

    if (!bars.IsEmpty())
      m_Writer.Triangles(bars, plAudioEngineUi::AudioColor());

    // loop markers: grey lines with flags pointing inwards
    if (bHasLoop)
    {
      for (int i = 0; i < 2; ++i)
      {
        const float x = FrameToX(i == 0 ? uiLoopStart : uiLoopEnd);
        const float fDir = i == 0 ? 1.0f : -1.0f;
        m_Writer.Rect(plRectFloat(x - 1.0f, fCanvasTop, 2.0f, fCanvasBottom - fCanvasTop), Muted());

        const plVec2 flag[] = {plVec2(x, fCanvasTop), plVec2(x + 8.0f * fDir, fCanvasTop), plVec2(x + 8.0f * fDir, fCanvasTop + 6.0f), plVec2(x, fCanvasTop), plVec2(x + 8.0f * fDir, fCanvasTop + 6.0f), plVec2(x, fCanvasTop + 10.0f)};
        m_Writer.Triangles(flag, Muted());
      }
    }

    // play cursor in the accent colour
    if (m_iPlayCursor >= 0)
    {
      const float x = FrameToX(static_cast<plUInt64>(m_iPlayCursor) % header.m_uiFrames);
      m_Writer.Rect(plRectFloat(plMath::Round(x), rulerRect.y + rulerRect.height, 1.0f, wave.y + wave.height - rulerRect.y - rulerRect.height), Accent());
    }

    // after the clip's end
    const float fEndX = plot.x + plot.width;
    if (fEndX < wave.x + wave.width)
      m_Writer.Rect(plRectFloat(fEndX, rulerRect.y + rulerRect.height, wave.x + wave.width - fEndX, wave.height - rulerRect.height), plColorGammaUB(0, 0, 0, 97));
  }

  DrawStats();

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sWaveEntry);
}

void plEditorUiAudioClipWaveform::DrawStats()
{
  using namespace plEditorUi;

  const plVec2 size = GetSurface()->GetLogicalSize();
  const float y = size.y - s_fStatsHeight;

  m_Writer.Rect(plRectFloat(0.0f, y, size.x, s_fStatsHeight), Surface());
  m_Writer.Rect(plRectFloat(0.0f, y, size.x, 1.0f), Line());

  float x = 8.0f;
  for (plUInt32 i = 0; i < m_Stats.GetCount(); ++i)
  {
    const Stat& stat = m_Stats[i];

    if (i > 0)
    {
      m_Writer.Text(plRectFloat(x, y, 12.0f, s_fStatsHeight), "|", Line(), FontSize - 1, plUiTextHAlign::Center);
      x += 12.0f;
    }

    const float fLabel = MeasureText(stat.m_sLabel, FontSize - 1) + 5.0f;
    m_Writer.Text(plRectFloat(x, y, fLabel, s_fStatsHeight), stat.m_sLabel, Dim(), FontSize - 1);
    x += fLabel;

    const float fValue = MeasureCodeText(stat.m_sValue, FontSize - 2) + 2.0f;
    m_Writer.Text(plRectFloat(x, y, plMath::Min(fValue, plMath::Max(size.x - x - 4.0f, 0.0f)), s_fStatsHeight), stat.m_sValue, Foreground(), FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    x += fValue + 4.0f;
  }
}

plString plEditorUiAudioClipWaveform::GetToolTipAt(const plVec2& vLogical) const
{
  if (!IsReady())
    return {};

  const plVec2 size = GetSurface()->GetLogicalSize();
  if (vLogical.y < size.y - s_fStatsHeight)
    return plEditorUiContent::GetToolTipAt(vLogical);

  // the stat under the pointer, laid out as DrawStats does
  float x = 8.0f;
  for (plUInt32 i = 0; i < m_Stats.GetCount(); ++i)
  {
    if (i > 0)
      x += 12.0f;

    const float fWidth = plEditorUi::MeasureText(m_Stats[i].m_sLabel, plEditorUi::FontSize - 1) + 5.0f + plEditorUi::MeasureCodeText(m_Stats[i].m_sValue, plEditorUi::FontSize - 2) + 6.0f;
    if (vLogical.x >= x && vLogical.x < x + fWidth)
      return m_Stats[i].m_sToolTip;

    x += fWidth;
  }

  return {};
}

void plEditorUiAudioClipWaveform::OnTabChanged(plUInt32 uiTab)
{
  m_uiCurrentTab = uiTab;

  if (m_OnTabPicked.IsValid())
    m_OnTabPicked(uiTab);
}

void plEditorUiAudioClipWaveform::OnUiEvent(const plMsgUiEvent& event)
{
  if (event.m_sIdentifier.GetView() == s_szAddTab && m_OnAddTab.IsValid())
  {
    const plRectFloat wave = GetWaveRect();
    m_OnAddTab(plVec2(6.0f + GetTabsWidth(m_TabLabels) + 4.0f, wave.y));
  }
}

void plEditorUiAudioClipWaveform::OnPointer(const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;
  using Button = plEditorUiPointerEvent::Button;

  // right-click on an override tab opens its menu
  if (e.m_Type == Type::Press && e.m_Button == Button::Right)
  {
    const plStringBuilder sPrefix(s_szTabs, ".tab.");
    if (e.m_sElement.StartsWith(sPrefix))
    {
      plStringView sIndex = e.m_sElement.GetView();
      sIndex.Shrink(sPrefix.GetCharacterCount(), 0);

      plUInt32 uiTab = 0;
      if (plConversionUtils::StringToUInt(sIndex, uiTab).Succeeded() && m_OnTabMenu.IsValid())
        m_OnTabMenu(uiTab, e.m_vPosition);
    }
    return;
  }

  const bool bValid = m_pPreview != nullptr && m_pPreview->m_bSuccess && m_pPreview->m_Header.m_uiFrames > 0;
  const plRectFloat wave = GetWaveRect();
  const float fTop = wave.y + plEditorUiTimeRuler::Height;

  switch (e.m_Type)
  {
    case Type::Move:
    {
      if (m_Drag != Drag::None)
        break;

      plUInt64 uiStart = 0;
      plUInt64 uiEnd = 0;
      const bool bInWave = bValid && e.m_sElement.IsEmpty() && wave.Contains(e.m_vPosition) && e.m_vPosition.y >= fTop;
      const bool bOverMarker = bInWave && GetLoop(uiStart, uiEnd) && (plMath::Abs(e.m_vPosition.x - FrameToX(uiStart)) <= s_fMarkerGrab || plMath::Abs(e.m_vPosition.x - FrameToX(uiEnd)) <= s_fMarkerGrab);

      if (bOverMarker)
        SetCursor(plEditorPlatformCursor::SizeWE);
      else if (bInWave)
        SetCursor(plEditorPlatformCursor::IBeam);
      else
        SetCursor(plEditorPlatformCursor::Arrow);
      return;
    }

    case Type::Press:
    {
      if (e.m_Button != Button::Left || !bValid || !e.m_sElement.IsEmpty() || !wave.Contains(e.m_vPosition) || e.m_vPosition.y < fTop)
        return;

      const float x = e.m_vPosition.x;
      plUInt64 uiStart = 0;
      plUInt64 uiEnd = 0;

      if (GetLoop(uiStart, uiEnd))
      {
        if (plMath::Abs(x - FrameToX(uiStart)) <= s_fMarkerGrab)
        {
          m_Drag = Drag::LoopStart;
          m_uiDragStart = uiStart;
          m_uiDragEnd = uiEnd;
          return;
        }

        if (plMath::Abs(x - FrameToX(uiEnd)) <= s_fMarkerGrab)
        {
          m_Drag = Drag::LoopEnd;
          m_uiDragStart = uiStart;
          m_uiDragEnd = uiEnd;
          return;
        }
      }

      m_Drag = Drag::Create;
      m_uiDragAnchor = XToFrame(x);
      m_uiDragStart = m_uiDragAnchor;
      m_uiDragEnd = m_uiDragAnchor;
      m_bDrawDirty = true;
      return;
    }

    case Type::DragStart:
    case Type::DragMove:
      break;

    case Type::Release:
    case Type::Drop:
    {
      if (m_Drag == Drag::None)
        return;

      const Drag drag = m_Drag;
      m_Drag = Drag::None;
      m_bDrawDirty = true;

      // a click without dragging does not create a loop
      if (drag == Drag::Create && FrameToX(m_uiDragEnd) - FrameToX(m_uiDragStart) < 3.0f)
        return;

      if (m_uiDragEnd > m_uiDragStart && m_OnLoopChanged.IsValid())
        m_OnLoopChanged(m_uiDragStart, m_uiDragEnd);
      return;
    }

    case Type::DragCancel:
      m_Drag = Drag::None;
      m_bDrawDirty = true;
      return;

    default:
      return;
  }

  // a drag in progress follows the pointer
  if (m_Drag == Drag::None)
    return;

  const plUInt64 uiFrame = XToFrame(e.m_vPosition.x);
  switch (m_Drag)
  {
    case Drag::LoopStart:
      m_uiDragStart = plMath::Min(uiFrame, m_uiDragEnd > 0 ? m_uiDragEnd - 1 : 0);
      break;
    case Drag::LoopEnd:
      m_uiDragEnd = plMath::Max(uiFrame, m_uiDragStart + 1);
      break;
    case Drag::Create:
      m_uiDragStart = plMath::Min(uiFrame, m_uiDragAnchor);
      m_uiDragEnd = plMath::Max(uiFrame, m_uiDragAnchor);
      break;
    default:
      break;
  }

  m_bDrawDirty = true;
}

//////////////////////////////////////////////////////////////////////////
// plEditorUiAudioClipUsedBy

/// The referencing assets as a flat list; an empty list shows one dim row.
class plEditorUiAudioClipUsedBy::Adapter : public plEditorUiTreeAdapter
{
public:
  explicit Adapter(const plDynamicArray<Entry>& entries)
    : m_Entries(entries)
  {
  }

  void GetChildren(plUInt64 uiParent, plDynamicArray<plUInt64>& out_children) const override
  {
    out_children.Clear();
    if (uiParent != 0)
      return;

    const plUInt32 uiCount = plMath::Max<plUInt32>(m_Entries.GetCount(), 1);
    for (plUInt32 i = 0; i < uiCount; ++i)
      out_children.PushBack(i + 1);
  }

  plUInt64 GetParent(plUInt64 uiItem) const override
  {
    PL_IGNORE_UNUSED(uiItem);
    return 0;
  }

  void GetRow(plUInt64 uiItem, plEditorUiTreeRow& out_row) const override
  {
    if (m_Entries.IsEmpty())
    {
      out_row.m_sText = "No asset references this clip";
      out_row.m_TextStyle = plEditorUiTreeTextStyle::Dimmed;
      return;
    }

    const Entry& entry = m_Entries[static_cast<plUInt32>(uiItem - 1)];
    const bool bBank = plStringView(entry.m_sPath).HasExtension("plAudioBankAsset");

    out_row.m_sText = entry.m_sPath;
    out_row.m_sTooltip = "Assets that reference this clip. Double-click to open one.";
    out_row.m_sIcon = plEditorUi::MonochromeIconPath(bBank ? plAudioEngineUi::IconBank : plAudioEngineUi::IconEvent);
    out_row.m_IconColor = plAudioEngineUi::AudioColor();
  }

  void Activate(plUInt64 uiItem) override
  {
    if (m_Entries.IsEmpty() || uiItem == 0 || uiItem > m_Entries.GetCount())
      return;

    plEditorApp::GetSingleton()->OpenDocument(m_Entries[static_cast<plUInt32>(uiItem - 1)].m_sAbsPath, plDocumentFlags::RequestWindow | plDocumentFlags::AddToRecentFilesList);
  }

private:
  const plDynamicArray<Entry>& m_Entries;
};

namespace
{
  const plHashedString s_sUsedByCount = plMakeHashedString("Clip.UsedBy.Count");
}

plEditorUiAudioClipUsedBy::plEditorUiAudioClipUsedBy()
  : plEditorUiContent("AudioClipUsedBy")
{
  m_pAdapter = PL_DEFAULT_NEW(Adapter, m_Entries);

  if (IsReady())
    m_pTree = PL_DEFAULT_NEW(plEditorUiTree, *GetSurface(), "Clip.UsedBy", *m_pAdapter);
}

plEditorUiAudioClipUsedBy::~plEditorUiAudioClipUsedBy()
{
  m_pTree.Clear();
}

void plEditorUiAudioClipUsedBy::SetEntries(plArrayPtr<const Entry> entries)
{
  m_Entries = entries;

  if (IsReady())
  {
    plStringBuilder sCount;
    sCount.SetFormat("{}", m_Entries.GetCount());
    GetSurface()->GetBlackboard().SetEntryValue(s_sUsedByCount, plString(sCount));

    if (!m_bRebuild)
      m_pTree->Refresh();
  }
}

void plEditorUiAudioClipUsedBy::OnUiUpdate()
{
  if (IsReady() && m_bRebuild)
    BuildDocument();
}

void plEditorUiAudioClipUsedBy::BuildDocument()
{
  using namespace plEditorUi;

  m_bRebuild = false;

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = Surface();

  SectionHeader(pRoot, "Used by", plAudioEngineUi::AudioColor(), s_sUsedByCount.GetView());

  plUiPanel* pList = Panel(pRoot);
  Fill(pList, plVec4(0, SectionHeaderHeight + 2.0f, 0, 0));
  pList->m_Color = Surface();
  m_pTree->Build(pList);

  SetDocument(Finish(pRoot));
  m_pTree->Refresh();
}
