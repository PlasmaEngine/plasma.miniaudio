#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/Platform/AudioPlatformsView.h>
#include <EditorUi/Menu.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <GameUiPlugin/Components/UiCanvasComponent.h>

namespace
{
  const plHashedString s_sDrawingEntry = plMakeHashedString("Platforms.Drawing");

  constexpr float s_fPad = 12.0f;
  constexpr float s_fTabRow = 32.0f;
  constexpr float s_fRowPitch = 30.0f;
  constexpr float s_fLabelWidth = 110.0f;
  constexpr float s_fSideWidth = 290.0f;
  constexpr float s_fMaxControl = 420.0f;
  constexpr plUInt32 s_uiMaxLog = 24;

  constexpr float s_fMaxPeriodMs = 40.0f;
  constexpr float s_fMaxFadeMs = 500.0f;

  // rows of the settings column
  constexpr plUInt32 s_uiRowOutput = 0;
  constexpr plUInt32 s_uiRowBackend = 1;
  constexpr plUInt32 s_uiRowSpeakers = 2;
  constexpr plUInt32 s_uiRowRate = 3;
  constexpr plUInt32 s_uiRowPeriod = 4;
  constexpr plUInt32 s_uiRowDecoding = 5;
  constexpr plUInt32 s_uiRowOpus = 6;
  constexpr plUInt32 s_uiRowBars = 7; ///< three rows of bars
  constexpr plUInt32 s_uiRowSuspend = 10;
  constexpr plUInt32 s_uiRowPolicy = 11;
  constexpr plUInt32 s_uiRowFade = 12;
  constexpr plUInt32 s_uiRowSimulate = 13;

  constexpr const char* s_szSpeakers[] = {"Device", "Stereo", "5.1", "7.1"};
  constexpr plUInt32 s_uiRates[] = {0, 44100, 48000, 96000};
  constexpr const char* s_szPolicies[] = {"Pause everything, resume where it was", "Keep playing (background audio)", "Stop everything"};

  plString IndexedName(const char* szPrefix, plUInt32 uiIndex)
  {
    plStringBuilder s;
    s.SetFormat("{}.{}", szPrefix, uiIndex);
    return s;
  }

  plString RateText(plUInt32 uiRate)
  {
    if (uiRate == 0)
      return "Device";

    plStringBuilder s;
    s.SetFormat("{} Hz", uiRate);
    return s;
  }
} // namespace

plEditorUiAudioPlatformsView::plEditorUiAudioPlatformsView()
  : plEditorUiContent("AudioPlatforms")
{
  if (IsReady())
  {
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioPlatformsView::OnPointer, this));
  }
}

plEditorUiAudioPlatformsView::~plEditorUiAudioPlatformsView()
{
  if (IsReady())
  {
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
  }
}

void plEditorUiAudioPlatformsView::SetProfiles(plArrayPtr<const plString> names, plUInt32 uiShown, plUInt32 uiActive)
{
  if (m_Profiles == names && m_uiShownProfile == uiShown && m_uiActiveProfile == uiActive)
    return;

  m_Profiles = names;
  m_uiShownProfile = uiShown;
  m_uiActiveProfile = uiActive;
  m_bRebuild = true;
  RequestRedraw();
}

void plEditorUiAudioPlatformsView::SetConfig(const plPlasmaAudioPlatformConfig& config)
{
  m_Config.m_sBackend = config.m_sBackend;
  m_Config.m_eSpeakers = config.m_eSpeakers;
  m_Config.m_uiSampleRate = config.m_uiSampleRate;
  m_Config.m_uiPeriodMs = config.m_uiPeriodMs;
  m_Config.m_bFollowDefaultDevice = config.m_bFollowDefaultDevice;
  m_Config.m_eSuspendPolicy = config.m_eSuspendPolicy;
  m_Config.m_fSuspendFadeMs = config.m_fSuspendFadeMs;
  m_bDrawDirty = true;
  RequestRedraw();
}

void plEditorUiAudioPlatformsView::SetBackends(plArrayPtr<const plString> names)
{
  m_Backends = names;
  m_bDrawDirty = true;
}

void plEditorUiAudioPlatformsView::SetOutput(plStringView sDevice, plStringView sState)
{
  if (m_sDevice == sDevice && m_sState == sState)
    return;

  m_sDevice = sDevice;
  m_sState = sState;
  m_bDrawDirty = true;
  RequestRedraw();
}

void plEditorUiAudioPlatformsView::AddLog(plStringView sText, LogKind kind)
{
  if (m_Log.GetCount() >= s_uiMaxLog)
    m_Log.RemoveAtAndCopy(0);

  LogLine& line = m_Log.ExpandAndGetRef();
  line.m_sText = sText;
  line.m_Kind = kind;
  m_bDrawDirty = true;
  RequestRedraw();
}

void plEditorUiAudioPlatformsView::SetSimdName(plStringView sName)
{
  m_sSimd = sName;
  m_bDrawDirty = true;
}

void plEditorUiAudioPlatformsView::SetBenchmark(const plAudioCodecBenchmark::Result& result)
{
  m_Benchmark = result;
  m_bHasBenchmark = true;
  m_bDrawDirty = true;
  RequestRedraw();
}

void plEditorUiAudioPlatformsView::SetBenchmarkRunning(bool bRunning)
{
  m_bBenchmarkRunning = bRunning;
  m_bDrawDirty = true;
  RequestRedraw();
}

void plEditorUiAudioPlatformsView::SetSuspendRunning(bool bRunning)
{
  m_bSuspendRunning = bRunning;
  m_bDrawDirty = true;
  RequestRedraw();
}

float plEditorUiAudioPlatformsView::GetRowY(plUInt32 uiRow) const
{
  return s_fTabRow + 8.0f + uiRow * s_fRowPitch;
}

float plEditorUiAudioPlatformsView::GetSideX() const
{
  const float fWidth = IsReady() ? GetSurface()->GetLogicalSize().x : 900.0f;
  return plMath::Max(fWidth - s_fSideWidth, 360.0f);
}

plRectFloat plEditorUiAudioPlatformsView::GetSliderRect(Slider slider) const
{
  const float x = s_fPad + s_fLabelWidth;
  const float fWidth = plMath::Min(GetSideX() - x - s_fPad - 70.0f, s_fMaxControl);
  const plUInt32 uiRow = slider == Slider::Period ? s_uiRowPeriod : s_uiRowFade;
  return plRectFloat(x, GetRowY(uiRow) + 6.0f, plMath::Max(fWidth, 60.0f), plEditorUi::ControlHeight - 12.0f);
}

void plEditorUiAudioPlatformsView::OnUiUpdate()
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

void plEditorUiAudioPlatformsView::BuildDocument()
{
  using namespace plEditorUi;

  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = Surface();
  DrawList(pRoot, "Platforms.Drawing", s_sDrawingEntry.GetView());

  plBlackboard* pBoard = &GetSurface()->GetBlackboard();

  // profile tabs
  float x = s_fPad;
  for (plUInt32 i = 0; i < m_Profiles.GetCount(); ++i)
  {
    const float fWidth = MeasureText(m_Profiles[i], FontSize) + 28.0f;
    plAudioEngineUi::HitButton(pRoot, plRectFloat(x, 2.0f, fWidth, s_fTabRow - 4.0f), IndexedName("Pf.Tab", i), i == m_uiActiveProfile ? "The profile the editor runs: changes apply to its engine process at once" : "Changes are saved with this profile", pBoard);
    x += fWidth;
  }

  const float fControlX = s_fPad + s_fLabelWidth;
  const float fControl = plMath::Min(GetSideX() - fControlX - s_fPad, s_fMaxControl);

  plAudioEngineUi::HitButton(pRoot, plRectFloat(fControlX, GetRowY(s_uiRowBackend), fControl, ControlHeight), "Pf.Backend", "The output backend; a backend plugin adds its own entry", pBoard);
  plAudioEngineUi::HitButton(pRoot, plRectFloat(fControlX, GetRowY(s_uiRowSpeakers), 150.0f, ControlHeight), "Pf.Speakers", "The speaker layout the output mixes for", pBoard);
  plAudioEngineUi::HitButton(pRoot, plRectFloat(fControlX + 170.0f, GetRowY(s_uiRowSpeakers), 230.0f, ControlHeight), "Pf.Follow", "Moves to the new default device when the system's default changes", pBoard);
  plAudioEngineUi::HitButton(pRoot, plRectFloat(fControlX, GetRowY(s_uiRowRate), 150.0f, ControlHeight), "Pf.Rate", "The output sample rate", pBoard);

  const float fBench = plAudioEngineUi::TextButtonWidth("Run decode benchmark", false);
  plAudioEngineUi::HitButton(pRoot, plRectFloat(fControlX + fControl - fBench, GetRowY(s_uiRowOpus), fBench, ControlHeight), "Pf.Bench", "Decodes a test signal for a moment with each codec on this computer", pBoard);

  plAudioEngineUi::HitButton(pRoot, plRectFloat(fControlX, GetRowY(s_uiRowPolicy), fControl, ControlHeight), "Pf.Policy", "What the output does while the application is suspended", pBoard);
  plAudioEngineUi::HitButton(pRoot, plRectFloat(fControlX, GetRowY(s_uiRowSimulate), plAudioEngineUi::TextButtonWidth("Simulate suspend", false), ControlHeight), "Pf.Suspend", "Suspends the editor's engine process for a moment and resumes it, as a platform does", pBoard);

  SetDocument(Finish(pRoot));
  m_bDrawDirty = true;
}

void plEditorUiAudioPlatformsView::Draw()
{
  using namespace plEditorUi;

  m_bDrawDirty = false;
  m_Writer.Clear();

  const plVec2 size = GetSurface()->GetLogicalSize();

  // profile tabs: the shown one underlined, the active one with a dot
  m_Writer.Rect(plRectFloat(0.0f, 0.0f, size.x, s_fTabRow), Surface());
  m_Writer.Rect(plRectFloat(0.0f, s_fTabRow - 1.0f, size.x, 1.0f), Line());

  float x = s_fPad;
  for (plUInt32 i = 0; i < m_Profiles.GetCount(); ++i)
  {
    const float fWidth = MeasureText(m_Profiles[i], FontSize) + 28.0f;
    const bool bShown = i == m_uiShownProfile;

    if (i == m_uiActiveProfile)
      m_Writer.Rect(plRectFloat(x + 6.0f, s_fTabRow * 0.5f - 2.0f, 4.0f, 4.0f), Accent(), 2.0f);

    m_Writer.Text(plRectFloat(x + 14.0f, 0.0f, fWidth - 14.0f, s_fTabRow), m_Profiles[i], bShown ? Foreground() : Muted(), FontSize);
    if (bShown)
      m_Writer.Rect(plRectFloat(x + 4.0f, s_fTabRow - 3.0f, fWidth - 8.0f, 2.0f), Accent());

    x += fWidth;
  }

  if (m_Profiles.IsEmpty())
    m_Writer.Text(plRectFloat(s_fPad, 0.0f, 400.0f, s_fTabRow), "no asset profiles", Dim(), FontSize);

  DrawSettings();
  DrawSide();

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sDrawingEntry);
}

void plEditorUiAudioPlatformsView::DrawSettings()
{
  using namespace plEditorUi;

  const float fSideX = GetSideX();
  const float fControlX = s_fPad + s_fLabelWidth;
  const float fControl = plMath::Min(fSideX - fControlX - s_fPad, s_fMaxControl);
  const plColorGammaUB audio = plAudioEngineUi::AudioColor();

  auto section = [&](plUInt32 uiRow, plStringView sTitle)
  { m_Writer.Text(plRectFloat(s_fPad, GetRowY(uiRow) + 8.0f, 300.0f, 16.0f), sTitle, Muted(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true); };

  auto label = [&](plUInt32 uiRow, plStringView sText)
  { m_Writer.Text(plRectFloat(s_fPad, GetRowY(uiRow), s_fLabelWidth - 8.0f, ControlHeight), sText, Foreground(), FontSize - 1); };

  // output
  section(s_uiRowOutput, "OUTPUT");

  label(s_uiRowBackend, "Backend");
  plAudioEngineUi::Dropdown(m_Writer, plRectFloat(fControlX, GetRowY(s_uiRowBackend), fControl, ControlHeight), {}, m_Config.m_sBackend.IsEmpty() ? plStringView("Platform default (usual order)") : m_Config.m_sBackend.GetView());

  label(s_uiRowSpeakers, "Speakers");
  plAudioEngineUi::Dropdown(m_Writer, plRectFloat(fControlX, GetRowY(s_uiRowSpeakers), 150.0f, ControlHeight), {}, s_szSpeakers[plMath::Min<plUInt32>(m_Config.m_eSpeakers.GetValue(), 3)]);

  {
    // the follow checkbox
    const plRectFloat box(fControlX + 170.0f, GetRowY(s_uiRowSpeakers) + 6.0f, 14.0f, 14.0f);
    m_Writer.Rect(box, m_Config.m_bFollowDefaultDevice ? Accent() : Field(), 3.0f);
    m_Writer.RectOutline(box, m_Config.m_bFollowDefaultDevice ? Accent() : Border(), 1.0f, 3.0f);
    if (m_Config.m_bFollowDefaultDevice)
    {
      const plVec2 tick[] = {plVec2(box.x + 3.5f, box.y + 7.5f), plVec2(box.x + 6.0f, box.y + 10.0f), plVec2(box.x + 10.5f, box.y + 4.5f)};
      m_Writer.Polyline(tick, Background(), 1.75f);
    }
    m_Writer.Text(plRectFloat(box.x + 22.0f, GetRowY(s_uiRowSpeakers), 220.0f, ControlHeight), "follow the default device", Foreground(), FontSize - 1);
  }

  label(s_uiRowRate, "Sample rate");
  plAudioEngineUi::Dropdown(m_Writer, plRectFloat(fControlX, GetRowY(s_uiRowRate), 150.0f, ControlHeight), {}, RateText(m_Config.m_uiSampleRate));

  label(s_uiRowPeriod, "Period");
  {
    const float fPeriod = m_DragSlider == Slider::Period ? m_fDragValue : static_cast<float>(m_Config.m_uiPeriodMs);
    const plRectFloat rect = GetSliderRect(Slider::Period);
    plAudioEngineUi::Slider(m_Writer, rect, fPeriod / s_fMaxPeriodMs, true, m_DragSlider == Slider::Period);

    plStringBuilder sValue;
    if (fPeriod <= 0.0f)
      sValue = "default";
    else
      sValue.SetFormat("{} ms", plArgF(fPeriod, 0));
    m_Writer.Text(plRectFloat(rect.x + rect.width + 8.0f, GetRowY(s_uiRowPeriod), 64.0f, ControlHeight), sValue, Muted(), FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
  }

  // decoding
  section(s_uiRowDecoding, "DECODING");

  label(s_uiRowOpus, "Opus");
  {
    plStringBuilder sSimd("kernels: ", m_sSimd);
    m_Writer.Text(plRectFloat(fControlX, GetRowY(s_uiRowOpus), fControl - 200.0f, ControlHeight), sSimd, Muted(), FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);

    const float fBench = plAudioEngineUi::TextButtonWidth("Run decode benchmark", false);
    plAudioEngineUi::TextButton(m_Writer, plRectFloat(fControlX + fControl - fBench, GetRowY(s_uiRowOpus), fBench, ControlHeight), m_bBenchmarkRunning ? "Measuring..." : "Run decode benchmark", {}, !m_bBenchmarkRunning);
  }

  {
    plStringBuilder sSimdBar("Opus, ", m_sSimd);
    const char* szNames[] = {"Opus, plain C", sSimdBar.GetData(), "IMA ADPCM"};
    const float fValues[] = {m_Benchmark.m_fOpusPlainVoices, m_Benchmark.m_fOpusSimdVoices, m_Benchmark.m_fAdpcmVoices};
    const plColorGammaUB colors[] = {Muted(), Accent(), audio};

    const float fMax = plMath::Max(plMath::Max(fValues[0], fValues[1]), plMath::Max(fValues[2], 0.001f));
    for (plUInt32 i = 0; i < 3; ++i)
    {
      const float y = GetRowY(s_uiRowBars + i);
      m_Writer.Text(plRectFloat(s_fPad + 8.0f, y, s_fLabelWidth + 40.0f, ControlHeight), szNames[i], Foreground(), FontSize - 2);

      const plRectFloat bar(fControlX + 40.0f, y + 7.0f, fControl - 110.0f, ControlHeight - 14.0f);
      m_Writer.Rect(bar, Field(), 2.0f);
      if (m_bHasBenchmark && fValues[i] > 0.0f)
        m_Writer.Rect(plRectFloat(bar.x, bar.y, bar.width * fValues[i] / fMax, bar.height), colors[i], 2.0f);

      plStringBuilder sValue;
      if (m_bHasBenchmark && fValues[i] > 0.0f)
        sValue.SetFormat("{}", plArgF(fValues[i], 1));
      else
        sValue = "-";
      m_Writer.Text(plRectFloat(bar.x + bar.width + 8.0f, y, 60.0f, ControlHeight), sValue, Muted(), FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
    }

    m_Writer.Text(plRectFloat(fControlX + 40.0f, GetRowY(s_uiRowBars + 3) - 8.0f, fControl, 14.0f), "voices decoded per 1 % of one core, on this computer", Dim(), MinFontSize);
  }

  // suspend and resume
  section(s_uiRowSuspend, "SUSPEND AND RESUME");

  label(s_uiRowPolicy, "On suspend");
  plAudioEngineUi::Dropdown(m_Writer, plRectFloat(fControlX, GetRowY(s_uiRowPolicy), fControl, ControlHeight), {}, s_szPolicies[plMath::Min<plUInt32>(m_Config.m_eSuspendPolicy.GetValue(), 2)]);

  label(s_uiRowFade, "Fade");
  {
    const float fFade = m_DragSlider == Slider::Fade ? m_fDragValue : m_Config.m_fSuspendFadeMs;
    const plRectFloat rect = GetSliderRect(Slider::Fade);
    plAudioEngineUi::Slider(m_Writer, rect, fFade / s_fMaxFadeMs, true, m_DragSlider == Slider::Fade);

    plStringBuilder sValue;
    sValue.SetFormat("{} ms", plArgF(fFade, 0));
    m_Writer.Text(plRectFloat(rect.x + rect.width + 8.0f, GetRowY(s_uiRowFade), 64.0f, ControlHeight), sValue, Muted(), FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
  }

  plAudioEngineUi::TextButton(m_Writer, plRectFloat(fControlX, GetRowY(s_uiRowSimulate), plAudioEngineUi::TextButtonWidth("Simulate suspend", false), ControlHeight), "Simulate suspend", {}, !m_bSuspendRunning);
}

void plEditorUiAudioPlatformsView::DrawSide()
{
  using namespace plEditorUi;

  const plVec2 size = GetSurface()->GetLogicalSize();
  const float fSideX = GetSideX();
  const float fWidth = size.x - fSideX;

  m_Writer.Rect(plRectFloat(fSideX, s_fTabRow, fWidth, plMath::Max(size.y - s_fTabRow, 1.0f)), plAudioEngineUi::Mix(Surface(), Background(), 0.5f));
  m_Writer.Rect(plRectFloat(fSideX, s_fTabRow, 1.0f, plMath::Max(size.y - s_fTabRow, 1.0f)), Line());

  float y = s_fTabRow + 16.0f;
  m_Writer.Text(plRectFloat(fSideX + s_fPad, y, fWidth, 16.0f), "ENGINE PROCESS OUTPUT", Muted(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
  y += 22.0f;

  // the device description, one part per line
  plHybridArray<plStringView, 8> parts;
  plStringView(m_sDevice).Split(false, parts, " \xC2\xB7 ");
  if (parts.IsEmpty())
    parts.PushBack("no engine process");

  for (plStringView sPart : parts)
  {
    m_Writer.Text(plRectFloat(fSideX + s_fPad, y, fWidth - 2.0f * s_fPad, 16.0f), sPart, Foreground(), FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    y += 17.0f;
  }

  y += 8.0f;
  if (!m_sState.IsEmpty())
  {
    const bool bRunning = m_sState.StartsWith("running");
    const plColorGammaUB color = bRunning ? Axis(1) : Warning();
    plAudioEngineUi::Chip(m_Writer, plRectFloat(fSideX + s_fPad, y, plAudioEngineUi::ChipWidth(m_sState), 18.0f), m_sState, color, true);
    y += 28.0f;
  }

  m_Writer.Text(plRectFloat(fSideX + s_fPad, y, fWidth, 16.0f), "OUTPUT LOG", Muted(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
  y += 20.0f;

  for (plUInt32 i = m_Log.GetCount(); i > 0 && y + 18.0f < size.y; --i)
  {
    const LogLine& line = m_Log[i - 1];
    const plColorGammaUB color = line.m_Kind == LogKind::Ok ? Axis(1) : (line.m_Kind == LogKind::Warning ? Warning() : Foreground());
    m_Writer.Text(plRectFloat(fSideX + s_fPad, y, fWidth - 2.0f * s_fPad, 18.0f), line.m_sText, color, FontSize - 2);
    y += 18.0f;
  }
}

void plEditorUiAudioPlatformsView::Changed()
{
  m_bDrawDirty = true;
  RequestRedraw();

  if (m_OnConfigChanged.IsValid())
    m_OnConfigChanged(m_Config);
}

void plEditorUiAudioPlatformsView::ShowMenu(plStringView sAction)
{
  const float fControlX = s_fPad + s_fLabelWidth;
  plEditorUiMenu menu;
  plUInt32 uiRow = 0;

  if (sAction == "Pf.Backend")
  {
    uiRow = s_uiRowBackend;
    menu.AddAction("Platform default (usual order)", 0);
    for (plUInt32 i = 0; i < m_Backends.GetCount(); ++i)
      menu.AddAction(m_Backends[i], i + 1);
  }
  else if (sAction == "Pf.Speakers")
  {
    uiRow = s_uiRowSpeakers;
    for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_szSpeakers); ++i)
      menu.AddAction(s_szSpeakers[i], i);
  }
  else if (sAction == "Pf.Rate")
  {
    uiRow = s_uiRowRate;
    for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_uiRates); ++i)
      menu.AddAction(RateText(s_uiRates[i]), i);
  }
  else if (sAction == "Pf.Policy")
  {
    uiRow = s_uiRowPolicy;
    for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_szPolicies); ++i)
      menu.AddAction(s_szPolicies[i], i);
  }
  else
  {
    return;
  }

  const plString sChosen = sAction;
  plEditorUiMenuView::Show(*GetSurface(), plVec2(fControlX, GetRowY(uiRow) + plEditorUi::ControlHeight), menu, [this, sChosen](plUInt32 uiId)
    {
      if (sChosen == "Pf.Backend")
        m_Config.m_sBackend = (uiId == 0 || uiId > m_Backends.GetCount()) ? plString() : m_Backends[uiId - 1];
      else if (sChosen == "Pf.Speakers")
        m_Config.m_eSpeakers = static_cast<plAudioSpeakerLayout::Enum>(uiId);
      else if (sChosen == "Pf.Rate")
        m_Config.m_uiSampleRate = s_uiRates[plMath::Min<plUInt32>(uiId, PL_ARRAY_SIZE(s_uiRates) - 1)];
      else if (sChosen == "Pf.Policy")
        m_Config.m_eSuspendPolicy = static_cast<plAudioSuspendPolicy::Enum>(uiId);

      Changed();
    });
}

void plEditorUiAudioPlatformsView::OnUiEvent(const plMsgUiEvent& event)
{
  const plStringView sId = event.m_sIdentifier.GetView();

  if (sId.StartsWith("Pf.Tab."))
  {
    plUInt32 uiTab = 0;
    if (plConversionUtils::StringToUInt(sId.GetSubString(7, 8), uiTab).Succeeded() && m_OnProfileChosen.IsValid())
      m_OnProfileChosen(uiTab);
  }
  else if (sId == "Pf.Follow")
  {
    m_Config.m_bFollowDefaultDevice = !m_Config.m_bFollowDefaultDevice;
    Changed();
  }
  else if (sId == "Pf.Bench")
  {
    if (!m_bBenchmarkRunning && m_OnRunBenchmark.IsValid())
      m_OnRunBenchmark();
  }
  else if (sId == "Pf.Suspend")
  {
    if (!m_bSuspendRunning && m_OnSimulateSuspend.IsValid())
      m_OnSimulateSuspend();
  }
  else
  {
    ShowMenu(sId);
  }
}

void plEditorUiAudioPlatformsView::OnPointer(const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;

  auto valueAt = [&](Slider slider, float x)
  {
    const float fFraction = plAudioEngineUi::SliderFractionAt(GetSliderRect(slider), x);
    return slider == Slider::Period ? plMath::Round(fFraction * s_fMaxPeriodMs) : plMath::Round(fFraction * s_fMaxFadeMs / 10.0f) * 10.0f;
  };

  switch (e.m_Type)
  {
    case Type::Press:
    {
      if (e.m_Button != plEditorUiPointerEvent::Button::Left || !e.m_sElement.IsEmpty())
        return;

      for (Slider slider : {Slider::Period, Slider::Fade})
      {
        const plRectFloat rect = GetSliderRect(slider);
        if (plRectFloat(rect.x - 4.0f, rect.y - 4.0f, rect.width + 8.0f, rect.height + 8.0f).Contains(e.m_vPosition))
        {
          m_DragSlider = slider;
          m_fDragValue = valueAt(slider, e.m_vPosition.x);
          m_bDrawDirty = true;
          RequestRedraw();
          return;
        }
      }
      return;
    }

    case Type::Move:
    case Type::DragStart:
    case Type::DragMove:
    {
      if (m_DragSlider == Slider::None)
        return;

      m_fDragValue = valueAt(m_DragSlider, e.m_vPosition.x);
      m_bDrawDirty = true;
      RequestRedraw();
      return;
    }

    case Type::Release:
    case Type::Drop:
    {
      if (m_DragSlider == Slider::None)
        return;

      // one change per drag, on release
      if (m_DragSlider == Slider::Period)
        m_Config.m_uiPeriodMs = static_cast<plUInt32>(m_fDragValue);
      else
        m_Config.m_fSuspendFadeMs = m_fDragValue;

      m_DragSlider = Slider::None;
      Changed();
      return;
    }

    case Type::DragCancel:
      m_DragSlider = Slider::None;
      m_bDrawDirty = true;
      return;

    default:
      return;
  }
}
