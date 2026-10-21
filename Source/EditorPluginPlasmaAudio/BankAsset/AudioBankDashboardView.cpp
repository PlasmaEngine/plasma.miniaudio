#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAsset.h>
#include <EditorPluginPlasmaAudio/BankAsset/AudioBankDashboardView.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorUi/Drawings.h>
#include <EditorUi/Menu.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

#include <QTimer>

namespace
{
  constexpr float s_fPad = 8.0f;
  constexpr float s_fBarHeight = 36.0f;
  constexpr float s_fBudgetHeight = 74.0f;
  constexpr float s_fBodyTop = s_fBarHeight + s_fBudgetHeight;
  constexpr float s_fStatusHeight = 22.0f;
  constexpr float s_fLeftWidth = 300.0f;
  constexpr float s_fBankRow = 26.0f;
  constexpr float s_fWarningRow = 36.0f;
  constexpr float s_fContentHeader = 36.0f;
  constexpr float s_fRow = plEditorUi::RowHeight;

  const plHashedString s_sDrawingEntry = plMakeHashedString("BankDashboard.Drawing");

  // the mockup's category colours (decoded is the Audio colour)
  const plColorGammaUB s_CompressedColor(143, 106, 135);
  const plColorGammaUB s_PrimingColor(110, 168, 254);
  const plColorGammaUB s_RingColor(86, 86, 94);
  const plColorGammaUB s_OkColor(105, 219, 124);

  constexpr double s_fMegabyte = 1024.0 * 1024.0;

  plStringView LoadModeName(plAudioClipLoadMode::Enum mode)
  {
    switch (mode)
    {
      case plAudioClipLoadMode::CompressedInMemory:
        return "Compressed in memory";
      case plAudioClipLoadMode::Streamed:
        return "Streamed (priming)";
      default:
        return "Decompress on load";
    }
  }

  plStringView LanguageName(plStringView sLanguage)
  {
    return sLanguage.IsEmpty() ? plStringView("default") : sLanguage;
  }

  /// The text cut to the width with an ellipsis.
  plString Elide(plStringView sText, float fWidth, plUInt32 uiFontSize, bool bMonospace = false)
  {
    auto Measure = [&](plStringView s)
    { return bMonospace ? plEditorUi::MeasureCodeText(s, uiFontSize) : plEditorUi::MeasureText(s, uiFontSize); };

    if (Measure(sText) <= fWidth)
      return sText;

    plStringBuilder sCut = sText;
    while (!sCut.IsEmpty())
    {
      sCut.Shrink(0, 1);

      plStringBuilder sTry = sCut;
      sTry.Append("\xE2\x80\xA6");
      if (Measure(sTry) <= fWidth)
        return sTry;
    }

    return plString();
  }

  /// Splits the text into two lines that fit the width; the second is elided.
  void WrapTwoLines(plStringView sText, float fWidth, plUInt32 uiFontSize, plStringBuilder& out_sFirst, plStringBuilder& out_sSecond)
  {
    out_sFirst.Clear();
    out_sSecond.Clear();

    plHybridArray<plStringView, 32> words;
    sText.Split(false, words, " ");

    plUInt32 uiWord = 0;
    for (; uiWord < words.GetCount(); ++uiWord)
    {
      plStringBuilder sTry = out_sFirst;
      sTry.AppendWithSeparator(" ", words[uiWord]);
      if (!out_sFirst.IsEmpty() && plEditorUi::MeasureText(sTry, uiFontSize) > fWidth)
        break;

      out_sFirst = sTry;
    }

    for (; uiWord < words.GetCount(); ++uiWord)
    {
      out_sSecond.AppendWithSeparator(" ", words[uiWord]);
    }

    out_sSecond = Elide(out_sSecond, fWidth, uiFontSize);
  }

  /// A painted check box with its label; returns the width it took.
  float CheckBox(plUiDrawListWriter& ref_writer, const plVec2& vAt, plStringView sLabel, bool bChecked)
  {
    using namespace plEditorUi;

    const plRectFloat box(vAt.x, vAt.y + (ControlHeight - 14.0f) * 0.5f, 14.0f, 14.0f);
    ref_writer.Rect(box, bChecked ? Accent() : Field(), 3.0f);
    if (!bChecked)
      ref_writer.RectOutline(box, Border(), 1.0f, 3.0f);
    else
    {
      const plVec2 check[] = {plVec2(box.x + 3.5f, box.y + 7.5f), plVec2(box.x + 6.0f, box.y + 10.0f), plVec2(box.x + 10.5f, box.y + 4.0f)};
      ref_writer.Polyline(check, Background(), 1.75f);
    }

    const float fLabel = MeasureText(sLabel, FontSize - 1);
    ref_writer.Text(plRectFloat(box.x + 20.0f, vAt.y, fLabel + 2.0f, ControlHeight), sLabel, Foreground(), FontSize - 1);
    return 20.0f + fLabel;
  }

  /// A small pill with a text, filled in a dim tint of the colour.
  float Badge(plUiDrawListWriter& ref_writer, const plVec2& vAt, plStringView sText, const plColorGammaUB& color, float fHeight = 16.0f)
  {
    const float fWidth = plEditorUi::MeasureText(sText, plEditorUi::MinFontSize + 1) + 12.0f;
    const plRectFloat rect(vAt.x, vAt.y, fWidth, fHeight);
    ref_writer.Rect(rect, plAudioEngineUi::Mix(plEditorUi::Surface(), color, 0.18f), fHeight * 0.5f);
    ref_writer.Text(rect, sText, color, plEditorUi::MinFontSize + 1, plUiTextHAlign::Center);
    return fWidth;
  }
} // namespace

plEditorUiAudioBankDashboard::plEditorUiAudioBankDashboard(plAudioBankAssetDocument* pDocument)
  : plEditorUiContent("AudioBankDashboard")
  , m_pDocument(pDocument)
{
  if (IsReady())
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioBankDashboard::OnPointer, this));

  m_pDocument->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioBankDashboard::PropertyEventHandler, this));
  m_pDocument->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioBankDashboard::StructureEventHandler, this));
  m_StatusSubscription = m_pDocument->m_EngineStatusEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioBankDashboard::OnEngineStatus, this));
  m_CuratorSubscription = plAssetCurator::GetSingleton()->m_Events.AddEventHandler(plMakeDelegate(&plEditorUiAudioBankDashboard::CuratorEventHandler, this));
}

plEditorUiAudioBankDashboard::~plEditorUiAudioBankDashboard()
{
  plAssetCurator::GetSingleton()->m_Events.RemoveEventHandler(m_CuratorSubscription);
  m_pDocument->m_EngineStatusEvents.RemoveEventHandler(m_StatusSubscription);
  m_pDocument->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plEditorUiAudioBankDashboard::PropertyEventHandler, this));
  m_pDocument->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plEditorUiAudioBankDashboard::StructureEventHandler, this));

  if (IsReady())
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
}

void plEditorUiAudioBankDashboard::OnTimer()
{
  if (m_iCuratorChanges.Set(0) == 0)
    return;

  Rebuild();
}

void plEditorUiAudioBankDashboard::Rebuild()
{
  const plString sSelected = GetSelectedBank() != nullptr ? GetSelectedBank()->m_sGuid : plString();

  m_Model.Build(*m_pDocument);
  m_Model.ApplyEngineStatus(m_pDocument->GetEngineStatus());

  // the selection follows its bank
  const plInt32 iBank = m_Model.FindBank(sSelected);
  m_iSelectedBank = iBank >= 0 ? iBank : 0;

  m_bDrawDirty = true;
  RequestRedraw();
}

const plAudioBankDashboard::Bank* plEditorUiAudioBankDashboard::GetSelectedBank() const
{
  if (m_iSelectedBank < 0 || m_iSelectedBank >= static_cast<plInt32>(m_Model.m_Banks.GetCount()))
    return nullptr;

  return &m_Model.m_Banks[m_iSelectedBank];
}

void plEditorUiAudioBankDashboard::OnEngineStatus(const plAudioBankStatusMsgToEditor& status)
{
  m_Model.ApplyEngineStatus(status);
  m_bDrawDirty = true;
  RequestRedraw();
}

void plEditorUiAudioBankDashboard::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  m_bRebuild = true;
  RequestRedraw();
}

void plEditorUiAudioBankDashboard::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  m_bRebuild = true;
  RequestRedraw();
}

void plEditorUiAudioBankDashboard::CuratorEventHandler(const plAssetCuratorEvent& e)
{
  // any thread; the window's timer rebuilds
  m_iCuratorChanges.Increment();
}

//////////////////////////////////////////////////////////////////////////
// Drawing

void plEditorUiAudioBankDashboard::OnUiUpdate()
{
  if (!IsReady())
    return;

  if (GetSurface()->GetLogicalSize() != m_vBuiltSize)
  {
    using namespace plEditorUi;

    m_vBuiltSize = GetSurface()->GetLogicalSize();

    plUiPanel* pRoot = Panel();
    pRoot->m_Color = Surface();
    DrawList(pRoot, "BankDashboard.Canvas", s_sDrawingEntry.GetView());
    SetDocument(Finish(pRoot));
    m_bDrawDirty = true;
  }

  if (m_bRebuild)
  {
    m_bRebuild = false;
    Rebuild();
  }

  if (m_bDrawDirty)
    Draw();
}

void plEditorUiAudioBankDashboard::AddHit(Hit::Kind kind, const plRectFloat& rect, plInt32 iIndex, plStringView sToolTip)
{
  Hit& hit = m_Hits.ExpandAndGetRef();
  hit.m_Kind = kind;
  hit.m_Rect = rect;
  hit.m_iIndex = iIndex;
  hit.m_sToolTip = sToolTip;
}

const plEditorUiAudioBankDashboard::Hit* plEditorUiAudioBankDashboard::FindHit(const plVec2& vPos) const
{
  for (const Hit& hit : m_Hits)
  {
    if (hit.m_Rect.Contains(vPos))
      return &hit;
  }

  return nullptr;
}

void plEditorUiAudioBankDashboard::Draw()
{
  m_bDrawDirty = false;
  m_Writer.Clear();
  m_Hits.Clear();

  const plVec2 size = GetSurface()->GetLogicalSize();

  DrawBar(size);
  DrawBudget(size);

  const float fBodyHeight = plMath::Max(size.y - s_fBodyTop - s_fStatusHeight, 40.0f);
  const float fLeft = plMath::Min(s_fLeftWidth, plMath::Max(size.x * 0.4f, 200.0f));

  // the banks take what they need up to half of the column, the warnings the rest
  const float fBanksHeight = plMath::Min(plAudioEngineUi::SectionHeight + plMath::Max<plUInt32>(m_Model.m_Banks.GetCount(), 1) * s_fBankRow + 4.0f, fBodyHeight * 0.5f);
  DrawBanks(plRectFloat(0.0f, s_fBodyTop, fLeft, fBanksHeight));
  DrawWarnings(plRectFloat(0.0f, s_fBodyTop + fBanksHeight, fLeft, fBodyHeight - fBanksHeight));

  m_Writer.Rect(plRectFloat(fLeft, s_fBodyTop, 1.0f, fBodyHeight), plEditorUi::Line());
  DrawContents(plRectFloat(fLeft + 1.0f, s_fBodyTop, size.x - fLeft - 1.0f, fBodyHeight));

  DrawStatusRow(size);

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sDrawingEntry);
}

void plEditorUiAudioBankDashboard::DrawBar(const plVec2& size)
{
  using namespace plEditorUi;

  const float y = (s_fBarHeight - ControlHeight) * 0.5f;
  float x = s_fPad;

  // the platforms: one segment per asset profile
  for (plUInt32 i = 0; i < m_Model.m_Profiles.GetCount(); ++i)
  {
    const plString& sProfile = m_Model.m_Profiles[i];
    const bool bOn = i == m_Model.m_uiProfile;
    const float fWidth = MeasureText(sProfile, FontSize - 1) + 20.0f;
    const plRectFloat rect(x, y, fWidth, ControlHeight);

    m_Writer.Rect(rect, bOn ? Raised() : Plate(), 3.0f);
    if (bOn)
      m_Writer.Rect(plRectFloat(rect.x + 4.0f, rect.y + rect.height - 2.0f, rect.width - 8.0f, 2.0f), Accent());

    m_Writer.Text(rect, sProfile, bOn ? Foreground() : Muted(), FontSize - 1, plUiTextHAlign::Center);
    AddHit(Hit::Kind::Platform, rect, static_cast<plInt32>(i), "The platform (asset profile) the sizes and the budget are for");
    x += fWidth + 2.0f;
  }

  // the language
  x += 10.0f;
  const plRectFloat language(x, y, 150.0f, ControlHeight);
  plAudioEngineUi::Dropdown(m_Writer, language, "Language", LanguageName(m_Model.m_sLanguage));
  AddHit(Hit::Kind::Language, language, -1, "The language localised clips are sized in; clips without it are warned about");

  // auto-include, of the window's bank, at the right end
  const bool bAuto = m_pDocument->GetProperties()->m_bAutoIncludeEventClips;
  const plStringView sAuto = "Auto-include event clips";
  const float fAutoWidth = 20.0f + MeasureText(sAuto, FontSize - 1);
  const plVec2 vAuto(size.x - s_fPad - fAutoWidth, y);
  CheckBox(m_Writer, vAuto, sAuto, bAuto);
  AddHit(Hit::Kind::AutoInclude, plRectFloat(vAuto.x, vAuto.y, fAutoWidth, ControlHeight), -1, "This bank also loads every clip its events play");

  m_Writer.Rect(plRectFloat(0.0f, s_fBarHeight - 1.0f, size.x, 1.0f), Line());
}

void plEditorUiAudioBankDashboard::DrawBudget(const plVec2& size)
{
  using namespace plEditorUi;

  const plAudioMemoryUsage& loaded = m_Model.m_Loaded;
  const plAudioMemoryBudget& budget = m_Model.m_Budget;
  const plUInt64 uiTotal = loaded.GetTotalBytes();
  const plStringView sProfile = m_Model.m_Profiles.IsEmpty() ? plStringView("this platform") : m_Model.m_Profiles[m_Model.m_uiProfile].GetView();

  const float y = s_fBarHeight + 8.0f;
  const float fWidth = size.x - 2.0f * s_fPad;

  // "Loaded banks: 34.5 MB" and the budget's share
  {
    plStringBuilder sLabel, sValue;
    sLabel.SetFormat("Loaded banks ({} of {}):", m_Model.m_uiLoadedBanks, m_Model.m_Banks.GetCount());
    sValue.SetFormat("{} MB", plArgF(uiTotal / s_fMegabyte, 1));

    const float fLabel = MeasureText(sLabel, FontSize - 1);
    m_Writer.Text(plRectFloat(s_fPad, y, fLabel + 2.0f, 14.0f), sLabel, Muted(), FontSize - 1);
    m_Writer.Text(plRectFloat(s_fPad + fLabel + 6.0f, y, 120.0f, 14.0f), sValue, Foreground(), FontSize - 1, plUiTextHAlign::Left, plUiTextVAlign::Center, true);

    plStringBuilder sBudget;
    plColorGammaUB budgetColor = Muted();
    if (budget.m_uiTotalBytes == 0)
    {
      sBudget.SetFormat("no budget for {} (Platform Profiles, Audio Budgets)", sProfile);
    }
    else if (uiTotal > budget.m_uiTotalBytes)
    {
      sBudget.SetFormat("budget {} MB \xC2\xB7 over by {} MB", plArgF(budget.m_uiTotalBytes / s_fMegabyte, budget.m_uiTotalBytes < 10 * 1024 * 1024 ? 1 : 0), plArgF((uiTotal - budget.m_uiTotalBytes) / s_fMegabyte, 1));
      budgetColor = Danger();
    }
    else
    {
      sBudget.SetFormat("budget {} MB \xC2\xB7 {}% used", plArgF(budget.m_uiTotalBytes / s_fMegabyte, budget.m_uiTotalBytes < 10 * 1024 * 1024 ? 1 : 0), static_cast<plInt32>(100.0 * uiTotal / budget.m_uiTotalBytes));
      budgetColor = s_OkColor;
    }

    m_Writer.Text(plRectFloat(s_fPad, y, fWidth, 14.0f), sBudget, budgetColor, FontSize - 1, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
  }

  // the bar: the categories one after the other, the budget as a white line
  const plRectFloat bar(s_fPad, y + 20.0f, fWidth, 16.0f);
  m_Writer.Rect(bar, Field(), 3.0f);
  {
    const double fScale = plMath::Max(plMath::Max(budget.m_uiTotalBytes * 1.1, uiTotal * 1.05), 1.0);
    const plUInt64 parts[] = {loaded.m_uiDecodedBytes, loaded.m_uiCompressedBytes, loaded.m_uiPrimingBytes, loaded.m_uiRingBytes};
    const plColorGammaUB colors[] = {plAudioEngineUi::AudioColor(), s_CompressedColor, s_PrimingColor, s_RingColor};
    const char* szNames[] = {"Resident (decoded)", "Resident (compressed)", "Stream priming", "Stream rings"};

    float x = bar.x;
    m_Writer.PushClip(bar);
    for (plUInt32 i = 0; i < 4; ++i)
    {
      const float fPart = static_cast<float>(parts[i] / fScale * bar.width);
      if (fPart <= 0.0f)
        continue;

      m_Writer.Rect(plRectFloat(x, bar.y, fPart, bar.height), colors[i]);

      plStringBuilder sTip;
      sTip.SetFormat("{}: {} MB", szNames[i], plArgF(parts[i] / s_fMegabyte, 2));
      AddHit(Hit::Kind::Row, plRectFloat(x, bar.y, fPart, bar.height), -2, sTip);
      x += fPart;
    }
    m_Writer.PopClip();

    if (budget.m_uiTotalBytes > 0)
    {
      const float fLine = bar.x + static_cast<float>(budget.m_uiTotalBytes / fScale * bar.width);
      m_Writer.Rect(plRectFloat(fLine - 1.0f, bar.y - 3.0f, 2.0f, bar.height + 6.0f), plColorGammaUB(255, 255, 255));
    }

    // the legend
    float fLegend = s_fPad;
    const float fLegendY = bar.y + bar.height + 6.0f;
    for (plUInt32 i = 0; i < 5; ++i)
    {
      const plStringView sName = i < 4 ? plStringView(szNames[i]) : plStringView("Budget");
      const plColorGammaUB color = i < 4 ? colors[i] : plColorGammaUB(255, 255, 255);
      m_Writer.Rect(plRectFloat(fLegend, fLegendY + 3.0f, 8.0f, 8.0f), color, 2.0f);

      const float fName = MeasureText(sName, MinFontSize + 1);
      m_Writer.Text(plRectFloat(fLegend + 13.0f, fLegendY, fName + 2.0f, 14.0f), sName, Muted(), MinFontSize + 1);
      fLegend += 13.0f + fName + 14.0f;
    }

    // the engine process's own measurement, on the editor's platform
    if (m_Model.m_bEngineAvailable)
    {
      plStringBuilder sMeasured;
      sMeasured.SetFormat("engine process: {} MB measured", plArgF(m_Model.m_Measured.GetTotalBytes() / s_fMegabyte, 1));
      m_Writer.Text(plRectFloat(s_fPad, fLegendY, fWidth, 14.0f), sMeasured, Dim(), MinFontSize + 1, plUiTextHAlign::Right);
    }
  }

  m_Writer.Rect(plRectFloat(0.0f, s_fBodyTop - 1.0f, size.x, 1.0f), Line());
}

void plEditorUiAudioBankDashboard::DrawBanks(const plRectFloat& area)
{
  using namespace plEditorUi;

  plStringBuilder sCount;
  sCount.SetFormat("{}", m_Model.m_Banks.GetCount());
  plAudioEngineUi::SectionHeader(m_Writer, plRectFloat(area.x, area.y, area.width, plAudioEngineUi::SectionHeight), "Banks", plAudioEngineUi::AudioColor(), sCount);

  m_Writer.PushClip(area);

  float y = area.y + plAudioEngineUi::SectionHeight + 2.0f;
  for (plUInt32 i = 0; i < m_Model.m_Banks.GetCount(); ++i)
  {
    const plAudioBankDashboard::Bank& bank = m_Model.m_Banks[i];
    const plRectFloat row(area.x, y, area.width, s_fBankRow);

    if (static_cast<plInt32>(i) == m_iSelectedBank)
      m_Writer.Rect(row, Selection());

    plColorGammaUB stateColor = Dim();
    plStringBuilder sState = "off";
    switch (bank.m_State)
    {
      case plAudioBankDashboard::BankState::Loaded:
        stateColor = s_OkColor;
        sState = "ready";
        break;
      case plAudioBankDashboard::BankState::Loading:
        stateColor = Warning();
        sState.SetFormat("{}%", static_cast<plInt32>(bank.m_fProgress * 100.0f));
        break;
      case plAudioBankDashboard::BankState::Missing:
        stateColor = Danger();
        sState = "missing";
        break;
      default:
        break;
    }

    m_Writer.Circle(plVec2(row.x + 14.0f, row.y + row.height * 0.5f), 3.5f, stateColor);

    plStringBuilder sSize;
    sSize.SetFormat("{}", plArgF(bank.m_uiClipBytes / s_fMegabyte, 1));

    const float fBadge = MeasureText(sState, MinFontSize + 1) + 12.0f;
    const float fBadgeX = row.x + row.width - s_fPad - fBadge;
    Badge(m_Writer, plVec2(fBadgeX, row.y + 5.0f), sState, stateColor);
    m_Writer.Text(plRectFloat(fBadgeX - 54.0f, row.y, 48.0f, row.height), sSize, Muted(), FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);

    const float fNameX = row.x + 26.0f;
    const plString sName = Elide(bank.m_sName, fBadgeX - 60.0f - fNameX, FontSize - 1);
    m_Writer.Text(plRectFloat(fNameX, row.y, fBadgeX - 60.0f - fNameX, row.height), sName, Foreground(), FontSize - 1);

    plStringBuilder sTip;
    sTip.SetFormat("{}{} \xC2\xB7 {} items \xC2\xB7 clips {} MB on {}", bank.m_sName, bank.m_bThisDocument ? " (this window)" : "", bank.m_Items.GetCount(), plArgF(bank.m_uiClipBytes / s_fMegabyte, 2),
      m_Model.m_Profiles.IsEmpty() ? plStringView("this platform") : m_Model.m_Profiles[m_Model.m_uiProfile].GetView());
    if (bank.m_bPacked)
      sTip.Append(" \xC2\xB7 packed");
    AddHit(Hit::Kind::Bank, row, static_cast<plInt32>(i), sTip);

    y += s_fBankRow;
  }

  m_Writer.PopClip();
}

void plEditorUiAudioBankDashboard::DrawWarnings(const plRectFloat& area)
{
  using namespace plEditorUi;

  const plUInt32 uiCount = m_Model.m_Warnings.GetCount();

  plStringBuilder sCount;
  sCount.SetFormat("{}", uiCount);
  plAudioEngineUi::SectionHeader(m_Writer, plRectFloat(area.x, area.y, area.width, plAudioEngineUi::SectionHeight), "Warnings", uiCount > 0 ? Warning() : Dim(), sCount);

  m_WarningRows = plRectFloat(area.x, area.y + plAudioEngineUi::SectionHeight, area.width, plMath::Max(area.height - plAudioEngineUi::SectionHeight, 0.0f));
  const float fVisible = plMath::Floor(m_WarningRows.height / s_fWarningRow);
  m_fWarningScroll = plMath::Clamp(m_fWarningScroll, 0.0f, plMath::Max(uiCount - fVisible, 0.0f));

  if (uiCount == 0)
  {
    m_Writer.Text(plRectFloat(m_WarningRows.x + s_fPad, m_WarningRows.y + 6.0f, m_WarningRows.width - 2.0f * s_fPad, 16.0f), "No problems found", Dim(), FontSize - 1);
    return;
  }

  m_Writer.PushClip(m_WarningRows);

  const float fTextX = m_WarningRows.x + 26.0f;
  const float fTextWidth = m_WarningRows.width - 26.0f - s_fPad;

  float y = m_WarningRows.y - m_fWarningScroll * s_fWarningRow;
  for (plUInt32 i = 0; i < uiCount; ++i, y += s_fWarningRow)
  {
    if (y + s_fWarningRow < m_WarningRows.y || y > m_WarningRows.y + m_WarningRows.height)
      continue;

    const plAudioBankDashboard::Warning& warning = m_Model.m_Warnings[i];
    const plRectFloat row(m_WarningRows.x, y, m_WarningRows.width, s_fWarningRow);

    if (m_iHoverRow == -100 - static_cast<plInt32>(i))
      m_Writer.Rect(row, Raised());

    // a triangle for an error, a dot for a warning
    const plColorGammaUB color = warning.m_bError ? Danger() : Warning();
    if (warning.m_bError)
    {
      const plVec2 c(row.x + 13.0f, row.y + 11.0f);
      const plVec2 triangle[] = {plVec2(c.x, c.y - 5.0f), plVec2(c.x + 5.5f, c.y + 4.5f), plVec2(c.x - 5.5f, c.y + 4.5f)};
      m_Writer.Triangles(triangle, color);
    }
    else
    {
      m_Writer.Circle(plVec2(row.x + 13.0f, row.y + 11.0f), 3.5f, color);
    }

    plStringBuilder sFirst, sSecond;
    WrapTwoLines(warning.m_sText, fTextWidth, FontSize - 2, sFirst, sSecond);
    m_Writer.Text(plRectFloat(fTextX, row.y + 4.0f, fTextWidth, 14.0f), sFirst, Foreground(), FontSize - 2);
    if (!sSecond.IsEmpty())
      m_Writer.Text(plRectFloat(fTextX, row.y + 18.0f, fTextWidth, 14.0f), sSecond, Foreground(), FontSize - 2);

    plStringBuilder sTip = warning.m_sText;
    if (!warning.m_sAsset.IsEmpty())
      sTip.Append(" \xC2\xB7 double-click opens the asset");
    AddHit(Hit::Kind::Warning, plRectFloat::MakeIntersection(row, m_WarningRows), static_cast<plInt32>(i), sTip);
  }

  m_Writer.PopClip();
}

void plEditorUiAudioBankDashboard::DrawContents(const plRectFloat& area)
{
  using namespace plEditorUi;

  const plAudioBankDashboard::Bank* pBank = GetSelectedBank();
  if (pBank == nullptr)
  {
    m_Writer.Text(plRectFloat(area.x + s_fPad, area.y + 10.0f, area.width - 2.0f * s_fPad, 16.0f), "The project has no audio banks yet", Dim(), FontSize - 1);
    return;
  }

  // the bank's name, its state, Open and Load
  {
    const float y = area.y + (s_fContentHeader - ControlHeight) * 0.5f;
    float x = area.x + s_fPad;

    const float fName = MeasureText(pBank->m_sName, FontSize);
    m_Writer.Text(plRectFloat(x, y, fName + 2.0f, ControlHeight), pBank->m_sName, Foreground(), FontSize);
    x += fName + 10.0f;

    plStringBuilder sState;
    plColorGammaUB stateColor = Dim();
    switch (pBank->m_State)
    {
      case plAudioBankDashboard::BankState::Loaded:
        sState = "ready: all clips resident";
        stateColor = s_OkColor;
        break;
      case plAudioBankDashboard::BankState::Loading:
        sState.SetFormat("loading {}% (clips not ready yet)", static_cast<plInt32>(pBank->m_fProgress * 100.0f));
        stateColor = Warning();
        break;
      case plAudioBankDashboard::BankState::Missing:
        sState = "not built: transform the bank";
        stateColor = Danger();
        break;
      default:
        sState = "not loaded";
        break;
    }

    x += Badge(m_Writer, plVec2(x, y + 4.0f), sState, stateColor) + 6.0f;
    if (pBank->m_bPacked)
      x += Badge(m_Writer, plVec2(x, y + 4.0f), "packed", Muted()) + 6.0f;
    if (pBank->m_bOverBudget)
      Badge(m_Writer, plVec2(x, y + 4.0f), "over budget", Danger());

    // Load or Unload, in the engine process; a bank the scene loaded is the scene's to unload
    const bool bLoaded = pBank->m_State != plAudioBankDashboard::BankState::NotLoaded;
    const bool bEnabled = m_Model.m_bEngineAvailable && (!bLoaded || pBank->m_bLoadedByWindow) && pBank->m_State != plAudioBankDashboard::BankState::Loading;
    const plStringView sLoad = bLoaded ? plStringView("Unload") : plStringView("Load");
    const float fLoad = plAudioEngineUi::TextButtonWidth(sLoad, false);
    const plRectFloat load(area.x + area.width - s_fPad - fLoad, y, fLoad, ControlHeight);
    plAudioEngineUi::TextButton(m_Writer, load, sLoad, {}, bEnabled);

    plStringView sLoadTip = bLoaded ? plStringView("Unloads the bank from the editor's engine process") : plStringView("Loads the bank (as built) in the editor's engine process");
    if (!m_Model.m_bEngineAvailable)
      sLoadTip = "The engine process has no PlasmaAudio output";
    else if (bLoaded && !pBank->m_bLoadedByWindow)
      sLoadTip = "Loaded by the running scene";
    AddHit(Hit::Kind::Load, load, bEnabled ? 1 : 0, sLoadTip);

    if (!pBank->m_bThisDocument)
    {
      const float fOpen = plAudioEngineUi::TextButtonWidth("Open", false);
      const plRectFloat open(load.x - 6.0f - fOpen, y, fOpen, ControlHeight);
      plAudioEngineUi::TextButton(m_Writer, open, "Open");
      AddHit(Hit::Kind::Open, open, -1, "Opens the bank to edit it");
    }
  }

  // the table
  const float fTableTop = area.y + s_fContentHeader;
  const float fWidth = area.width - 2.0f * s_fPad;
  const float fType = 56.0f, fMode = 150.0f, fSize = 78.0f, fBanks = 86.0f, fStatus = 90.0f;
  const float fAsset = plMath::Max(fWidth - fType - fMode - fSize - fBanks - fStatus, 80.0f);
  const float columns[] = {area.x + s_fPad, area.x + s_fPad + fAsset, area.x + s_fPad + fAsset + fType, area.x + s_fPad + fAsset + fType + fMode,
    area.x + s_fPad + fAsset + fType + fMode + fSize, area.x + s_fPad + fAsset + fType + fMode + fSize + fBanks};

  m_Writer.Rect(plRectFloat(area.x, fTableTop, area.width, s_fRow), Background());
  {
    const char* szHeads[] = {"ASSET", "TYPE", "LOAD MODE", "SIZE", "BANKS", "STATUS"};
    for (plUInt32 c = 0; c < 6; ++c)
    {
      const bool bRight = c == 3;
      const float fColumnWidth = (c == 3 ? fSize : 80.0f) - 10.0f;
      m_Writer.Text(plRectFloat(columns[c], fTableTop, fColumnWidth, s_fRow), szHeads[c], Muted(), MinFontSize + 1, bRight ? plUiTextHAlign::Right : plUiTextHAlign::Left);
    }
  }

  const bool bNote = pBank->m_uiExcludedEventClips > 0;
  const plUInt32 uiRows = pBank->m_Items.GetCount() + (bNote ? 1 : 0);

  m_ContentRows = plRectFloat(area.x, fTableTop + s_fRow, area.width, plMath::Max(area.y + area.height - fTableTop - s_fRow, 0.0f));
  const float fVisible = plMath::Floor(m_ContentRows.height / s_fRow);
  m_fContentScroll = plMath::Clamp(m_fContentScroll, 0.0f, plMath::Max(uiRows - fVisible, 0.0f));

  m_Writer.PushClip(m_ContentRows);

  float y = m_ContentRows.y - m_fContentScroll * s_fRow;
  for (plUInt32 i = 0; i < pBank->m_Items.GetCount(); ++i, y += s_fRow)
  {
    if (y + s_fRow < m_ContentRows.y || y > m_ContentRows.y + m_ContentRows.height)
      continue;

    const plAudioBankDashboard::Item& item = pBank->m_Items[i];
    const plRectFloat row(area.x, y, area.width, s_fRow);

    if (!m_sHighlightedAsset.IsEmpty() && item.m_sGuid == m_sHighlightedAsset)
      m_Writer.Rect(row, Selection());
    else if (m_iHoverRow == static_cast<plInt32>(i))
      m_Writer.Rect(row, Raised());

    m_Writer.Rect(plRectFloat(row.x, row.y + row.height - 1.0f, row.width, 1.0f), Line());

    // asset: an arrow for an auto-included clip, the language when it is localised
    {
      float x = columns[0];
      if (item.m_bAuto)
      {
        m_Writer.Text(plRectFloat(x, y, 14.0f, s_fRow), "\xE2\x86\xB3", Dim(), FontSize - 1);
        x += 14.0f;
      }

      const float fBadge = item.m_bLocalized ? MeasureText(LanguageName(m_Model.m_sLanguage), MinFontSize + 1) + 18.0f : 0.0f;
      const float fNameWidth = columns[1] - x - 8.0f - fBadge;
      const plString sName = Elide(item.m_sName, fNameWidth, FontSize - 1);
      m_Writer.Text(plRectFloat(x, y, fNameWidth, s_fRow), sName, item.m_bUnknown ? Danger() : Foreground(), FontSize - 1);

      if (item.m_bLocalized)
        Badge(m_Writer, plVec2(x + MeasureText(sName, FontSize - 1) + 6.0f, y + 3.0f), LanguageName(m_Model.m_sLanguage), item.m_bMissingLanguage ? Danger() : Muted());
    }

    m_Writer.Text(plRectFloat(columns[1], y, fType - 6.0f, s_fRow), item.m_bClip ? "Clip" : "Event", Muted(), FontSize - 1);
    m_Writer.Text(plRectFloat(columns[2], y, fMode - 6.0f, s_fRow), item.m_bClip && !item.m_bUnknown ? LoadModeName(item.m_eLoadMode) : plStringView("-"), item.m_bAssumedSettings ? Dim() : Muted(), FontSize - 1);

    plStringBuilder sBytes;
    if (!item.m_bUnknown)
      plAudioEngineUi::FormatBytes(item.m_uiBytes, sBytes);
    m_Writer.Text(plRectFloat(columns[3], y, fSize - 10.0f, s_fRow), sBytes, Foreground(), FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);

    if (item.m_uiBanks > 1)
    {
      plStringBuilder sShared;
      sShared.SetFormat("shared \xC2\xB7 {}", item.m_uiBanks);
      Badge(m_Writer, plVec2(columns[4], y + 3.0f), sShared, Warning());
    }
    else
    {
      m_Writer.Text(plRectFloat(columns[4], y, fBanks - 6.0f, s_fRow), "unique", Dim(), FontSize - 1);
    }

    {
      plStringBuilder sStatus = "-";
      plColorGammaUB statusColor = Dim();
      if (item.m_bUnknown)
      {
        sStatus = "unknown";
        statusColor = Danger();
      }
      else if (item.m_bMissingLanguage)
      {
        sStatus.SetFormat("missing {}", m_Model.m_sLanguage);
        statusColor = Danger();
      }
      else if (pBank->m_State == plAudioBankDashboard::BankState::Loaded)
      {
        sStatus = "ready";
        statusColor = s_OkColor;
      }
      else if (pBank->m_State == plAudioBankDashboard::BankState::Loading)
      {
        sStatus = "loading";
        statusColor = Warning();
      }

      m_Writer.Text(plRectFloat(columns[5], y, fStatus, s_fRow), sStatus, statusColor, FontSize - 1);
    }

    plStringBuilder sTip;
    sTip.SetFormat("{} \xC2\xB7 double-click opens it", item.m_sName);
    if (item.m_bAuto)
      sTip.Append(" \xC2\xB7 included because an event of the bank plays it");
    if (item.m_bAssumedSettings)
      sTip.Append(" \xC2\xB7 size from the default settings: save the clip to publish its own");
    AddHit(Hit::Kind::Row, plRectFloat::MakeIntersection(row, m_ContentRows), static_cast<plInt32>(i), sTip);
  }

  if (bNote)
  {
    plStringBuilder sNote;
    sNote.SetFormat("{} event clip(s) not included; turn on Auto-include or add them by hand", pBank->m_uiExcludedEventClips);
    m_Writer.Text(plRectFloat(columns[0], y, fWidth, s_fRow), sNote, Dim(), FontSize - 1);
  }

  m_Writer.PopClip();
}

void plEditorUiAudioBankDashboard::DrawStatusRow(const plVec2& size)
{
  using namespace plEditorUi;

  const float y = size.y - s_fStatusHeight;
  m_Writer.Rect(plRectFloat(0.0f, y, size.x, 1.0f), Line());

  plStringBuilder sLeft;
  sLeft.SetFormat("{} banks \xC2\xB7 {} assets \xC2\xB7 sizes of the built clips, estimated from their settings where a platform is not built", m_Model.m_Banks.GetCount(), m_Model.m_uiAssets);
  m_Writer.Text(plRectFloat(s_fPad, y + 1.0f, size.x - 2.0f * s_fPad, s_fStatusHeight - 1.0f), sLeft, Dim(), FontSize - 2);
}

plString plEditorUiAudioBankDashboard::GetToolTipAt(const plVec2& vLogical) const
{
  if (const Hit* pHit = FindHit(vLogical))
  {
    if (!pHit->m_sToolTip.IsEmpty())
      return pHit->m_sToolTip;
  }

  return plEditorUiContent::GetToolTipAt(vLogical);
}

//////////////////////////////////////////////////////////////////////////
// Input

void plEditorUiAudioBankDashboard::OnPointer(const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;

  if (e.m_Type == Type::Wheel)
  {
    if (m_ContentRows.Contains(e.m_vPosition))
      m_fContentScroll -= e.m_fWheel * 3.0f;
    else if (m_WarningRows.Contains(e.m_vPosition))
      m_fWarningScroll -= e.m_fWheel;
    else
      return;

    m_bDrawDirty = true;
    RequestRedraw();
    return;
  }

  const Hit* pHit = FindHit(e.m_vPosition);

  if (e.m_Type == Type::Move)
  {
    plInt32 iHover = -1;
    if (pHit != nullptr && pHit->m_Kind == Hit::Kind::Row && pHit->m_iIndex >= 0)
      iHover = pHit->m_iIndex;
    else if (pHit != nullptr && pHit->m_Kind == Hit::Kind::Warning)
      iHover = -100 - pHit->m_iIndex;

    if (iHover != m_iHoverRow)
    {
      m_iHoverRow = iHover;
      m_bDrawDirty = true;
      RequestRedraw();
    }
    return;
  }

  if (pHit == nullptr || e.m_Button != plEditorUiPointerEvent::Button::Left)
    return;

  if (e.m_Type == Type::DoubleClick && pHit->m_Kind == Hit::Kind::Row && pHit->m_iIndex >= 0)
  {
    if (const plAudioBankDashboard::Bank* pBank = GetSelectedBank())
      OpenAsset(pBank->m_Items[pHit->m_iIndex].m_sGuid);
    return;
  }

  if (e.m_Type == Type::DoubleClick && pHit->m_Kind == Hit::Kind::Warning && pHit->m_iIndex >= 0)
  {
    if (!m_Model.m_Warnings[pHit->m_iIndex].m_sAsset.IsEmpty())
      OpenAsset(m_Model.m_Warnings[pHit->m_iIndex].m_sAsset);
    return;
  }

  if (e.m_Type == Type::Press)
    OnClick(*pHit);
}

void plEditorUiAudioBankDashboard::OnClick(const Hit& hit)
{
  const plAudioBankDashboard::Bank* pBank = GetSelectedBank();

  switch (hit.m_Kind)
  {
    case Hit::Kind::Platform:
      m_Model.m_uiProfile = static_cast<plUInt32>(hit.m_iIndex);
      Rebuild();
      return;

    case Hit::Kind::Language:
      ShowLanguageMenu(hit.m_Rect);
      return;

    case Hit::Kind::AutoInclude:
      SetAutoInclude(!m_pDocument->GetProperties()->m_bAutoIncludeEventClips);
      return;

    case Hit::Kind::Bank:
      m_iSelectedBank = hit.m_iIndex;
      m_sHighlightedAsset.Clear();
      m_fContentScroll = 0.0f;
      break;

    case Hit::Kind::Warning:
      SelectWarning(static_cast<plUInt32>(hit.m_iIndex));
      break;

    case Hit::Kind::Load:
      if (pBank != nullptr && hit.m_iIndex == 1)
        m_pDocument->SendBankCommand(pBank->m_State == plAudioBankDashboard::BankState::NotLoaded ? plAudioBankCommand::Load : plAudioBankCommand::Unload, pBank->m_sGuid);
      return;

    case Hit::Kind::Open:
      if (pBank != nullptr)
        OpenAsset(pBank->m_sGuid);
      return;

    case Hit::Kind::Row:
      if (pBank != nullptr && hit.m_iIndex >= 0)
        m_sHighlightedAsset = pBank->m_Items[hit.m_iIndex].m_sGuid;
      break;
  }

  m_bDrawDirty = true;
  RequestRedraw();
}

void plEditorUiAudioBankDashboard::ShowLanguageMenu(const plRectFloat& rect)
{
  plEditorUiMenu menu;
  for (plUInt32 i = 0; i < m_Model.m_Languages.GetCount(); ++i)
  {
    plEditorUiMenuItem& item = menu.AddAction(LanguageName(m_Model.m_Languages[i]), i);
    item.m_bCheckable = true;
    item.m_bChecked = m_Model.m_Languages[i] == m_Model.m_sLanguage;
  }

  plEditorUiMenuView::Show(*GetSurface(), plVec2(rect.x, rect.y + rect.height), menu, [this](plUInt32 uiId)
    {
      if (uiId < m_Model.m_Languages.GetCount())
      {
        m_Model.m_sLanguage = m_Model.m_Languages[uiId];
        Rebuild();
      } });
}

void plEditorUiAudioBankDashboard::SetAutoInclude(bool bAutoInclude)
{
  const auto& children = m_pDocument->GetObjectManager()->GetRootObject()->GetChildren();
  if (children.IsEmpty())
    return;

  plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
  accessor.StartTransaction(bAutoInclude ? "Auto-include Event Clips" : "Do Not Auto-include Event Clips");
  accessor.SetValueByName(children[0], "AutoIncludeEventClips", bAutoInclude).IgnoreResult();
  accessor.FinishTransaction();
}

void plEditorUiAudioBankDashboard::SelectWarning(plUInt32 uiWarning)
{
  if (uiWarning >= m_Model.m_Warnings.GetCount())
    return;

  const plAudioBankDashboard::Warning& warning = m_Model.m_Warnings[uiWarning];
  if (warning.m_iBank >= 0)
    m_iSelectedBank = warning.m_iBank;

  m_sHighlightedAsset = warning.m_sAsset;

  // scroll the asset into view
  if (const plAudioBankDashboard::Bank* pBank = GetSelectedBank())
  {
    for (plUInt32 i = 0; i < pBank->m_Items.GetCount(); ++i)
    {
      if (pBank->m_Items[i].m_sGuid == m_sHighlightedAsset)
      {
        const float fVisible = plMath::Max(plMath::Floor(m_ContentRows.height / s_fRow), 1.0f);
        if (i < m_fContentScroll || i >= m_fContentScroll + fVisible)
          m_fContentScroll = static_cast<float>(i);
      }
    }
  }

}

void plEditorUiAudioBankDashboard::OpenAsset(plStringView sGuid)
{
  plString sPath;
  {
    const auto pSubAsset = plAssetCurator::GetSingleton()->FindSubAsset(sGuid);
    if (!pSubAsset.isValid() || pSubAsset->m_pAssetInfo == nullptr)
      return;

    sPath = pSubAsset->m_pAssetInfo->m_Path.GetAbsolutePath();
  }

  // a new document frame subscribes to the platform events this click is being dispatched by; open after it
  QTimer::singleShot(0, [sPath]()
    { plQtEditorApp::GetSingleton()->OpenDocument(sPath, plDocumentFlags::RequestWindow | plDocumentFlags::AddToRecentFilesList); });
}
