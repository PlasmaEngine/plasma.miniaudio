#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/Profiler/AudioProfilerView.h>
#include <EditorUi/Drawings.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Tabs.h>
#include <EditorUi/Widgets.h>
#include <GameUiPlugin/Components/UiCanvasComponent.h>
#include <PlasmaAudioPlugin/Codecs/AudioCodec.h>
#include <PlasmaAudioPlugin/Profiling/AudioEventLog.h>

namespace
{
  constexpr float s_fHeaderRow = 30.0f;
  constexpr float s_fTabRow = 30.0f;
  constexpr float s_fToolRow = 30.0f;
  constexpr float s_fStatus = plEditorUi::StatusRowHeight + 1.0f;
  constexpr float s_fTableHeader = 26.0f;
  constexpr float s_fRow = plEditorUi::RowHeight;
  constexpr float s_fDetailsWidth = 260.0f;
  constexpr float s_fTotalsRow = 28.0f;
  constexpr float s_fSearchWidth = 220.0f;
  constexpr float s_fWheelStep = 3.0f * plEditorUi::RowHeight;
  constexpr plUInt32 s_uiMaxRows = 200;
  constexpr plUInt32 s_uiMaxLogLines = 2000;

  constexpr float s_fBusStripWidth = 86.0f;
  constexpr float s_fBusStripGap = 6.0f;
  constexpr float s_fBusMeterHeight = 120.0f;

  const plHashedString s_sDrawing = plMakeHashedString("Profiler.Drawing");

  constexpr const char* s_szStateNames[3] = {"real", "virtual", "stopping"};
  constexpr const char* s_szLogCategoryNames[4] = {"lifecycle", "voices", "warnings", "system"};
  constexpr const char* s_szLogCategoryTips[4] = {
    "Instance starts, stops and ends",
    "Voices culled at start, virtualised and realised",
    "Stolen instances, rejected starts, killed voices, missing clips, lost devices",
    "Clip load waits, device recovery, mixer, bank and language changes",
  };

  constexpr const char* s_szDetailNames[] = {"Entity", "State", "Priority", "Audibility", "Distance", "Bus", "Codec", "Age", "Occlusion"};

  plUInt32 GetLogCategory(plUInt8 uiType)
  {
    const auto type = static_cast<plAudioLogEventType::Enum>(uiType);
    if (plAudioLogEventType::IsWarning(type))
      return 2;

    switch (type)
    {
      case plAudioLogEventType::Start:
      case plAudioLogEventType::Stop:
      case plAudioLogEventType::Finished:
        return 0;
      case plAudioLogEventType::CulledAtStart:
      case plAudioLogEventType::Virtualised:
      case plAudioLogEventType::Realised:
        return 1;
      default:
        return 3;
    }
  }

  /// Voice states as system colours: real in the accent, virtual in the XR colour, stopping in the warning colour.
  plColorGammaUB StateColor(plUInt8 uiState)
  {
    switch (uiState)
    {
      case plAudioVoiceSnapshotEntry::Real:
        return plEditorUi::Accent();
      case plAudioVoiceSnapshotEntry::Virtual:
        return plAudioEngineUi::CategoryColor("XR");
      default:
        return plEditorUi::Warning();
    }
  }

  plStringView CodecName(plUInt8 uiCodec)
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

  plStringView LoadModeName(plUInt8 uiLoadMode)
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

  plString CodecText(const plAudioVoiceSnapshotEntry& voice)
  {
    if (voice.m_uiCodec == plAudioVoiceSnapshotEntry::Embedded)
      return "Embedded";

    plStringBuilder sCodec = CodecName(voice.m_uiCodec);
    if (voice.m_uiLoadMode == plAudioClipLoadMode::Streamed)
      sCodec.Append(" stream");
    else if (voice.m_uiLoadMode == plAudioClipLoadMode::CompressedInMemory)
      sCodec.Append(" compressed");

    return sCodec;
  }

  /// The event's file name without extension when the name is a path, else the name as it is.
  plString ShortName(const plString& sName)
  {
    const plStringView sView = sName.GetView();
    if (sView.FindLastSubString("/") != nullptr || sView.FindLastSubString("\\") != nullptr)
      return sView.GetFileName();

    return sName;
  }

  plString EntityText(const plAudioVoiceSnapshotEntry& voice)
  {
    if (voice.m_uiEntityId == 0)
      return "-";

    plStringBuilder s;
    s.SetFormat("#{}", voice.m_uiEntityId);
    return s;
  }

  plString IndexedName(const char* szPrefix, plUInt32 uiIndex)
  {
    plStringBuilder s;
    s.SetFormat("{}.{}", szPrefix, uiIndex);
    return s;
  }

  /// Column x positions for widths, where a width of 0 takes the remaining space.
  void LayoutColumns(float x, float fWidth, plArrayPtr<const float> widths, plHybridArray<float, 8>& out_x, plHybridArray<float, 8>& out_w)
  {
    float fFixed = 0.0f;
    for (float w : widths)
      fFixed += w;

    const float fStretch = plMath::Max(fWidth - fFixed, 60.0f);
    out_x.Clear();
    out_w.Clear();
    for (float w : widths)
    {
      out_x.PushBack(x);
      out_w.PushBack(w > 0.0f ? w : fStretch);
      x += out_w.PeekBack();
    }
  }
} // namespace

plEditorUiAudioProfilerView::plEditorUiAudioProfilerView()
  : plEditorUiContent("AudioProfiler")
{
  if (IsReady())
  {
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioProfilerView::OnPointer, this));

    m_pVoiceSearch = PL_DEFAULT_NEW(plEditorUiSearchField, *GetSurface(), "Ap.VoiceSearch");
    m_VoiceSearchSubscription = m_pVoiceSearch->m_Events.AddEventHandler(plMakeDelegate(&plEditorUiAudioProfilerView::OnSearch, this));

    m_pLogSearch = PL_DEFAULT_NEW(plEditorUiSearchField, *GetSurface(), "Ap.LogSearch");
    m_LogSearchSubscription = m_pLogSearch->m_Events.AddEventHandler(plMakeDelegate(&plEditorUiAudioProfilerView::OnSearch, this));
  }
}

plEditorUiAudioProfilerView::~plEditorUiAudioProfilerView()
{
  if (m_OnDestroyed.IsValid())
    m_OnDestroyed(this);

  if (IsReady())
  {
    m_pLogSearch->m_Events.RemoveEventHandler(m_LogSearchSubscription);
    m_pVoiceSearch->m_Events.RemoveEventHandler(m_VoiceSearchSubscription);
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
  }
}

void plEditorUiAudioProfilerView::SetLinkState(LinkState state)
{
  if (m_LinkState == state)
    return;

  m_LinkState = state;
  m_bDrawDirty = true;
}

void plEditorUiAudioProfilerView::SetSnapshot(const plAudioProfilingSnapshot& snapshot, plArrayPtr<const plAudioProfilerLogLine> newLines)
{
  m_Snapshot = snapshot;
  m_bHasSnapshot = true;

  for (const plAudioProfilerLogLine& line : newLines)
  {
    if (line.m_uiType != plAudioProfilerLogLine::GapType)
      ++m_uiLogReceived;

    m_LogLines.PushBack(line);
    if (m_LogLines.GetCount() > s_uiMaxLogLines)
      m_LogLines.PopFront();
  }

  RebuildVoiceRows();
  RebuildMemoryRows();
  RebuildVisibleLog();

  m_bDrawDirty = true;
}

//////////////////////////////////////////////////////////////////////////
// Data

void plEditorUiAudioProfilerView::RebuildVoiceRows()
{
  m_VoiceRows.Clear();

  for (const plAudioVoiceSnapshotEntry& voice : m_Snapshot.m_Voices)
  {
    if (!m_StateFilter[plMath::Min<plUInt8>(voice.m_uiState, 2)])
      continue;

    if (!m_sVoiceFilter.IsEmpty() && voice.m_sEvent.FindSubString_NoCase(m_sVoiceFilter) == nullptr)
      continue;

    m_VoiceRows.PushBack(&voice);
  }

  // real voices first, then by audibility; the id keeps the order stable
  m_VoiceRows.Sort([](const plAudioVoiceSnapshotEntry* a, const plAudioVoiceSnapshotEntry* b)
    {
      const int iRankA = a->m_uiState == plAudioVoiceSnapshotEntry::Real ? 0 : 1;
      const int iRankB = b->m_uiState == plAudioVoiceSnapshotEntry::Real ? 0 : 1;
      if (iRankA != iRankB)
        return iRankA < iRankB;
      if (a->m_fAudibility != b->m_fAudibility)
        return a->m_fAudibility > b->m_fAudibility;
      return a->m_uiId < b->m_uiId; });

  if (m_VoiceRows.GetCount() > s_uiMaxRows)
    m_VoiceRows.SetCount(s_uiMaxRows);

  // the selected voice keeps its last values once it ended
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
}

void plEditorUiAudioProfilerView::RebuildMemoryRows()
{
  const plAudioProfilingSnapshot& s = m_Snapshot;
  m_MemoryRows.Clear();

  for (const plAudioClipMemorySnapshotEntry& clip : s.m_Clips)
  {
    MemoryRow& row = m_MemoryRows.ExpandAndGetRef();
    row.m_sName = ShortName(clip.m_sName);
    row.m_sToolTip = clip.m_sName;
    row.m_sType = "Clip";
    row.m_sLoadMode = LoadModeName(clip.m_uiLoadMode);
    row.m_sCodec = CodecName(clip.m_uiCodec);

    plStringBuilder sStreams;
    sStreams.SetFormat("{}", clip.m_uiStreams);
    row.m_sStreams = clip.m_uiLoadMode == plAudioClipLoadMode::Streamed ? plString(sStreams) : plString("-");
    row.m_uiResident = clip.m_uiResidentBytes;
  }

  for (const plAudioEventMemorySnapshotEntry& event : s.m_Events)
  {
    MemoryRow& row = m_MemoryRows.ExpandAndGetRef();
    row.m_sName = ShortName(event.m_sName);
    row.m_sToolTip = event.m_sName;
    row.m_sType = "Event";
    row.m_sLoadMode = event.m_uiSampleBytes > 0 ? "Embedded samples" : "-";
    row.m_sCodec = "-";
    row.m_sStreams = "-";
    row.m_uiResident = event.m_uiSampleBytes;
  }

  if (s.m_uiStreamRingBytes > 0 || s.m_uiStreams > 0)
  {
    MemoryRow& row = m_MemoryRows.ExpandAndGetRef();
    row.m_sName = "Stream buffers";

    plStringBuilder sTip;
    sTip.SetFormat("Rings of the open and pooled disk streams ({} open, {} pooled)", s.m_uiStreams, s.m_uiPooledStreams);
    row.m_sToolTip = sTip;
    row.m_sType = "Streams";
    row.m_sLoadMode = "-";
    row.m_sCodec = "-";

    plStringBuilder sStreams;
    sStreams.SetFormat("{}", s.m_uiStreams);
    row.m_sStreams = sStreams;
    row.m_uiResident = s.m_uiStreamRingBytes;
  }

  for (const plAudioBankMemorySnapshotEntry& bank : s.m_Banks)
  {
    MemoryRow& row = m_MemoryRows.ExpandAndGetRef();
    row.m_sName = ShortName(bank.m_sName);
    row.m_sToolTip = bank.m_sName;
    row.m_sType = "Bank";

    plStringBuilder sCodec;
    sCodec.SetFormat("{} events, {} clips", bank.m_uiEvents, bank.m_uiClips);
    row.m_sCodec = sCodec;
    row.m_sStreams = "-";
    row.m_bHasResident = false;

    switch (bank.m_uiState)
    {
      case plAudioBankMemorySnapshotEntry::Loaded:
        row.m_sLoadMode = "loaded";
        break;
      case plAudioBankMemorySnapshotEntry::Missing:
        row.m_sLoadMode = "missing";
        row.m_bWarning = true;
        break;
      default:
        row.m_sLoadMode = "loading";
        break;
    }
  }

  // largest first; banks (no resident bytes of their own) last
  m_MemoryRows.Sort([](const MemoryRow& a, const MemoryRow& b)
    {
      if (a.m_bHasResident != b.m_bHasResident)
        return a.m_bHasResident;
      if (a.m_uiResident != b.m_uiResident)
        return a.m_uiResident > b.m_uiResident;
      return a.m_sName < b.m_sName; });
}

bool plEditorUiAudioProfilerView::IsLogLineVisible(const plAudioProfilerLogLine& line) const
{
  if (line.m_uiType == plAudioProfilerLogLine::GapType)
    return true;

  if (!m_LogCategory[GetLogCategory(line.m_uiType)])
    return false;

  if (m_sLogFilter.IsEmpty())
    return true;

  return line.m_sName.FindSubString_NoCase(m_sLogFilter) != nullptr || line.m_sDetail.FindSubString_NoCase(m_sLogFilter) != nullptr ||
         plStringView(plAudioLogEventType::GetName(static_cast<plAudioLogEventType::Enum>(line.m_uiType))).FindSubString_NoCase(m_sLogFilter) != nullptr;
}

void plEditorUiAudioProfilerView::RebuildVisibleLog()
{
  m_VisibleLog.Clear();
  for (plUInt32 i = 0; i < m_LogLines.GetCount(); ++i)
  {
    if (IsLogLineVisible(m_LogLines[i]))
      m_VisibleLog.PushBack(i);
  }
}

//////////////////////////////////////////////////////////////////////////
// Layout

plRectFloat plEditorUiAudioProfilerView::GetBodyRect() const
{
  const plVec2 size = IsReady() ? GetSurface()->GetLogicalSize() : plVec2(800.0f, 400.0f);
  const float y = s_fHeaderRow + s_fTabRow;
  return plRectFloat(0.0f, y, size.x, plMath::Max(size.y - y - s_fStatus, 1.0f));
}

plRectFloat plEditorUiAudioProfilerView::GetToolRowRect() const
{
  const plRectFloat body = GetBodyRect();
  return plRectFloat(body.x, body.y, body.width, s_fToolRow);
}

plRectFloat plEditorUiAudioProfilerView::GetTableRect() const
{
  const plRectFloat body = GetBodyRect();

  switch (m_Tab)
  {
    case Tab::Voices:
      return plRectFloat(body.x, body.y + s_fToolRow, plMath::Max(body.width - s_fDetailsWidth, 100.0f), plMath::Max(body.height - s_fToolRow, 1.0f));
    case Tab::Memory:
      return plRectFloat(body.x, body.y, body.width, plMath::Max(body.height - s_fTotalsRow, 1.0f));
    case Tab::Log:
      return plRectFloat(body.x, body.y + s_fToolRow, body.width, plMath::Max(body.height - s_fToolRow, 1.0f));
    default:
      return plRectFloat(body.x, body.y + s_fToolRow, body.width, plMath::Max(body.height - s_fToolRow, 1.0f));
  }
}

plRectFloat plEditorUiAudioProfilerView::GetDetailsRect() const
{
  const plRectFloat table = GetTableRect();
  const plRectFloat body = GetBodyRect();
  return plRectFloat(table.x + table.width, table.y, body.width - table.width, table.height);
}

plString plEditorUiAudioProfilerView::GetTabLabel(Tab tab) const
{
  switch (tab)
  {
    case Tab::Voices:
      return "Voices";
    case Tab::Buses:
      return "Buses";
    case Tab::Memory:
      return "Memory";
    default:
      return "Event log";
  }
}

plString plEditorUiAudioProfilerView::GetTabCount(Tab tab) const
{
  plStringBuilder s;

  switch (tab)
  {
    case Tab::Voices:
      if (m_bHasSnapshot)
        s.SetFormat("{}", m_Snapshot.m_Voices.GetCount());
      break;

    case Tab::Buses:
      if (!m_Snapshot.m_Buses.IsEmpty())
        s.SetFormat("{}", m_Snapshot.m_Buses.GetCount());
      break;

    case Tab::Memory:
      if (m_bHasSnapshot)
      {
        // clip data and stream rings plus the embedded samples of the events, as the memory tab totals them
        plUInt64 uiMemory = m_Snapshot.m_uiMemoryBytes;
        for (const plAudioEventMemorySnapshotEntry& event : m_Snapshot.m_Events)
          uiMemory += event.m_uiSampleBytes;
        plAudioEngineUi::FormatBytes(uiMemory, s);
      }
      break;

    default:
      s.SetFormat("{}", m_uiLogReceived);
      break;
  }

  return s;
}

float plEditorUiAudioProfilerView::GetTabWidth(Tab tab) const
{
  plStringBuilder sLabel = GetTabLabel(tab);
  sLabel.ToUpper();

  float fWidth = 20.0f + plEditorUi::MeasureText(sLabel, plEditorUi::FontSize - 2);

  const plString sCount = GetTabCount(tab);
  if (!sCount.IsEmpty())
    fWidth += 6.0f + plEditorUi::MeasureCodeText(sCount, plEditorUi::MinFontSize) + 10.0f;

  return plMath::Ceil(fWidth);
}

//////////////////////////////////////////////////////////////////////////
// Document

void plEditorUiAudioProfilerView::OnUiUpdate()
{
  if (!IsReady())
    return;

  if (GetSurface()->GetLogicalSize() != m_vBuiltSize)
    m_bRebuild = true;

  // a tab whose count got wider moves the tabs after it
  for (plUInt32 i = 0; i < static_cast<plUInt32>(Tab::Count); ++i)
  {
    if (GetTabWidth(static_cast<Tab>(i)) != m_BuiltTabWidths[i])
      m_bRebuild = true;
  }

  // the document is not rebuilt while a search field is being typed into
  const plStringView sFocused = GetSurface()->GetCanvas().GetFocusedElementName();
  const bool bTyping = sFocused.StartsWith("Ap.VoiceSearch") || sFocused.StartsWith("Ap.LogSearch");

  if (m_bRebuild && (!bTyping || GetSurface()->GetLogicalSize() != m_vBuiltSize))
    BuildDocument();

  if (m_bDrawDirty)
    Draw();
}

void plEditorUiAudioProfilerView::BuildDocument()
{
  using namespace plEditorUi;

  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();
  plBlackboard& board = GetSurface()->GetBlackboard();

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = Surface();
  DrawList(pRoot, "Profiler.Drawing", s_sDrawing.GetView());

  // Pause
  plUiButton* pPause = IconButton(pRoot, Icon::Pause, "Ap.Pause");
  plAudioEngineUi::Place(pPause, plRectFloat(m_vBuiltSize.x - 32.0f, 3.0f, ControlHeight, ControlHeight));
  SetToolTip(board, "Ap.Pause", "Freeze the view and stop polling the engine process");

  // tabs
  float x = 6.0f;
  for (plUInt32 i = 0; i < static_cast<plUInt32>(Tab::Count); ++i)
  {
    const float fWidth = GetTabWidth(static_cast<Tab>(i));
    m_BuiltTabWidths[i] = fWidth;
    plAudioEngineUi::HitButton(pRoot, plRectFloat(x, s_fHeaderRow + 2.0f, fWidth, plEditorUiTabs::Height), IndexedName("Ap.Tab", i));
    x += fWidth;
  }

  const plRectFloat tool = GetToolRowRect();
  const float fToolY = tool.y + 3.0f;

  if (m_Tab == Tab::Voices)
  {
    x = 8.0f + MeasureText("State", FontSize - 1) + 10.0f;
    for (plUInt32 i = 0; i < 3; ++i)
    {
      const float fWidth = plAudioEngineUi::ChipWidth(s_szStateNames[i]);
      plStringBuilder sTip;
      sTip.SetFormat("Show {} voices", s_szStateNames[i]);
      plAudioEngineUi::HitButton(pRoot, plRectFloat(x, fToolY + 1.0f, fWidth, 22.0f), IndexedName("Ap.State", i), sTip, &board);
      x += fWidth + 4.0f;
    }

    plUiPanel* pSearch = Panel(pRoot);
    plAudioEngineUi::Place(pSearch, plRectFloat(x + 12.0f, fToolY, s_fSearchWidth, ControlHeight));
    pSearch->m_Color = plColorGammaUB(0, 0, 0, 0);
    m_pVoiceSearch->Build(pSearch, "filter event...");
    m_pVoiceSearch->SetText(m_sVoiceFilter);
  }
  else if (m_Tab == Tab::Log)
  {
    x = 8.0f + MeasureText("Type", FontSize - 1) + 10.0f;
    for (plUInt32 i = 0; i < LogCategoryCount; ++i)
    {
      const float fWidth = plAudioEngineUi::ChipWidth(s_szLogCategoryNames[i]);
      plAudioEngineUi::HitButton(pRoot, plRectFloat(x, fToolY + 1.0f, fWidth, 22.0f), IndexedName("Ap.LogType", i), s_szLogCategoryTips[i], &board);
      x += fWidth + 4.0f;
    }

    plUiPanel* pSearch = Panel(pRoot);
    plAudioEngineUi::Place(pSearch, plRectFloat(x + 12.0f, fToolY, s_fSearchWidth, ControlHeight));
    pSearch->m_Color = plColorGammaUB(0, 0, 0, 0);
    m_pLogSearch->Build(pSearch, "filter text...");
    m_pLogSearch->SetText(m_sLogFilter);

    const float fClear = plAudioEngineUi::TextButtonWidth("Clear", false);
    plAudioEngineUi::HitButton(pRoot, plRectFloat(x + 12.0f + s_fSearchWidth + 6.0f, fToolY, fClear, ControlHeight), "Ap.LogClear", "Remove all lines from the log", &board);
  }

  SetDocument(Finish(pRoot));
  m_bDrawDirty = true;
}

void plEditorUiAudioProfilerView::OnUiEvent(const plMsgUiEvent& event)
{
  const plStringView sId = event.m_sIdentifier.GetView();

  if (sId == "Ap.Pause")
  {
    m_bPaused = !m_bPaused;
    m_bDrawDirty = true;

    if (m_OnPauseToggled.IsValid())
      m_OnPauseToggled(m_bPaused);
    return;
  }

  if (sId == "Ap.LogClear")
  {
    // the sequence position stays with the panel, so cleared lines are not received again
    m_LogLines.Clear();
    m_VisibleLog.Clear();
    m_uiLogReceived = 0;
    m_fScroll = 0.0f;
    m_bDrawDirty = true;
    return;
  }

  auto indexOf = [&](const char* szPrefix) -> plInt32
  {
    const plStringBuilder sPrefix(szPrefix, ".");
    if (!sId.StartsWith(sPrefix))
      return -1;

    plStringView sIndex = sId;
    sIndex.Shrink(sPrefix.GetCharacterCount(), 0);

    plUInt32 uiIndex = 0;
    return plConversionUtils::StringToUInt(sIndex, uiIndex).Succeeded() ? static_cast<plInt32>(uiIndex) : -1;
  };

  if (const plInt32 iTab = indexOf("Ap.Tab"); iTab >= 0 && iTab < static_cast<plInt32>(Tab::Count))
  {
    if (static_cast<Tab>(iTab) != m_Tab)
    {
      m_Tab = static_cast<Tab>(iTab);
      m_fScroll = 0.0f;
      m_bFollowLog = true;
      m_bRebuild = true;
    }
  }
  else if (const plInt32 iState = indexOf("Ap.State"); iState >= 0 && iState < 3)
  {
    m_StateFilter[iState] = !m_StateFilter[iState];
    RebuildVoiceRows();
    m_bDrawDirty = true;
  }
  else if (const plInt32 iType = indexOf("Ap.LogType"); iType >= 0 && iType < static_cast<plInt32>(LogCategoryCount))
  {
    m_LogCategory[iType] = !m_LogCategory[iType];
    RebuildVisibleLog();
    m_bDrawDirty = true;
  }
}

void plEditorUiAudioProfilerView::OnSearch(const plEditorUiSearchFieldEvent& e)
{
  if (e.m_Type != plEditorUiSearchFieldEvent::Type::TextChanged)
    return;

  if (m_Tab == Tab::Voices)
  {
    plStringBuilder sText = m_pVoiceSearch->GetText();
    sText.Trim(" \t");
    m_sVoiceFilter = sText;
    RebuildVoiceRows();
  }
  else if (m_Tab == Tab::Log)
  {
    plStringBuilder sText = m_pLogSearch->GetText();
    sText.Trim(" \t");
    m_sLogFilter = sText;
    RebuildVisibleLog();
  }

  m_bDrawDirty = true;
}

//////////////////////////////////////////////////////////////////////////
// Drawing

void plEditorUiAudioProfilerView::Draw()
{
  m_bDrawDirty = false;
  m_Writer.Clear();

  ClampScroll();
  DrawHeader();

  switch (m_Tab)
  {
    case Tab::Voices:
      DrawVoices();
      break;
    case Tab::Buses:
      DrawBuses();
      break;
    case Tab::Memory:
      DrawMemory();
      break;
    default:
      DrawLog();
      break;
  }

  DrawStatus();
  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sDrawing);
}

void plEditorUiAudioProfilerView::DrawHeader()
{
  using namespace plEditorUi;

  const plVec2 size = GetSurface()->GetLogicalSize();

  // link state: a system colour dot and its text
  plColorGammaUB dot = Danger();
  plStringBuilder sLink;
  switch (m_LinkState)
  {
    case LinkState::NoEngine:
      sLink = "Engine process not running";
      break;
    case LinkState::NoData:
      dot = Warning();
      sLink = "Engine process \xC2\xB7 no PlasmaAudio data";
      break;
    case LinkState::Live:
      dot = Accent();
      sLink = "Engine process";
      break;
  }

  if (m_bPaused)
    sLink.Append(" \xC2\xB7 paused");

  plAudioEngineUi::DotText(m_Writer, plRectFloat(10.0f, 0.0f, size.x - 60.0f, s_fHeaderRow), sLink, dot, plAudioEngineUi::Mix(Foreground(), dot, 0.82f), FontSize - 1);

  // Pause: the plate of a pressed toggle
  if (m_bPaused)
    m_Writer.Rect(plRectFloat(size.x - 32.0f, 3.0f, ControlHeight, ControlHeight), Selection(), 4.0f);

  m_Writer.Rect(plRectFloat(0.0f, s_fHeaderRow - 1.0f, size.x, 1.0f), Line());

  // dock-style tabs with count pills
  float x = 6.0f;
  for (plUInt32 i = 0; i < static_cast<plUInt32>(Tab::Count); ++i)
  {
    const Tab tab = static_cast<Tab>(i);
    const float fWidth = GetTabWidth(tab);
    const bool bCurrent = tab == m_Tab;
    const plRectFloat rect(x, s_fHeaderRow + 2.0f, fWidth, plEditorUiTabs::Height);

    if (bCurrent)
    {
      m_Writer.Rect(rect, Surface(), 5.0f);
      m_Writer.Rect(plRectFloat(rect.x, rect.y + rect.height - 2.0f, rect.width, 2.0f), Accent());
    }

    plStringBuilder sLabel = GetTabLabel(tab);
    sLabel.ToUpper();
    const float fLabel = MeasureText(sLabel, FontSize - 2);
    m_Writer.Text(plRectFloat(rect.x + 10.0f, rect.y, fLabel + 2.0f, rect.height), sLabel, bCurrent ? Foreground() : Dim(), FontSize - 2);

    const plString sCount = GetTabCount(tab);
    if (!sCount.IsEmpty())
    {
      const plRectFloat pill(rect.x + 10.0f + fLabel + 6.0f, rect.y + rect.height * 0.5f - 8.0f, MeasureCodeText(sCount, MinFontSize) + 10.0f, 16.0f);
      m_Writer.Rect(pill, Raised(), 8.0f);
      m_Writer.Text(pill, sCount, Muted(), MinFontSize, plUiTextHAlign::Center, plUiTextVAlign::Center, true);
    }

    x += fWidth;
  }

  m_Writer.Rect(plRectFloat(0.0f, s_fHeaderRow + s_fTabRow - 1.0f, size.x, 1.0f), Line());
}

void plEditorUiAudioProfilerView::DrawTableHeader(const plRectFloat& rect, plArrayPtr<const char* const> titles, plArrayPtr<const float> widths, plArrayPtr<const bool> rightAligned)
{
  using namespace plEditorUi;

  m_Writer.Rect(plRectFloat(rect.x, rect.y, rect.width, s_fTableHeader), Surface());
  m_Writer.Rect(plRectFloat(rect.x, rect.y + s_fTableHeader - 1.0f, rect.width, 1.0f), Line());

  plHybridArray<float, 8> xs, ws;
  LayoutColumns(rect.x, rect.width, widths, xs, ws);

  for (plUInt32 i = 0; i < titles.GetCount(); ++i)
  {
    plStringBuilder sTitle(titles[i]);
    sTitle.ToUpper();
    m_Writer.Text(plRectFloat(xs[i] + 8.0f, rect.y, ws[i] - 16.0f, s_fTableHeader), sTitle, Dim(), MinFontSize, rightAligned[i] ? plUiTextHAlign::Right : plUiTextHAlign::Left);
  }
}

void plEditorUiAudioProfilerView::DrawVoices()
{
  using namespace plEditorUi;

  const plRectFloat tool = GetToolRowRect();
  const plRectFloat table = GetTableRect();
  const plRectFloat details = GetDetailsRect();

  // tool row: state chips; the search field is a retained element
  m_Writer.Text(plRectFloat(8.0f, tool.y, 60.0f, s_fToolRow), "State", Dim(), FontSize - 1);
  float x = 8.0f + MeasureText("State", FontSize - 1) + 10.0f;
  for (plUInt32 i = 0; i < 3; ++i)
  {
    const float fWidth = plAudioEngineUi::ChipWidth(s_szStateNames[i]);
    plAudioEngineUi::Chip(m_Writer, plRectFloat(x, tool.y + 4.0f, fWidth, 22.0f), s_szStateNames[i], StateColor(static_cast<plUInt8>(i)), m_StateFilter[i]);
    x += fWidth + 4.0f;
  }
  m_Writer.Rect(plRectFloat(x + 4.0f, tool.y + 7.0f, 1.0f, 16.0f), Line());
  m_Writer.Rect(plRectFloat(0.0f, tool.y + s_fToolRow - 1.0f, tool.width, 1.0f), Line());

  // table
  const char* const titles[] = {"Event", "Entity", "State", "Prio", "Audibility", "Dist", "Bus", "Codec"};
  const float widths[] = {0.0f, 80.0f, 86.0f, 50.0f, 104.0f, 70.0f, 100.0f, 120.0f};
  const bool right[] = {false, false, false, true, false, true, false, false};

  m_Writer.Rect(table, Surface());
  DrawTableHeader(table, titles, widths, right);

  plHybridArray<float, 8> xs, ws;
  LayoutColumns(table.x, table.width, widths, xs, ws);

  const plRectFloat rows(table.x, table.y + s_fTableHeader, table.width, plMath::Max(table.height - s_fTableHeader, 1.0f));
  m_Writer.PushClip(rows);

  for (plUInt32 r = 0; r < m_VoiceRows.GetCount(); ++r)
  {
    const float y = rows.y + r * s_fRow - m_fScroll;
    if (y + s_fRow < rows.y || y > rows.y + rows.height)
      continue;

    const plAudioVoiceSnapshotEntry& voice = *m_VoiceRows[r];
    const bool bSelected = m_bHasSelection && voice.m_uiId == m_uiSelectedId;
    const plColorGammaUB text = bSelected ? Foreground() : Muted();

    if (bSelected)
      m_Writer.Rect(plRectFloat(rows.x, y, rows.width, s_fRow), Selection());
    m_Writer.Rect(plRectFloat(rows.x, y + s_fRow - 1.0f, rows.width, 1.0f), plAudioEngineUi::Mix(Surface(), Line(), 0.6f));

    auto cell = [&](plUInt32 c)
    { return plRectFloat(xs[c] + 8.0f, y, ws[c] - 16.0f, s_fRow); };

    m_Writer.Text(cell(0), ShortName(voice.m_sEvent), Foreground(), FontSize - 1);
    m_Writer.Text(cell(1), EntityText(voice), text, FontSize - 1);
    plAudioEngineUi::DotText(m_Writer, cell(2), s_szStateNames[plMath::Min<plUInt8>(voice.m_uiState, 2)], StateColor(voice.m_uiState), plAudioEngineUi::Mix(Foreground(), StateColor(voice.m_uiState), 0.82f), FontSize - 1);

    plStringBuilder s;
    s.SetFormat("{}", voice.m_uiPriority);
    m_Writer.Text(cell(3), s, text, FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);

    const plRectFloat audibility = cell(4);
    plAudioEngineUi::ValueBar(m_Writer, plRectFloat(audibility.x, y + 8.0f, plMath::Min(80.0f, audibility.width), 6.0f), voice.m_fAudibility, Muted());

    s.SetFormat("{} m", plArgF(voice.m_fDistance, 1));
    m_Writer.Text(cell(5), s, text, FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
    m_Writer.Text(cell(6), voice.m_sBus, text, FontSize - 1);
    m_Writer.Text(cell(7), CodecText(voice), text, FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
  }

  m_Writer.PopClip();

  if (m_VoiceRows.IsEmpty())
    m_Writer.Text(plRectFloat(rows.x, rows.y + 8.0f, rows.width, 20.0f), m_bHasSnapshot ? "No voices" : "No data from the engine process", Dim(), FontSize - 1, plUiTextHAlign::Center);

  // details of the selected voice
  m_Writer.Rect(details, Surface());
  m_Writer.Rect(plRectFloat(details.x, details.y, 1.0f, details.height), Line());

  const plRectFloat section(details.x + 1.0f, details.y, details.width - 1.0f, plAudioEngineUi::SectionHeight);
  if (!m_bHasSelection)
  {
    plAudioEngineUi::SectionHeader(m_Writer, section, "Voice", plAudioEngineUi::AudioColor());
    m_Writer.Text(plRectFloat(details.x + 10.0f, section.y + section.height + 6.0f, details.width - 20.0f, 20.0f), "Select a voice.", Dim(), FontSize - 1);
    return;
  }

  const plAudioVoiceSnapshotEntry& voice = m_SelectedVoice;
  plAudioEngineUi::SectionHeader(m_Writer, section, ShortName(voice.m_sEvent), plAudioEngineUi::AudioColor());

  plStringBuilder values[PL_ARRAY_SIZE(s_szDetailNames)];
  values[0] = EntityText(voice);
  values[1] = m_bSelectedVoiceEnded ? plStringView("ended") : plStringView(s_szStateNames[plMath::Min<plUInt8>(voice.m_uiState, 2)]);
  values[2].SetFormat("{}", voice.m_uiPriority);
  values[3].SetFormat("{}", plArgF(voice.m_fAudibility, 3));
  values[4].SetFormat("{} m", plArgF(voice.m_fDistance, 2));
  values[5] = voice.m_sBus;
  values[6] = CodecText(voice);
  values[7].SetFormat("{} s", plArgF(voice.m_fAge, 2));
  values[8].SetFormat("{}", plArgF(voice.m_fOcclusion, 2));

  float y = section.y + section.height + 6.0f;
  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_szDetailNames); ++i)
  {
    const plRectFloat line(details.x + 10.0f, y, details.width - 20.0f, 18.0f);
    m_Writer.Text(line, s_szDetailNames[i], Dim(), FontSize - 1);

    plColorGammaUB value = Muted();
    if (i == 1)
      value = m_bSelectedVoiceEnded ? Dim() : plAudioEngineUi::Mix(Foreground(), StateColor(voice.m_uiState), 0.82f);

    m_Writer.Text(line, values[i], value, FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
    y += 20.0f;
  }
}

void plEditorUiAudioProfilerView::DrawBuses()
{
  using namespace plEditorUi;

  const plRectFloat tool = GetToolRowRect();
  const plRectFloat body = GetBodyRect();
  const plAudioProfilingSnapshot& s = m_Snapshot;

  // tool row: the active snapshots, the master level on the right
  m_Writer.Text(plRectFloat(8.0f, tool.y, 80.0f, s_fToolRow), "Snapshots", Dim(), FontSize - 1);
  float x = 8.0f + MeasureText("Snapshots", FontSize - 1) + 10.0f;

  if (s.m_ActiveSnapshots.IsEmpty())
    m_Writer.Text(plRectFloat(x, tool.y, 100.0f, s_fToolRow), s.m_Buses.IsEmpty() ? "-" : "none active", Dim(), FontSize - 1);

  for (const plAudioActiveSnapshotEntry& active : s.m_ActiveSnapshots)
  {
    plStringBuilder sLabel;
    sLabel.SetFormat("{} ({})", active.m_sName, plArgF(active.m_fIntensity, 2));
    const plRectFloat chip(x, tool.y + 4.0f, MeasureText(sLabel, FontSize - 1) + 18.0f, 22.0f);
    m_Writer.Rect(chip, Selection(), 11.0f);
    m_Writer.RectOutline(chip, plAudioEngineUi::Mix(Surface(), Accent(), 0.55f), 1.0f, 11.0f);
    m_Writer.Text(chip, sLabel, Foreground(), FontSize - 1, plUiTextHAlign::Center);
    x += chip.width + 4.0f;
  }

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
    m_Writer.Text(plRectFloat(0.0f, tool.y, tool.width - 10.0f, s_fToolRow), "No mixer loaded", Dim(), FontSize - 1, plUiTextHAlign::Right);
  }
  else
  {
    plStringBuilder sPeak, sRms, sText;
    plAudioEngineUi::FormatDbfs(pMaster->m_fPeak, sPeak);
    plAudioEngineUi::FormatDbfs(pMaster->m_fRms, sRms);

    // "Master peak -3.1 dBFS · RMS -14.6 dBFS" with the values in the code face
    const plString sValue2 = plStringBuilder(sRms, " dBFS");
    const plString sValue1 = plStringBuilder(sPeak, " dBFS");
    const plString sLabel1 = plStringBuilder(pMaster->m_sName, " peak ");
    const plStringView sLabel2 = " \xC2\xB7 RMS ";

    float fRight = tool.width - 10.0f;
    auto rightText = [&](plStringView sText, bool bValue)
    {
      const float fWidth = bValue ? MeasureCodeText(sText, FontSize - 2) + 2.0f : MeasureText(sText, FontSize - 1) + 2.0f;
      m_Writer.Text(plRectFloat(fRight - fWidth, tool.y, fWidth, s_fToolRow), sText, bValue ? Foreground() : Dim(), bValue ? FontSize - 2 : FontSize - 1, plUiTextHAlign::Left, plUiTextVAlign::Center, bValue);
      fRight -= fWidth;
    };

    rightText(sValue2, true);
    rightText(sLabel2, false);
    rightText(sValue1, true);
    rightText(sLabel1, false);
  }

  m_Writer.Rect(plRectFloat(0.0f, tool.y + s_fToolRow - 1.0f, tool.width, 1.0f), Line());

  const plRectFloat area(body.x, tool.y + s_fToolRow, body.width, plMath::Max(body.height - s_fToolRow, 1.0f));
  m_Writer.Rect(area, Background());

  if (s.m_Buses.IsEmpty())
  {
    m_Writer.Text(area, "No mixer loaded", Dim(), FontSize - 1, plUiTextHAlign::Center);
    return;
  }

  // one strip per bus: name, meter, peak, gain, aux tag
  m_Writer.PushClip(area);
  x = area.x + 10.0f - m_fScroll;
  for (const plAudioBusSnapshotEntry& bus : s.m_Buses)
  {
    const float y = area.y + 10.0f;
    const plRectFloat strip(x, y, s_fBusStripWidth, s_fBusMeterHeight + 90.0f);

    m_Writer.Text(plRectFloat(strip.x, y, strip.width, 18.0f), bus.m_sName, Foreground(), FontSize - 1, plUiTextHAlign::Center);
    plEditorUiDraw::LevelMeter(m_Writer, plRectFloat(strip.x + strip.width * 0.5f - 8.0f, y + 22.0f, 16.0f, s_fBusMeterHeight), plEditorUiDraw::LinearToDb(bus.m_fRms), plEditorUiDraw::LinearToDb(bus.m_fPeak));

    plStringBuilder sPeak, sText;
    plAudioEngineUi::FormatDbfs(bus.m_fPeak, sPeak);
    sText.SetFormat("{} dBFS", sPeak);
    float fY = y + 22.0f + s_fBusMeterHeight + 4.0f;
    m_Writer.Text(plRectFloat(strip.x, fY, strip.width, 16.0f), sText, Muted(), FontSize - 2, plUiTextHAlign::Center, plUiTextVAlign::Center, true);
    fY += 16.0f;

    sText.SetFormat("gain {}{} dB", bus.m_fGainDb >= 0.0f ? "+" : "", plArgF(bus.m_fGainDb, 1));
    m_Writer.Text(plRectFloat(strip.x, fY, strip.width, 16.0f), sText, Dim(), FontSize - 2, plUiTextHAlign::Center, plUiTextVAlign::Center, true);
    fY += 18.0f;

    if (bus.m_bIsAux)
    {
      const plColorGammaUB scripting = plAudioEngineUi::CategoryColor("Scripting");
      const plRectFloat badge(strip.x + strip.width * 0.5f - 16.0f, fY, 32.0f, 16.0f);
      m_Writer.Rect(badge, plAudioEngineUi::Mix(Background(), scripting, 0.13f), 3.0f);
      m_Writer.Text(badge, "AUX", scripting, MinFontSize, plUiTextHAlign::Center, plUiTextVAlign::Center, true);
    }

    x += s_fBusStripWidth + s_fBusStripGap;
  }
  m_Writer.PopClip();
}

void plEditorUiAudioProfilerView::DrawMemory()
{
  using namespace plEditorUi;

  const plRectFloat table = GetTableRect();
  const plRectFloat body = GetBodyRect();
  const plAudioProfilingSnapshot& s = m_Snapshot;

  const char* const titles[] = {"Resource", "Type", "Load mode", "Codec", "Resident", "Streams", "Share"};
  const float widths[] = {0.0f, 70.0f, 150.0f, 130.0f, 90.0f, 70.0f, 120.0f};
  const bool right[] = {false, false, false, false, true, true, false};

  m_Writer.Rect(table, Surface());
  DrawTableHeader(table, titles, widths, right);

  plHybridArray<float, 8> xs, ws;
  LayoutColumns(table.x, table.width, widths, xs, ws);

  plUInt64 uiEventBytes = 0;
  for (const plAudioEventMemorySnapshotEntry& event : s.m_Events)
    uiEventBytes += event.m_uiSampleBytes;

  const plUInt64 uiTotal = s.m_uiClipResidentBytes + uiEventBytes + s.m_uiStreamRingBytes;

  const plRectFloat rows(table.x, table.y + s_fTableHeader, table.width, plMath::Max(table.height - s_fTableHeader, 1.0f));
  m_Writer.PushClip(rows);
  for (plUInt32 r = 0; r < m_MemoryRows.GetCount(); ++r)
  {
    const float y = rows.y + r * s_fRow - m_fScroll;
    if (y + s_fRow < rows.y || y > rows.y + rows.height)
      continue;

    const MemoryRow& row = m_MemoryRows[r];
    const plColorGammaUB text = row.m_bWarning ? Warning() : Muted();
    m_Writer.Rect(plRectFloat(rows.x, y + s_fRow - 1.0f, rows.width, 1.0f), plAudioEngineUi::Mix(Surface(), Line(), 0.6f));

    auto cell = [&](plUInt32 c)
    { return plRectFloat(xs[c] + 8.0f, y, ws[c] - 16.0f, s_fRow); };

    m_Writer.Text(cell(0), row.m_sName, row.m_bWarning ? Warning() : Foreground(), FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    m_Writer.Text(cell(1), row.m_sType, text, FontSize - 1);
    m_Writer.Text(cell(2), row.m_sLoadMode, text, FontSize - 1);
    m_Writer.Text(cell(3), row.m_sCodec, text, FontSize - 1);

    plStringBuilder sResident("-");
    if (row.m_bHasResident)
      plAudioEngineUi::FormatBytes(row.m_uiResident, sResident);
    m_Writer.Text(cell(4), sResident, text, FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
    m_Writer.Text(cell(5), row.m_sStreams, text, FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);

    const float fShare = (row.m_bHasResident && uiTotal > 0) ? static_cast<float>(static_cast<double>(row.m_uiResident) / static_cast<double>(uiTotal)) : 0.0f;
    const plRectFloat share = cell(6);
    plAudioEngineUi::ValueBar(m_Writer, plRectFloat(share.x, y + 8.0f, plMath::Min(80.0f, share.width), 6.0f), fShare, Muted());
  }
  m_Writer.PopClip();

  if (m_MemoryRows.IsEmpty())
    m_Writer.Text(plRectFloat(rows.x, rows.y + 8.0f, rows.width, 20.0f), m_bHasSnapshot ? "Nothing loaded" : "No data from the engine process", Dim(), FontSize - 1, plUiTextHAlign::Center);

  // totals
  const plRectFloat totals(body.x, table.y + table.height, body.width, s_fTotalsRow);
  m_Writer.Rect(totals, Surface());
  m_Writer.Rect(plRectFloat(totals.x, totals.y, totals.width, 1.0f), Line());

  plStringBuilder sClips, sEvents, sRings, sTotal, sLine;
  plAudioEngineUi::FormatBytes(s.m_uiClipResidentBytes, sClips);
  plAudioEngineUi::FormatBytes(uiEventBytes, sEvents);
  plAudioEngineUi::FormatBytes(s.m_uiStreamRingBytes, sRings);
  plAudioEngineUi::FormatBytes(uiTotal, sTotal);
  sLine.SetFormat("Clips {} ({}) \xC2\xB7 Event samples {} ({}) \xC2\xB7 Stream rings {} \xC2\xB7 Streams {} open, {} pooled \xC2\xB7 Banks {} \xC2\xB7 Total {}", sClips, s.m_Clips.GetCount(), sEvents, s.m_Events.GetCount(), sRings, s.m_uiStreams, s.m_uiPooledStreams, s.m_Banks.GetCount(), sTotal);
  m_Writer.Text(plRectFloat(totals.x + 8.0f, totals.y, totals.width - 16.0f, totals.height), sLine, Dim(), FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
}

void plEditorUiAudioProfilerView::DrawLog()
{
  using namespace plEditorUi;

  const plRectFloat tool = GetToolRowRect();
  const plRectFloat table = GetTableRect();

  // tool row: type chips, the retained search field, Clear
  m_Writer.Text(plRectFloat(8.0f, tool.y, 60.0f, s_fToolRow), "Type", Dim(), FontSize - 1);
  float x = 8.0f + MeasureText("Type", FontSize - 1) + 10.0f;
  for (plUInt32 i = 0; i < LogCategoryCount; ++i)
  {
    const float fWidth = plAudioEngineUi::ChipWidth(s_szLogCategoryNames[i]);
    plAudioEngineUi::Chip(m_Writer, plRectFloat(x, tool.y + 4.0f, fWidth, 22.0f), s_szLogCategoryNames[i], i == 2 ? Warning() : Muted(), m_LogCategory[i]);
    x += fWidth + 4.0f;
  }
  m_Writer.Rect(plRectFloat(x + 4.0f, tool.y + 7.0f, 1.0f, 16.0f), Line());

  const float fClear = plAudioEngineUi::TextButtonWidth("Clear", false);
  plAudioEngineUi::TextButton(m_Writer, plRectFloat(x + 12.0f + s_fSearchWidth + 6.0f, tool.y + 3.0f, fClear, ControlHeight), "Clear");
  m_Writer.Rect(plRectFloat(0.0f, tool.y + s_fToolRow - 1.0f, tool.width, 1.0f), Line());

  // table
  const char* const titles[] = {"Time", "Type", "Event"};
  const float widths[] = {80.0f, 120.0f, 0.0f};
  const bool right[] = {true, false, false};

  m_Writer.Rect(table, Surface());
  DrawTableHeader(table, titles, widths, right);

  plHybridArray<float, 8> xs, ws;
  LayoutColumns(table.x, table.width, widths, xs, ws);

  const plRectFloat rows(table.x, table.y + s_fTableHeader, table.width, plMath::Max(table.height - s_fTableHeader, 1.0f));
  m_Writer.PushClip(rows);
  for (plUInt32 r = 0; r < m_VisibleLog.GetCount(); ++r)
  {
    const float y = rows.y + r * s_fRow - m_fScroll;
    if (y + s_fRow < rows.y || y > rows.y + rows.height)
      continue;

    const plAudioProfilerLogLine& line = m_LogLines[m_VisibleLog[r]];
    m_Writer.Rect(plRectFloat(rows.x, y + s_fRow - 1.0f, rows.width, 1.0f), plAudioEngineUi::Mix(Surface(), Line(), 0.6f));

    auto cell = [&](plUInt32 c)
    { return plRectFloat(xs[c] + 8.0f, y, ws[c] - 16.0f, s_fRow); };

    if (line.m_uiType == plAudioProfilerLogLine::GapType)
    {
      plStringBuilder sText;
      sText.SetFormat("{} entries lost (paused, or dropped before they were polled)", line.m_uiLost);
      m_Writer.Text(cell(1), "lost", Dim(), FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
      m_Writer.Text(cell(2), sText, Dim(), FontSize - 1);
      continue;
    }

    const auto type = static_cast<plAudioLogEventType::Enum>(line.m_uiType);
    const bool bWarning = plAudioLogEventType::IsWarning(type);

    plStringBuilder sTime, sText;
    sTime.SetFormat("{} s", plArgF(line.m_fTime, 2));
    sText = ShortName(line.m_sName);
    if (!line.m_sDetail.IsEmpty())
      sText.Append("   ", line.m_sDetail);

    m_Writer.Text(cell(0), sTime, Dim(), FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
    m_Writer.Text(cell(1), plAudioLogEventType::GetName(type), bWarning ? Warning() : Muted(), FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    m_Writer.Text(cell(2), sText, bWarning ? Warning() : Muted(), FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
  }
  m_Writer.PopClip();

  if (m_VisibleLog.IsEmpty())
    m_Writer.Text(plRectFloat(rows.x, rows.y + 8.0f, rows.width, 20.0f), m_LogLines.IsEmpty() ? "No log entries" : "No entries match the filter", Dim(), FontSize - 1, plUiTextHAlign::Center);
}

void plEditorUiAudioProfilerView::DrawStatus()
{
  using namespace plEditorUi;

  const plVec2 size = GetSurface()->GetLogicalSize();
  const plRectFloat status(0.0f, size.y - s_fStatus, size.x, s_fStatus);
  m_Writer.Rect(status, Background());
  m_Writer.Rect(plRectFloat(0.0f, status.y, status.width, 1.0f), Line());

  const plAudioProfilingSnapshot& s = m_Snapshot;

  // "Real 5/32" ... with the values in the code face
  plStringBuilder values[7];
  const char* labels[7] = {"Real", "Virtual", "Instances", "DSP", "Audio thread", "Streams", "Memory"};

  if (m_bHasSnapshot)
  {
    values[0].SetFormat("{}/{}", s.m_uiRealVoices, s.m_uiMaxReal);
    values[1].SetFormat("{}/{}", s.m_uiVirtualVoices, s.m_uiMaxVirtual);
    values[2].SetFormat("{}", s.m_uiInstances);
    values[3].SetFormat("{}%", plArgF(s.m_fDspLoad * 100.0f, 1));
    values[4].SetFormat("{} ms", plArgF(s.m_fUpdateMs, 2));
    values[5].SetFormat("{}", s.m_uiStreams);
    plAudioEngineUi::FormatBytes(s.m_uiMemoryBytes, values[6]);
  }
  else
  {
    for (plStringBuilder& value : values)
      value = "-";
  }

  float x = 8.0f;
  for (plUInt32 i = 0; i < 7; ++i)
  {
    const float fLabel = MeasureText(labels[i], FontSize - 1) + 5.0f;
    const float fValue = MeasureCodeText(values[i], FontSize - 2) + 2.0f;
    if (x + fLabel + fValue > size.x - 8.0f)
      break;

    m_Writer.Text(plRectFloat(x, status.y + 1.0f, fLabel, status.height - 1.0f), labels[i], Dim(), FontSize - 1);
    m_Writer.Text(plRectFloat(x + fLabel, status.y + 1.0f, fValue, status.height - 1.0f), values[i], Muted(), FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    x += fLabel + fValue + 12.0f;
  }
}

//////////////////////////////////////////////////////////////////////////
// Input

void plEditorUiAudioProfilerView::ClampScroll()
{
  const plRectFloat table = GetTableRect();
  const float fVisible = plMath::Max(table.height - s_fTableHeader, 1.0f);

  float fContent = 0.0f;
  switch (m_Tab)
  {
    case Tab::Voices:
      fContent = m_VoiceRows.GetCount() * s_fRow;
      break;
    case Tab::Memory:
      fContent = m_MemoryRows.GetCount() * s_fRow;
      break;
    case Tab::Log:
      fContent = m_VisibleLog.GetCount() * s_fRow;
      break;
    case Tab::Buses:
      fContent = m_Snapshot.m_Buses.GetCount() * (s_fBusStripWidth + s_fBusStripGap) + 20.0f;
      m_fScroll = plMath::Clamp(m_fScroll, 0.0f, plMath::Max(fContent - table.width, 0.0f));
      return;
    default:
      break;
  }

  const float fMax = plMath::Max(fContent - fVisible, 0.0f);

  // the log follows its newest line unless the user scrolled up
  if (m_Tab == Tab::Log && m_bFollowLog)
    m_fScroll = fMax;

  m_fScroll = plMath::Clamp(m_fScroll, 0.0f, fMax);
}

void plEditorUiAudioProfilerView::OnPointer(const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;

  const plRectFloat table = GetTableRect();

  if (e.m_Type == Type::Wheel)
  {
    if (!table.Contains(e.m_vPosition))
      return;

    m_fScroll -= e.m_fWheel * (m_Tab == Tab::Buses ? 40.0f : s_fWheelStep);
    ClampScroll();

    if (m_Tab == Tab::Log)
    {
      const float fMax = plMath::Max(m_VisibleLog.GetCount() * s_fRow - (table.height - s_fTableHeader), 0.0f);
      m_bFollowLog = m_fScroll >= fMax - 2.0f;
    }

    m_bDrawDirty = true;
    return;
  }

  if (e.m_Type != Type::Press || e.m_Button != plEditorUiPointerEvent::Button::Left || m_Tab != Tab::Voices || !e.m_sElement.IsEmpty())
    return;

  const float fRowsTop = table.y + s_fTableHeader;
  if (!table.Contains(e.m_vPosition) || e.m_vPosition.y < fRowsTop)
    return;

  const plInt32 iRow = static_cast<plInt32>((e.m_vPosition.y - fRowsTop + m_fScroll) / s_fRow);
  if (iRow < 0 || iRow >= static_cast<plInt32>(m_VoiceRows.GetCount()))
    return;

  m_uiSelectedId = m_VoiceRows[iRow]->m_uiId;
  m_SelectedVoice = *m_VoiceRows[iRow];
  m_bHasSelection = true;
  m_bSelectedVoiceEnded = false;
  m_bDrawDirty = true;
}

plString plEditorUiAudioProfilerView::GetToolTipAt(const plVec2& vLogical) const
{
  const plString sElementTip = plEditorUiContent::GetToolTipAt(vLogical);
  if (!sElementTip.IsEmpty())
    return sElementTip;

  const plRectFloat table = GetTableRect();
  const plRectFloat tool = GetToolRowRect();

  if (m_Tab == Tab::Buses)
  {
    if (tool.Contains(vLogical) && vLogical.x > tool.width * 0.5f)
    {
      if (m_Snapshot.m_Buses.IsEmpty())
        return {};

      plStringBuilder sTip;
      sTip.SetFormat("Mixer: {}", m_Snapshot.m_sMixer);
      return sTip;
    }

    if (tool.Contains(vLogical) && !m_Snapshot.m_ActiveSnapshots.IsEmpty())
    {
      // the active snapshot chips, laid out as DrawBuses does
      float x = 8.0f + plEditorUi::MeasureText("Snapshots", plEditorUi::FontSize - 1) + 10.0f;
      for (const plAudioActiveSnapshotEntry& active : m_Snapshot.m_ActiveSnapshots)
      {
        plStringBuilder sLabel;
        sLabel.SetFormat("{} ({})", active.m_sName, plArgF(active.m_fIntensity, 2));
        const float fWidth = plEditorUi::MeasureText(sLabel, plEditorUi::FontSize - 1) + 18.0f;
        if (vLogical.x >= x && vLogical.x < x + fWidth)
        {
          plStringBuilder sTip;
          sTip.SetFormat("Intensity {}", plArgF(active.m_fIntensity, 2));
          return sTip;
        }
        x += fWidth + 4.0f;
      }
    }

    if (table.Contains(vLogical))
    {
      const plInt32 iStrip = static_cast<plInt32>((vLogical.x - table.x - 10.0f + m_fScroll) / (s_fBusStripWidth + s_fBusStripGap));
      if (iStrip < 0 || iStrip >= static_cast<plInt32>(m_Snapshot.m_Buses.GetCount()))
        return {};

      const plAudioBusSnapshotEntry& bus = m_Snapshot.m_Buses[iStrip];
      plStringBuilder sPeak, sRms, sTip;
      plAudioEngineUi::FormatDbfs(bus.m_fPeak, sPeak);
      plAudioEngineUi::FormatDbfs(bus.m_fRms, sRms);
      sTip.SetFormat("{}{}\nPeak {} dBFS\nRMS {} dBFS\nGain {}{} dB", bus.m_sName, bus.m_bIsAux ? " (aux)" : "", sPeak, sRms, bus.m_fGainDb >= 0.0f ? "+" : "", plArgF(bus.m_fGainDb, 1));

      if (bus.m_uiParent < m_Snapshot.m_Buses.GetCount())
        sTip.AppendFormat("\nParent {}", m_Snapshot.m_Buses[bus.m_uiParent].m_sName);

      return sTip;
    }

    return {};
  }

  const float fRowsTop = table.y + s_fTableHeader;
  if (!table.Contains(vLogical) || vLogical.y < fRowsTop)
    return {};

  const plInt32 iRow = static_cast<plInt32>((vLogical.y - fRowsTop + m_fScroll) / s_fRow);
  if (iRow < 0)
    return {};

  if (m_Tab == Tab::Voices && iRow < static_cast<plInt32>(m_VoiceRows.GetCount()))
  {
    const plAudioVoiceSnapshotEntry& voice = *m_VoiceRows[iRow];
    plStringBuilder sTip;
    sTip.SetFormat("{}\nAudibility {}", voice.m_sEvent, plArgF(voice.m_fAudibility, 3));
    return sTip;
  }

  if (m_Tab == Tab::Memory && iRow < static_cast<plInt32>(m_MemoryRows.GetCount()))
    return m_MemoryRows[iRow].m_sToolTip;

  if (m_Tab == Tab::Log && iRow < static_cast<plInt32>(m_VisibleLog.GetCount()))
  {
    const plAudioProfilerLogLine& line = m_LogLines[m_VisibleLog[iRow]];
    if (line.m_uiType == plAudioProfilerLogLine::GapType)
      return {};

    plStringBuilder sTip = line.m_sName;
    if (!line.m_sDetail.IsEmpty())
      sTip.Append("\n", line.m_sDetail);
    return sTip;
  }

  return {};
}
