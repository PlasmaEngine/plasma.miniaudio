#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/Profiler/AudioProfilerView.h>
#include <EditorUi/Drawings.h>
#include <EditorUi/Menu.h>
#include <EditorUi/Runtime.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Tabs.h>
#include <EditorUi/Widgets.h>
#include <GameUiPlugin/Components/UiCanvasComponent.h>
#include <PlasmaAudioPlugin/Codecs/AudioCodec.h>
#include <PlasmaAudioPlugin/Profiling/AudioEventLog.h>

namespace
{
  constexpr float s_fHeaderRow = 30.0f;
  constexpr float s_fGraphHeight = 196.0f;    ///< the graphs' height in a tall panel
  constexpr float s_fMinGraphHeight = 110.0f; ///< and in a short one, which leaves the rest to the tabs
  constexpr float s_fGraphHead = 182.0f; ///< the row titles left of the plots
  constexpr float s_fLaneRow = 18.0f;
  constexpr float s_fCardWidth = 250.0f;
  constexpr float s_fTabRow = 30.0f;
  constexpr float s_fToolRow = 30.0f;
  constexpr float s_fStatus = plEditorUi::StatusRowHeight + 1.0f;
  constexpr float s_fTableHeader = 26.0f;
  constexpr float s_fRow = plEditorUi::RowHeight;
  constexpr float s_fDetailsWidth = 260.0f;
  constexpr float s_fTotalsRow = 28.0f;
  constexpr float s_fSearchWidth = 220.0f;
  constexpr float s_fWheelStep = 3.0f * plEditorUi::RowHeight;
  constexpr plUInt32 s_uiMaxRows = 1000;
  constexpr plUInt32 s_uiMaxLogLines = 2000;

  constexpr float s_fSummaryRow = 50.0f; ///< the Voices tab's summary cards
  constexpr float s_fBannerRow = 22.0f;  ///< the note above the voice table when the query's cap cut rows
  constexpr plUInt32 s_uiSummaryCards = 6;
  constexpr float s_fBusDropdownWidth = 150.0f;

  constexpr float s_fBusStripWidth = 86.0f;
  constexpr float s_fBusStripGap = 6.0f;
  constexpr float s_fBusMeterHeight = 120.0f;

  const plHashedString s_sDrawing = plMakeHashedString("Profiler.Drawing");

  constexpr const char* s_szStateNames[4] = {"real", "virtual", "stopping", "ended"};
  constexpr const char* s_szStateTips[4] = {
    "Show real voices: decoded and mixed",
    "Show virtual voices: tracked without decoding, over the real budget or beyond their max distance",
    "Show voices whose instance fades out",
    "Show the voices that ended in the last seconds, and why: stolen, killed by the budget, stopped or finished",
  };
  constexpr const char* s_szLogCategoryNames[4] = {"lifecycle", "voices", "warnings", "system"};
  constexpr const char* s_szLogCategoryTips[4] = {
    "Instance starts, stops and ends",
    "Voices culled at start, virtualised and realised",
    "Stolen instances, rejected starts, killed voices, missing clips, lost devices",
    "Clip load waits, device recovery, mixer, bank and language changes",
  };

  constexpr const char* s_szDetailNames[] = {"Owner", "Entity id", "State", "Stop reason", "Priority", "Audibility", "Volume", "Distance", "Occlusion", "Bus", "Codec", "Age"};

  /// The voice table's columns; a width of 0 takes the remaining space.
  constexpr const char* s_szVoiceColumns[] = {"Event", "Owner", "State", "Prio", "Audibility", "Volume", "Dist", "Bus", "Codec"};
  constexpr float s_fVoiceColumnWidths[] = {0.0f, 110.0f, 156.0f, 50.0f, 92.0f, 100.0f, 70.0f, 96.0f, 120.0f};
  constexpr bool s_bVoiceColumnRight[] = {false, false, false, true, false, true, true, false, false};
  constexpr plAudioProfilerVoiceQuery::SortKey s_VoiceColumnKeys[] = {plAudioProfilerVoiceQuery::SortKey::Event, plAudioProfilerVoiceQuery::SortKey::Owner,
    plAudioProfilerVoiceQuery::SortKey::State, plAudioProfilerVoiceQuery::SortKey::Priority, plAudioProfilerVoiceQuery::SortKey::Audibility,
    plAudioProfilerVoiceQuery::SortKey::Volume, plAudioProfilerVoiceQuery::SortKey::Distance, plAudioProfilerVoiceQuery::SortKey::Bus,
    plAudioProfilerVoiceQuery::SortKey::Codec};
  constexpr plUInt32 s_uiVoiceColumns = PL_ARRAY_SIZE(s_szVoiceColumns);

  /// The status row's values, and what each of them measures.
  constexpr const char* s_szStatusLabels[7] = {"Real", "Virtual", "Instances", "Mix CPU", "Audio update", "Streams", "Audio data"};
  constexpr const char* s_szStatusTips[7] = {
    "Real voices: decoded and mixed, against PlasmaAudio.MaxRealVoices. Counted on the audio thread at the snapshot.",
    "Virtual voices: tracked without decoding, against PlasmaAudio.MaxVirtualVoices. Counted on the audio thread at the snapshot.",
    "Event instances alive in the event manager. An instance can have several voices.",
    "Audio device thread: time spent in the device callback mixing one period, as a share of the period, smoothed. Measured every callback.",
    "Audio thread: wall time of the last PlasmaAudio update (voice ranking, spatialisation, parameters, streams). Measured around the update.",
    "Streamed clip voices reading from disk right now.",
    "Resident audio data: clip sample data, stream rings and the samples embedded in events. Miniaudio node and decoder state are not counted.",
  };

  /// The summary cards of the Voices tab: title, scope and what the card measures.
  constexpr const char* s_szCardTitles[s_uiSummaryCards] = {"Real", "Virtual", "Ended", "Mix CPU", "Audio update", "Audio data"};
  constexpr const char* s_szCardScopes[s_uiSummaryCards] = {"of budget", "of budget", "last 5 s", "device thread", "audio thread", "resident"};
  constexpr const char* s_szCardTips[s_uiSummaryCards] = {
    s_szStatusTips[0],
    s_szStatusTips[1],
    "Voices that ended in the last 5 seconds: stolen by a newer instance of their polyphony group, killed by the voice budget, stopped by the game, or finished.",
    s_szStatusTips[3],
    s_szStatusTips[4],
    s_szStatusTips[6],
  };

  /// The status row's values and where each label starts; out_xs[i + 1] is where item i ends (with its gap). Returns how
  /// many items fit into fWidth.
  plUInt32 LayoutStatus(const plAudioProfilingSnapshot& s, bool bHasSnapshot, float fWidth, plStringBuilder out_values[7], float out_xs[8])
  {
    using namespace plEditorUi;

    if (bHasSnapshot)
    {
      out_values[0].SetFormat("{}/{}", s.m_uiRealVoices, s.m_uiMaxReal);
      out_values[1].SetFormat("{}/{}", s.m_uiVirtualVoices, s.m_uiMaxVirtual);
      out_values[2].SetFormat("{}", s.m_uiInstances);
      out_values[3].SetFormat("{}%", plArgF(s.m_fDspLoad * 100.0f, 1));
      out_values[4].SetFormat("{} ms", plArgF(s.m_fUpdateMs, 2));
      out_values[5].SetFormat("{}", s.m_uiStreams);
      plAudioEngineUi::FormatBytes(s.m_uiMemoryBytes, out_values[6]);
    }
    else
    {
      for (plUInt32 i = 0; i < 7; ++i)
        out_values[i] = "-";
    }

    float x = 8.0f;
    plUInt32 uiShown = 0;
    for (; uiShown < 7; ++uiShown)
    {
      const float fLabel = MeasureText(s_szStatusLabels[uiShown], FontSize - 1) + 5.0f;
      const float fValue = MeasureCodeText(out_values[uiShown], FontSize - 2) + 2.0f;
      if (x + fLabel + fValue > fWidth - 8.0f)
        break;

      out_xs[uiShown] = x;
      x += fLabel + fValue + 12.0f;
    }

    out_xs[uiShown] = x;
    return uiShown;
  }

  /// A state chip of the Voices tab: its name and room for the count.
  float StateChipWidth(plUInt32 uiState)
  {
    return plAudioEngineUi::ChipWidth(s_szStateNames[uiState]) + 26.0f;
  }

  /// Where the Voices tab's bus dropdown starts: after the state chips.
  float BusDropdownX()
  {
    float x = 8.0f + plEditorUi::MeasureText("State", plEditorUi::FontSize - 1) + 10.0f;
    for (plUInt32 i = 0; i < 4; ++i)
      x += StateChipWidth(i) + 4.0f;

    return x + 12.0f;
  }

  /// Linear volume as dB, "-inf" for silence.
  plString VolumeText(float fLinear)
  {
    if (fLinear <= 0.00001f)
      return "-inf dB";

    plStringBuilder s;
    s.SetFormat("{} dB", plArgF(20.0f * plMath::Log10(fLinear), 1));
    return s;
  }

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

  /// Voice states as system colours: real in the accent, virtual in the XR colour, stopping in the warning colour, ended
  /// dimmed.
  plColorGammaUB StateColor(plUInt8 uiState)
  {
    switch (uiState)
    {
      case plAudioVoiceSnapshotEntry::Real:
        return plEditorUi::Accent();
      case plAudioVoiceSnapshotEntry::Virtual:
        return plAudioEngineUi::CategoryColor("XR");
      case plAudioVoiceSnapshotEntry::Ended:
        return plEditorUi::Dim();
      default:
        return plEditorUi::Warning();
    }
  }

  /// "real", "stopping · virtual", "stopping · stolen · oldest", "ended · finished".
  plString StateText(const plAudioVoiceSnapshotEntry& voice)
  {
    const auto reason = static_cast<plAudioStopReason::Enum>(voice.m_uiStopReason);
    plStringBuilder s = s_szStateNames[plMath::Min<plUInt8>(voice.m_uiState, plAudioVoiceSnapshotEntry::Ended)];

    if (voice.m_uiState == plAudioVoiceSnapshotEntry::Stopping)
      s.Append(" \xC2\xB7 ", plAudioStopReason::IsForced(reason) ? plStringView(plAudioStopReason::GetName(reason)) : plStringView(voice.m_bVirtual ? "virtual" : "real"));
    else if (voice.m_uiState == plAudioVoiceSnapshotEntry::Ended && reason != plAudioStopReason::None)
      s.Append(" \xC2\xB7 ", plAudioStopReason::GetName(reason));

    return s;
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

  /// The game object the voice plays on: its name, its entity id without a name, "2D" without an entity.
  plString OwnerText(const plAudioVoiceSnapshotEntry& voice)
  {
    if (!voice.m_sEntity.IsEmpty())
      return voice.m_sEntity;

    if (voice.m_uiEntityId == 0)
      return "2D";

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
  template <plUInt32 N>
  void LayoutColumns(float x, float fWidth, plArrayPtr<const float> widths, plHybridArray<float, N>& out_x, plHybridArray<float, N>& out_w)
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

void plEditorUiAudioProfilerView::SetTargetText(plStringView sText)
{
  if (m_sTargetText == sText)
    return;

  m_sTargetText = sText;
  m_bRebuild = true;
}

void plEditorUiAudioProfilerView::SetSnapshot(const plAudioProfilingSnapshot& snapshot, plArrayPtr<const plAudioProfilerLogLine> newLines)
{
  m_LiveSnapshot = snapshot;
  m_bHasLiveSnapshot = true;

  // the live graphs run on while paused, as a recording does
  AddLiveSample(snapshot);

  // paused, the lines wait, so the shown log keeps its lines
  if (m_bPaused)
  {
    for (const plAudioProfilerLogLine& line : newLines)
    {
      m_PausedLines.PushBack(line);
      if (m_PausedLines.GetCount() > s_uiMaxLogLines)
        m_PausedLines.PopFront();
    }

    m_bDrawDirty = true;
    return;
  }

  for (const plAudioProfilerLogLine& line : newLines)
  {
    if (line.m_uiType != plAudioProfilerLogLine::GapType)
      ++m_uiLogReceived;

    m_LogLines.PushBack(line);
    if (m_LogLines.GetCount() > s_uiMaxLogLines)
      m_LogLines.PopFront();
  }

  // with a range the tabs keep showing the range
  if (!HasRange())
  {
    m_Snapshot = m_LiveSnapshot;
    m_bHasSnapshot = true;
    RebuildVoiceRows();
    RebuildMemoryRows();
    RebuildVisibleLog();
  }

  m_bDrawDirty = true;
}

void plEditorUiAudioProfilerView::SetCapture(const plAudioProfilerCapture* pCapture)
{
  m_pCapture = pCapture;
  OnCaptureChanged();
}

void plEditorUiAudioProfilerView::OnCaptureChanged()
{
  // a range past the end of a new capture goes
  if (m_pCapture == nullptr || m_pCapture->IsEmpty() || (HasRange() && plMath::Min(m_fRangeA, m_fRangeB) > m_pCapture->GetSamples().PeekBack().m_fTime))
  {
    m_fRangeA = m_fRangeB = -1.0;
  }

  if (m_pCapture == nullptr || m_iSelectedInstance >= static_cast<plInt32>(m_pCapture->GetInstances().GetCount()))
    m_iSelectedInstance = -1;

  // a recording capture changes ten times a second; the range's tabs only change with the range
  if (!HasRange() || m_Lanes.IsEmpty())
    RebuildLanes();

  m_bDrawDirty = true;
}

void plEditorUiAudioProfilerView::SetRecording(bool bRecording)
{
  if (m_bRecording == bRecording)
    return;

  m_bRecording = bRecording;

  // a recording is what the graphs follow while it grows
  if (bRecording)
    SetGraphSource(GraphSource::Capture);

  m_bDrawDirty = true;
}

void plEditorUiAudioProfilerView::SetGraphSource(GraphSource source)
{
  if (m_GraphSource == source)
    return;

  m_GraphSource = source;
  m_fHoverTime = -1.0;
  m_bDraggingRange = false;

  // a range belongs to the capture; the live graphs go with the live tabs
  if (source == GraphSource::Live && HasRange())
  {
    m_fRangeA = m_fRangeB = -1.0;
    ApplyRange();
  }

  m_bDrawDirty = true;
}

const plDynamicArray<plAudioProfilerCapture::Sample>& plEditorUiAudioProfilerView::GetGraphSamples() const
{
  return IsGraphLive() ? m_LiveSamples : m_pCapture->GetSamples();
}

void plEditorUiAudioProfilerView::GetGraphSpan(double& out_fStart, double& out_fEnd) const
{
  if (IsGraphLive())
  {
    out_fEnd = m_LiveSamples.IsEmpty() ? 0.0 : m_LiveSamples.PeekBack().m_fTime;
    out_fStart = out_fEnd - LiveWindowSeconds;
    return;
  }

  out_fStart = 0.0;
  out_fEnd = plMath::Max(m_pCapture->GetSamples().PeekBack().m_fTime, 1.0);
}

void plEditorUiAudioProfilerView::AddLiveSample(const plAudioProfilingSnapshot& snapshot)
{
  // snapshots before version 3 have no time of their own
  const double fTime = snapshot.m_fTime > 0.0 ? snapshot.m_fTime : plTime::Now().GetSeconds();

  if (!m_LiveSamples.IsEmpty())
  {
    // the same snapshot can arrive twice (a poll before the next one was taken)
    if (fTime == m_LiveSamples.PeekBack().m_fTime)
      return;

    // a clock that went back is a new target or a restarted one: the window starts over
    if (fTime < m_LiveSamples.PeekBack().m_fTime)
    {
      m_LiveSamples.Clear();
    }
  }

  const bool bFirst = m_LiveSamples.IsEmpty();

  plAudioProfilerCapture::Sample& sample = m_LiveSamples.ExpandAndGetRef();
  sample.m_fTime = fTime;
  sample.m_uiReal = snapshot.m_uiRealVoices;
  sample.m_uiVirtual = snapshot.m_uiVirtualVoices;
  sample.m_uiMaxReal = snapshot.m_uiMaxReal;
  sample.m_uiMaxVirtual = snapshot.m_uiMaxVirtual;
  sample.m_fDspLoad = snapshot.m_fDspLoad;
  sample.m_uiStreams = snapshot.m_uiStreams;
  sample.m_uiMemoryBytes = snapshot.m_uiMemoryBytes;
  sample.m_uiBudgetBytes = snapshot.m_uiMemoryBudgetBytes;
  sample.m_uiUnderruns = (!bFirst && snapshot.m_uiStreamUnderruns > m_uiLiveUnderruns) ? static_cast<plUInt32>(snapshot.m_uiStreamUnderruns - m_uiLiveUnderruns) : 0;
  m_uiLiveUnderruns = snapshot.m_uiStreamUnderruns;

  // what left the window is forgotten; only a recording keeps it
  plUInt32 uiOld = 0;
  while (uiOld < m_LiveSamples.GetCount() && fTime - m_LiveSamples[uiOld].m_fTime > LiveWindowSeconds)
    ++uiOld;

  if (uiOld > 0)
    m_LiveSamples.RemoveAtAndCopy(0, uiOld);

  if (IsGraphLive())
    m_bDrawDirty = true;
}

void plEditorUiAudioProfilerView::ShowToast(plStringView sText)
{
  m_sToast = sText;
  m_ToastTime = plTime::Now();
  m_bDrawDirty = true;
}

void plEditorUiAudioProfilerView::GetRange(double& out_fStart, double& out_fEnd) const
{
  if (HasRange())
  {
    out_fStart = plMath::Min(m_fRangeA, m_fRangeB);
    out_fEnd = plMath::Max(m_fRangeA, m_fRangeB);
    return;
  }

  out_fStart = 0.0;
  out_fEnd = (m_pCapture != nullptr && !m_pCapture->IsEmpty()) ? m_pCapture->GetSamples().PeekBack().m_fTime : 0.0;
}

void plEditorUiAudioProfilerView::ApplyRange()
{
  m_RangeLog.Clear();

  if (HasRange() && m_pCapture != nullptr && !m_pCapture->IsEmpty())
  {
    double fStart = 0.0, fEnd = 0.0;
    GetRange(fStart, fEnd);

    // the tabs show the snapshot at the range's end
    if (m_pCapture->GetSnapshot(m_pCapture->FindSample(fEnd), m_Snapshot).Succeeded())
      m_bHasSnapshot = true;

    for (const plAudioProfilerCapture::LogLine& line : m_pCapture->GetLog())
    {
      if (line.m_fTime < fStart || line.m_fTime > fEnd)
        continue;

      plAudioProfilerLogLine& shown = m_RangeLog.ExpandAndGetRef();
      shown.m_fTime = line.m_fTime;
      shown.m_uiType = line.m_uiType;
      shown.m_sName = line.m_sName;
      shown.m_sDetail = line.m_sDetail;
    }
  }
  else
  {
    m_Snapshot = m_LiveSnapshot;
    m_bHasSnapshot = m_bHasLiveSnapshot;
  }

  RebuildVoiceRows();
  RebuildMemoryRows();
  RebuildVisibleLog();
  RebuildLanes();
  m_fScroll = 0.0f;
  m_bDrawDirty = true;
}

void plEditorUiAudioProfilerView::RebuildLanes()
{
  m_Lanes.Clear();
  if (m_pCapture == nullptr)
    return;

  double fStart = 0.0, fEnd = 0.0;
  GetRange(fStart, fEnd);

  // one lane per event, by name; its instances in start order
  const auto& instances = m_pCapture->GetInstances();
  for (plUInt32 i = 0; i < instances.GetCount(); ++i)
  {
    const plAudioProfilerCapture::Instance& instance = instances[i];
    if (instance.m_fEnd < fStart || instance.m_fStart > fEnd)
      continue;

    Lane* pLane = nullptr;
    for (Lane& lane : m_Lanes)
    {
      if (lane.m_sEvent == instance.m_sEvent)
        pLane = &lane;
    }

    if (pLane == nullptr)
    {
      pLane = &m_Lanes.ExpandAndGetRef();
      pLane->m_sEvent = instance.m_sEvent;
    }

    pLane->m_Instances.PushBack(i);
  }

  m_Lanes.Sort([](const Lane& a, const Lane& b)
    { return ShortName(a.m_sEvent).Compare_NoCase(ShortName(b.m_sEvent)) < 0; });
}

//////////////////////////////////////////////////////////////////////////
// Data

void plEditorUiAudioProfilerView::RebuildVoiceRows()
{
  m_VoiceQuery.m_sText = m_sVoiceFilter;
  m_VoiceQuery.m_uiMaxRows = s_uiMaxRows;
  m_uiVoiceMatches = m_VoiceQuery.Run(m_Snapshot, m_VoiceRows);

  // the selected voice keeps its last values once it ended; the ended list has its final ones. A selected ended row
  // stays ended even when its slot plays again under the same id.
  if (m_bHasSelection)
  {
    m_bSelectedVoiceEnded = true;
    for (const plAudioVoiceSnapshotEntry& voice : m_Snapshot.m_Voices)
    {
      if (voice.m_uiId == m_uiSelectedId && m_SelectedVoice.m_uiState != plAudioVoiceSnapshotEntry::Ended)
      {
        m_SelectedVoice = voice;
        m_bSelectedVoiceEnded = false;
        break;
      }
    }

    if (m_bSelectedVoiceEnded)
    {
      for (const plAudioEndedVoiceSnapshotEntry& ended : m_Snapshot.m_Ended)
      {
        if (ended.m_Voice.m_uiId == m_uiSelectedId)
          m_SelectedVoice = ended.m_Voice;
      }
    }
  }
}

void plEditorUiAudioProfilerView::SetPaused(bool bPaused)
{
  if (m_bPaused == bPaused)
    return;

  m_bPaused = bPaused;

  if (!bPaused)
  {
    // the lines that arrived while paused, then the latest snapshot
    for (const plAudioProfilerLogLine& line : m_PausedLines)
    {
      if (line.m_uiType != plAudioProfilerLogLine::GapType)
        ++m_uiLogReceived;

      m_LogLines.PushBack(line);
      if (m_LogLines.GetCount() > s_uiMaxLogLines)
        m_LogLines.PopFront();
    }
    m_PausedLines.Clear();

    if (!HasRange())
    {
      m_Snapshot = m_LiveSnapshot;
      m_bHasSnapshot = m_bHasLiveSnapshot;
    }

    RebuildVoiceRows();
    RebuildMemoryRows();
    RebuildVisibleLog();
  }

  m_bRebuild = true;
  m_bDrawDirty = true;
}

void plEditorUiAudioProfilerView::SortVoicesBy(plUInt32 uiColumn)
{
  using SortKey = plAudioProfilerVoiceQuery::SortKey;

  const SortKey key = s_VoiceColumnKeys[uiColumn];

  if (m_VoiceQuery.m_SortKey == key)
  {
    m_VoiceQuery.m_bAscending = !m_VoiceQuery.m_bAscending;
  }
  else
  {
    // names read A to Z first; numbers largest first, except audibility, whose default order is already the loudest first
    const bool bText = key == SortKey::Event || key == SortKey::Owner || key == SortKey::Bus || key == SortKey::Codec || key == SortKey::State;
    m_VoiceQuery.m_bAscending = bText || (key == SortKey::Audibility && m_VoiceQuery.m_SortKey == SortKey::Default);
    m_VoiceQuery.m_SortKey = key;
  }

  RebuildVoiceRows();
  m_bDrawDirty = true;
}

void plEditorUiAudioProfilerView::ShowBusMenu()
{
  // the loaded mixer's buses, and buses voices play on that it does not have
  plHybridArray<plString, 16> buses;
  for (const plAudioBusSnapshotEntry& bus : m_Snapshot.m_Buses)
    buses.PushBack(bus.m_sName);

  for (const plAudioVoiceSnapshotEntry& voice : m_Snapshot.m_Voices)
  {
    if (!voice.m_sBus.IsEmpty() && !buses.Contains(voice.m_sBus))
      buses.PushBack(voice.m_sBus);
  }

  plEditorUiMenu menu;
  plEditorUiMenuItem& all = menu.AddAction("All buses", 0);
  all.m_bCheckable = true;
  all.m_bChecked = m_VoiceQuery.m_sBus.IsEmpty();

  for (plUInt32 i = 0; i < buses.GetCount(); ++i)
  {
    plEditorUiMenuItem& item = menu.AddAction(buses[i], i + 1);
    item.m_bCheckable = true;
    item.m_bChecked = m_VoiceQuery.m_sBus == buses[i];
  }

  const plRectFloat tool = GetToolRowRect();
  plEditorUiMenuView::Show(*GetSurface(), plVec2(BusDropdownX(), tool.y + 3.0f + plEditorUi::ControlHeight), menu, [this, buses](plUInt32 uiId)
    {
      m_VoiceQuery.m_sBus = uiId == 0 ? plString() : buses[uiId - 1];
      RebuildVoiceRows();
      m_fScroll = 0.0f;
      m_bDrawDirty = true; });
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
  const plDeque<plAudioProfilerLogLine>& log = GetShownLog();

  m_VisibleLog.Clear();
  for (plUInt32 i = 0; i < log.GetCount(); ++i)
  {
    if (IsLogLineVisible(log[i]))
      m_VisibleLog.PushBack(i);
  }
}

//////////////////////////////////////////////////////////////////////////
// Layout

plRectFloat plEditorUiAudioProfilerView::GetBodyRect() const
{
  const plVec2 size = IsReady() ? GetSurface()->GetLogicalSize() : plVec2(800.0f, 400.0f);
  const float y = s_fHeaderRow + GetGraphHeight() + s_fTabRow;
  return plRectFloat(0.0f, y, size.x, plMath::Max(size.y - y - s_fStatus, 1.0f));
}

plRectFloat plEditorUiAudioProfilerView::GetToolRowRect() const
{
  const plRectFloat body = GetBodyRect();
  const float fTop = m_Tab == Tab::Voices ? s_fSummaryRow : 0.0f;
  return plRectFloat(body.x, body.y + fTop, body.width, s_fToolRow);
}

plRectFloat plEditorUiAudioProfilerView::GetVoiceSummaryRect() const
{
  const plRectFloat body = GetBodyRect();
  return plRectFloat(body.x, body.y, body.width, s_fSummaryRow);
}

plRectFloat plEditorUiAudioProfilerView::GetVoiceCardRect(plUInt32 uiCard) const
{
  const plRectFloat summary = GetVoiceSummaryRect();
  const float fWidth = summary.width / s_uiSummaryCards;
  return plRectFloat(summary.x + uiCard * fWidth, summary.y, fWidth, summary.height - 1.0f);
}

float plEditorUiAudioProfilerView::GetVoiceRowsTop() const
{
  const float fBanner = m_uiVoiceMatches > m_VoiceRows.GetCount() ? s_fBannerRow : 0.0f;
  return GetTableRect().y + fBanner + s_fTableHeader;
}

void plEditorUiAudioProfilerView::LayoutVoiceColumns(plHybridArray<float, 12>& out_x, plHybridArray<float, 12>& out_w) const
{
  const plRectFloat table = GetTableRect();
  LayoutColumns(table.x, table.width, plArrayPtr<const float>(s_fVoiceColumnWidths), out_x, out_w);
}

float plEditorUiAudioProfilerView::GetGraphSourceX() const
{
  // left of the range text, which sits left of Clear range
  const plVec2 size = IsReady() ? GetSurface()->GetLogicalSize() : plVec2(800.0f, 400.0f);
  const float fButtons = plAudioEngineUi::TextButtonWidth("Live", false) + 2.0f + plAudioEngineUi::TextButtonWidth("Capture", false);
  return size.x - 8.0f - plAudioEngineUi::TextButtonWidth("Clear range", false) - 210.0f - 8.0f - fButtons;
}

float plEditorUiAudioProfilerView::GetPauseButtonX() const
{
  // after Record, Save capture and Open...; the target follows it
  return 6.0f + plAudioEngineUi::TextButtonWidth("Record", true) + 4.0f + plAudioEngineUi::TextButtonWidth("Save capture", false) + 4.0f + plAudioEngineUi::TextButtonWidth("Open...", false) + 6.0f;
}

plRectFloat plEditorUiAudioProfilerView::GetTableRect() const
{
  const plRectFloat body = GetBodyRect();

  switch (m_Tab)
  {
    case Tab::Instances:
      return plRectFloat(body.x, body.y, plMath::Max(body.width - s_fCardWidth, 100.0f), body.height);
    case Tab::Voices:
      return plRectFloat(body.x, body.y + s_fSummaryRow + s_fToolRow, plMath::Max(body.width - s_fDetailsWidth, 100.0f), plMath::Max(body.height - s_fSummaryRow - s_fToolRow, 1.0f));
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
    case Tab::Instances:
      return "Instances";
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
    case Tab::Instances:
    {
      plUInt32 uiInstances = 0;
      for (const Lane& lane : m_Lanes)
        uiInstances += lane.m_Instances.GetCount();
      if (m_pCapture != nullptr && !m_pCapture->IsEmpty())
        s.SetFormat("{}", uiInstances);
      break;
    }

    case Tab::Log:
      s.SetFormat("{}", HasRange() ? m_RangeLog.GetCount() : m_uiLogReceived);
      break;

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
        plAudioEngineUi::FormatBytes(m_Snapshot.m_uiMemoryBytes, s);
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

  // the cap banner moves the voice table's sort headers
  if (m_Tab == Tab::Voices && (m_uiVoiceMatches > m_VoiceRows.GetCount()) != m_bBuiltVoiceBanner)
    m_bRebuild = true;

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
  m_bBuiltVoiceBanner = m_uiVoiceMatches > m_VoiceRows.GetCount();
  plBlackboard& board = GetSurface()->GetBlackboard();

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = Surface();
  DrawList(pRoot, "Profiler.Drawing", s_sDrawing.GetView());

  // the capture bar: Record, Save capture, Open..., the target; Clear range at the right end
  {
    const float y = 3.0f;
    float x = 6.0f;

    const float fRecord = plAudioEngineUi::TextButtonWidth("Record", true);
    plAudioEngineUi::HitButton(pRoot, plRectFloat(x, y, fRecord, ControlHeight), "Ap.Record", "Record the target's snapshots into the capture; stop to keep it", &board);
    x += fRecord + 4.0f;

    const float fSave = plAudioEngineUi::TextButtonWidth("Save capture", false);
    plAudioEngineUi::HitButton(pRoot, plRectFloat(x, y, fSave, ControlHeight), "Ap.Save", "Save the capture as a .plAudioCapture file", &board);
    x += fSave + 4.0f;

    const float fOpen = plAudioEngineUi::TextButtonWidth("Open...", false);
    plAudioEngineUi::HitButton(pRoot, plRectFloat(x, y, fOpen, ControlHeight), "Ap.Open", "Open a saved .plAudioCapture", &board);

    x = GetPauseButtonX();
    const float fPause = plAudioEngineUi::TextButtonWidth("Resume view", false);
    plAudioEngineUi::HitButton(pRoot, plRectFloat(x, y, fPause, ControlHeight), "Ap.Pause", "Keep the tabs on the snapshot they show; the target keeps running and Record keeps recording", &board);
    x += fPause + 14.0f;

    plAudioEngineUi::HitButton(pRoot, plRectFloat(x, y, 260.0f, ControlHeight), "Ap.Target", "What the profiler listens to: the editor's engine process, or a Player through its telemetry", &board);

    const float fLive = plAudioEngineUi::TextButtonWidth("Live", false);
    plAudioEngineUi::HitButton(pRoot, plRectFloat(GetGraphSourceX(), y, fLive, ControlHeight), "Ap.GraphLive", "Graphs: the last 60 seconds of the target, recorded or not; what leaves the window is dropped", &board);
    plAudioEngineUi::HitButton(pRoot, plRectFloat(GetGraphSourceX() + fLive + 2.0f, y, plAudioEngineUi::TextButtonWidth("Capture", false), ControlHeight), "Ap.GraphCapture",
      "Graphs: the whole recording or opened capture, with ranges and instance lanes", &board);

    const float fClear = plAudioEngineUi::TextButtonWidth("Clear range", false);
    plAudioEngineUi::HitButton(pRoot, plRectFloat(m_vBuiltSize.x - 8.0f - fClear, y, fClear, ControlHeight), "Ap.ClearRange", "Show the whole capture and the live state again", &board);
  }

  // tabs
  float x = 6.0f;
  const float fTabY = s_fHeaderRow + GetGraphHeight() + 2.0f;
  for (plUInt32 i = 0; i < static_cast<plUInt32>(Tab::Count); ++i)
  {
    const float fWidth = GetTabWidth(static_cast<Tab>(i));
    m_BuiltTabWidths[i] = fWidth;
    plAudioEngineUi::HitButton(pRoot, plRectFloat(x, fTabY, fWidth, plEditorUiTabs::Height), IndexedName("Ap.Tab", i));
    x += fWidth;
  }

  // the selected instance's links
  if (m_Tab == Tab::Instances && m_iSelectedInstance >= 0)
  {
    const plRectFloat card = GetDetailsRect();
    const float fY = card.y + plAudioEngineUi::SectionHeight + 6.0f;
    const float fEntity = plAudioEngineUi::TextButtonWidth("Select entity", false);
    plAudioEngineUi::HitButton(pRoot, plRectFloat(card.x + 12.0f, fY, fEntity, ControlHeight), "Ap.SelectEntity", "Select the objects with the entity's name in the open scene", &board);
    plAudioEngineUi::HitButton(pRoot, plRectFloat(card.x + 12.0f + fEntity + 6.0f, fY, plAudioEngineUi::TextButtonWidth("Open event", false), ControlHeight), "Ap.OpenEvent", "Open the event asset", &board);
  }

  const plRectFloat tool = GetToolRowRect();
  const float fToolY = tool.y + 3.0f;

  if (m_Tab == Tab::Voices)
  {
    x = 8.0f + MeasureText("State", FontSize - 1) + 10.0f;
    for (plUInt32 i = 0; i < 4; ++i)
    {
      const float fWidth = StateChipWidth(i);
      plAudioEngineUi::HitButton(pRoot, plRectFloat(x, fToolY + 1.0f, fWidth, 22.0f), IndexedName("Ap.State", i), s_szStateTips[i], &board);
      x += fWidth + 4.0f;
    }

    x = BusDropdownX();
    plAudioEngineUi::HitButton(pRoot, plRectFloat(x, fToolY, s_fBusDropdownWidth, ControlHeight), "Ap.Bus", "Show the voices of one mixer bus", &board);

    plUiPanel* pSearch = Panel(pRoot);
    plAudioEngineUi::Place(pSearch, plRectFloat(x + s_fBusDropdownWidth + 8.0f, fToolY, s_fSearchWidth, ControlHeight));
    pSearch->m_Color = plColorGammaUB(0, 0, 0, 0);
    m_pVoiceSearch->Build(pSearch, "filter event, owner or bus...");
    m_pVoiceSearch->SetText(m_sVoiceFilter);

    // the column headers sort; they move down with the cap banner
    plHybridArray<float, 12> xs, ws;
    LayoutVoiceColumns(xs, ws);
    const float fHeaderY = GetVoiceRowsTop() - s_fTableHeader;
    for (plUInt32 i = 0; i < s_uiVoiceColumns; ++i)
    {
      plStringBuilder sTip;
      sTip.SetFormat("Sort by {}; click again to reverse", s_szVoiceColumns[i]);
      plAudioEngineUi::HitButton(pRoot, plRectFloat(xs[i], fHeaderY, ws[i], s_fTableHeader), IndexedName("Ap.Sort", i), sTip, &board);
    }

    // the selected voice's links
    if (m_bHasSelection)
    {
      const plRectFloat details = GetDetailsRect();
      const float fY = details.y + plAudioEngineUi::SectionHeight + 6.0f;
      const float fOwner = plAudioEngineUi::TextButtonWidth("Select owner", false);
      plAudioEngineUi::HitButton(pRoot, plRectFloat(details.x + 12.0f, fY, fOwner, ControlHeight), "Ap.VoiceOwner", "Select the objects with the owner's name in the open scene", &board);
      plAudioEngineUi::HitButton(pRoot, plRectFloat(details.x + 12.0f + fOwner + 6.0f, fY, plAudioEngineUi::TextButtonWidth("Open event", false), ControlHeight), "Ap.VoiceEvent", "Open the event asset", &board);
    }
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

  if (sId == "Ap.Record")
  {
    if (m_OnRecordToggled.IsValid())
      m_OnRecordToggled(!m_bRecording);
    return;
  }

  if (sId == "Ap.GraphLive" || sId == "Ap.GraphCapture")
  {
    const bool bHasCapture = m_pCapture != nullptr && !m_pCapture->IsEmpty();
    if (sId == "Ap.GraphLive" || bHasCapture)
      SetGraphSource(sId == "Ap.GraphLive" ? GraphSource::Live : GraphSource::Capture);
    return;
  }

  // an opened capture is what the graphs show next
  if (sId == "Ap.Open")
    SetGraphSource(GraphSource::Capture);

  if (sId == "Ap.Save" || sId == "Ap.Open")
  {
    // the file dialog runs its own loop; it opens after this click has been handled
    plDelegate<void()> action = sId == "Ap.Save" ? m_OnSave : m_OnOpen;
    if (action.IsValid())
      plEditorUiRuntime::RunAfterFrame([action]()
        { action(); });
    return;
  }

  if (sId == "Ap.Target")
  {
    ShowTargetMenu();
    return;
  }

  if (sId == "Ap.Pause")
  {
    SetPaused(!m_bPaused);
    return;
  }

  if (sId == "Ap.Bus")
  {
    ShowBusMenu();
    return;
  }

  if (sId == "Ap.VoiceOwner" || sId == "Ap.VoiceEvent")
  {
    if (!m_bHasSelection)
      return;

    const plString sTarget = sId == "Ap.VoiceOwner" ? m_SelectedVoice.m_sEntity : m_SelectedVoice.m_sEvent;
    plDelegate<void(plStringView)> action = sId == "Ap.VoiceOwner" ? m_OnSelectEntity : m_OnOpenEvent;

    // opening a document subscribes to the events this click is dispatched by
    if (action.IsValid() && !sTarget.IsEmpty())
      plEditorUiRuntime::RunAfterFrame([action, sTarget]()
        { action(sTarget); });
    return;
  }

  if (sId == "Ap.ClearRange")
  {
    m_fRangeA = m_fRangeB = -1.0;
    ApplyRange();
    return;
  }

  if (sId == "Ap.SelectEntity" || sId == "Ap.OpenEvent")
  {
    if (m_pCapture == nullptr || m_iSelectedInstance < 0)
      return;

    const plAudioProfilerCapture::Instance& instance = m_pCapture->GetInstances()[m_iSelectedInstance];
    const plString sTarget = sId == "Ap.SelectEntity" ? instance.m_sEntity : instance.m_sEvent;
    plDelegate<void(plStringView)> action = sId == "Ap.SelectEntity" ? m_OnSelectEntity : m_OnOpenEvent;

    // opening a document subscribes to the events this click is dispatched by
    if (action.IsValid())
      plEditorUiRuntime::RunAfterFrame([action, sTarget]()
        { action(sTarget); });
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
  else if (const plInt32 iState = indexOf("Ap.State"); iState >= 0 && iState < 4)
  {
    m_VoiceQuery.m_StateFilter[iState] = !m_VoiceQuery.m_StateFilter[iState];
    RebuildVoiceRows();
    m_bRebuild = true; // the cap banner can come or go, which moves the sort headers
    m_bDrawDirty = true;
  }
  else if (const plInt32 iColumn = indexOf("Ap.Sort"); iColumn >= 0 && iColumn < static_cast<plInt32>(s_uiVoiceColumns))
  {
    SortVoicesBy(static_cast<plUInt32>(iColumn));
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
  DrawGraphs();

  switch (m_Tab)
  {
    case Tab::Instances:
      DrawInstances();
      break;
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

  // the capture bar: Record with its dot, Save capture, Open..., the target with its link state
  {
    const float y = 3.0f;
    float x = 6.0f;

    const float fRecord = plAudioEngineUi::TextButtonWidth("Record", true);
    const plRectFloat record(x, y, fRecord, ControlHeight);
    // the plate, then a dot where the icon goes: red while recording
    plAudioEngineUi::TextButton(m_Writer, record, "");
    m_Writer.Circle(plVec2(record.x + 14.0f, record.y + record.height * 0.5f), 4.5f, m_bRecording ? Danger() : Dim());
    m_Writer.Text(plRectFloat(record.x + 24.0f, record.y, record.width - 28.0f, record.height), "Record", Foreground(), FontSize - 1);
    if (m_bRecording)
      m_Writer.RectOutline(record, Danger(), 1.0f, 4.0f);
    x += fRecord + 4.0f;

    const float fSave = plAudioEngineUi::TextButtonWidth("Save capture", false);
    plAudioEngineUi::TextButton(m_Writer, plRectFloat(x, y, fSave, ControlHeight), "Save capture", {}, m_pCapture != nullptr && !m_pCapture->IsEmpty());
    x += fSave + 4.0f;

    const float fOpen = plAudioEngineUi::TextButtonWidth("Open...", false);
    plAudioEngineUi::TextButton(m_Writer, plRectFloat(x, y, fOpen, ControlHeight), "Open...");

    // Pause view, outlined in the warning colour while paused
    x = GetPauseButtonX();
    const plRectFloat pause(x, y, plAudioEngineUi::TextButtonWidth("Resume view", false), ControlHeight);
    plAudioEngineUi::TextButton(m_Writer, pause, m_bPaused ? "Resume view" : "Pause view");
    if (m_bPaused)
      m_Writer.RectOutline(pause, Warning(), 1.0f, 4.0f);
    x += pause.width + 14.0f;

    // the target, and whether it answers
    plColorGammaUB dot = Danger();
    plStringBuilder sState;
    switch (m_LinkState)
    {
      case LinkState::NoEngine:
        sState = "not connected";
        break;
      case LinkState::NoData:
        dot = Warning();
        sState = "no PlasmaAudio data";
        break;
      case LinkState::Live:
        dot = Accent();
        sState = "live";
        break;
    }

    if (m_bPaused)
    {
      dot = Warning();
      sState.Append(" \xC2\xB7 view paused");
    }

    plAudioEngineUi::Dropdown(m_Writer, plRectFloat(x, y, 260.0f, ControlHeight), "Target", m_sTargetText);
    x += 260.0f + 10.0f;
    plAudioEngineUi::DotText(m_Writer, plRectFloat(x, 0.0f, 200.0f, s_fHeaderRow), sState, dot, plAudioEngineUi::Mix(Foreground(), dot, 0.82f), FontSize - 1);

    // the range and Clear range
    const float fClear = plAudioEngineUi::TextButtonWidth("Clear range", false);
    plAudioEngineUi::TextButton(m_Writer, plRectFloat(size.x - 8.0f - fClear, y, fClear, ControlHeight), "Clear range", {}, HasRange());

    // Live and Capture: what the graphs plot; Capture waits for a recording or an opened file
    {
      const bool bLive = IsGraphLive();
      const bool bHasCapture = m_pCapture != nullptr && !m_pCapture->IsEmpty();
      const plRectFloat live(GetGraphSourceX(), y, plAudioEngineUi::TextButtonWidth("Live", false), ControlHeight);
      const plRectFloat capture(live.x + live.width + 2.0f, y, plAudioEngineUi::TextButtonWidth("Capture", false), ControlHeight);
      plAudioEngineUi::TextButton(m_Writer, live, "Live");
      plAudioEngineUi::TextButton(m_Writer, capture, "Capture", {}, bHasCapture);
      m_Writer.RectOutline(bLive ? live : capture, Accent(), 1.0f, 4.0f);
    }

    plStringBuilder sRange = IsGraphLive() ? "live: last 60 s" : "range: whole capture";
    if (HasRange())
    {
      double fStart = 0.0, fEnd = 0.0;
      GetRange(fStart, fEnd);
      sRange.SetFormat("range: {} - {} s", plArgF(fStart, 1), plArgF(fEnd, 1));
    }
    m_Writer.Text(plRectFloat(size.x - 8.0f - fClear - 210.0f, 0.0f, 200.0f, s_fHeaderRow), sRange, Muted(), FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
  }

  m_Writer.Rect(plRectFloat(0.0f, s_fHeaderRow - 1.0f, size.x, 1.0f), Line());

  // dock-style tabs with count pills, under the graphs
  const float fTabsTop = s_fHeaderRow + GetGraphHeight();
  float x = 6.0f;
  for (plUInt32 i = 0; i < static_cast<plUInt32>(Tab::Count); ++i)
  {
    const Tab tab = static_cast<Tab>(i);
    const float fWidth = GetTabWidth(tab);
    const bool bCurrent = tab == m_Tab;
    const plRectFloat rect(x, fTabsTop + 2.0f, fWidth, plEditorUiTabs::Height);

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

  m_Writer.Rect(plRectFloat(0.0f, fTabsTop + s_fTabRow - 1.0f, size.x, 1.0f), Line());
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

  DrawVoiceSummary();

  // tool row: state chips with their counts, the bus dropdown; the search field is a retained element
  plUInt32 counts[4] = {};
  plAudioProfilerVoiceQuery::CountStates(m_Snapshot, counts);

  m_Writer.Rect(tool, Surface());
  m_Writer.Text(plRectFloat(8.0f, tool.y, 60.0f, s_fToolRow), "State", Dim(), FontSize - 1);
  float x = 8.0f + MeasureText("State", FontSize - 1) + 10.0f;
  for (plUInt32 i = 0; i < 4; ++i)
  {
    const float fWidth = StateChipWidth(i);
    const plRectFloat chip(x, tool.y + 4.0f, fWidth, 22.0f);
    plAudioEngineUi::Chip(m_Writer, chip, s_szStateNames[i], StateColor(static_cast<plUInt8>(i)), m_VoiceQuery.m_StateFilter[i]);

    // the count in the room StateChipWidth leaves at the chip's right end
    plStringBuilder sCount;
    sCount.SetFormat("{}", counts[i]);
    m_Writer.Text(plRectFloat(chip.x + fWidth - 32.0f, chip.y, 24.0f, chip.height), sCount, m_VoiceQuery.m_StateFilter[i] ? Muted() : Dim(), MinFontSize, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
    x += fWidth + 4.0f;
  }
  m_Writer.Rect(plRectFloat(x + 4.0f, tool.y + 7.0f, 1.0f, 16.0f), Line());

  plAudioEngineUi::Dropdown(m_Writer, plRectFloat(BusDropdownX(), tool.y + 3.0f, s_fBusDropdownWidth, ControlHeight), "Bus", m_VoiceQuery.m_sBus.IsEmpty() ? plStringView("All") : m_VoiceQuery.m_sBus.GetView());

  // how many rows the filters leave, of all voices
  plStringBuilder sShown;
  const plUInt32 uiAll = m_Snapshot.m_Voices.GetCount() + (m_VoiceQuery.m_StateFilter[plAudioVoiceSnapshotEntry::Ended] ? m_Snapshot.m_Ended.GetCount() : 0);
  sShown.SetFormat("{} of {} shown", m_VoiceRows.GetCount(), uiAll);
  m_Writer.Text(plRectFloat(table.x + table.width - 160.0f, tool.y, 152.0f, s_fToolRow), sShown, Dim(), MinFontSize + 1, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
  m_Writer.Rect(plRectFloat(0.0f, tool.y + s_fToolRow - 1.0f, tool.width, 1.0f), Line());

  m_Writer.Rect(table, Surface());

  // the cap banner: rows the query cut are announced, never silently dropped
  plRectFloat header = table;
  if (m_uiVoiceMatches > m_VoiceRows.GetCount())
  {
    const plRectFloat banner(table.x, table.y, table.width, s_fBannerRow);
    m_Writer.Rect(banner, plAudioEngineUi::Mix(Surface(), Warning(), 0.14f));

    plStringBuilder sBanner;
    sBanner.SetFormat("Showing {} of {} voices. Filter by state, bus or name to see the rest.", m_VoiceRows.GetCount(), m_uiVoiceMatches);
    m_Writer.Text(plRectFloat(banner.x + 10.0f, banner.y, banner.width - 20.0f, banner.height), sBanner, Warning(), FontSize - 2);

    header = plRectFloat(table.x, table.y + s_fBannerRow, table.width, table.height - s_fBannerRow);
  }

  // the header marks the sort column; the default order is the loudest first
  const plAudioProfilerVoiceQuery::SortKey sortKey = m_VoiceQuery.m_SortKey == plAudioProfilerVoiceQuery::SortKey::Default ? plAudioProfilerVoiceQuery::SortKey::Audibility : m_VoiceQuery.m_SortKey;
  const bool bAscending = m_VoiceQuery.m_SortKey != plAudioProfilerVoiceQuery::SortKey::Default && m_VoiceQuery.m_bAscending;

  plStringBuilder titles[s_uiVoiceColumns];
  const char* szTitles[s_uiVoiceColumns] = {};
  for (plUInt32 i = 0; i < s_uiVoiceColumns; ++i)
  {
    titles[i] = s_szVoiceColumns[i];
    if (s_VoiceColumnKeys[i] == sortKey)
      titles[i].Append(bAscending ? " \xE2\x96\xB4" : " \xE2\x96\xBE");
    szTitles[i] = titles[i].GetData();
  }
  DrawTableHeader(header, plArrayPtr<const char* const>(szTitles), plArrayPtr<const float>(s_fVoiceColumnWidths), plArrayPtr<const bool>(s_bVoiceColumnRight));

  plHybridArray<float, 12> xs, ws;
  LayoutVoiceColumns(xs, ws);

  const float fRowsTop = GetVoiceRowsTop();
  const plRectFloat rows(table.x, fRowsTop, table.width, plMath::Max(table.y + table.height - fRowsTop, 1.0f));
  m_Writer.PushClip(rows);

  for (plUInt32 r = 0; r < m_VoiceRows.GetCount(); ++r)
  {
    const float y = rows.y + r * s_fRow - m_fScroll;
    if (y + s_fRow < rows.y || y > rows.y + rows.height)
      continue;

    // a slot that plays again reuses the id of the voice that ended in it, so ended and live rows are told apart
    const plAudioVoiceSnapshotEntry& voice = *m_VoiceRows[r];
    const bool bEnded = voice.m_uiState == plAudioVoiceSnapshotEntry::Ended;
    const bool bSelected = m_bHasSelection && voice.m_uiId == m_uiSelectedId && bEnded == (m_SelectedVoice.m_uiState == plAudioVoiceSnapshotEntry::Ended);
    const plColorGammaUB text = bEnded ? Dim() : (bSelected ? Foreground() : Muted());

    if (bSelected)
      m_Writer.Rect(plRectFloat(rows.x, y, rows.width, s_fRow), Selection());
    m_Writer.Rect(plRectFloat(rows.x, y + s_fRow - 1.0f, rows.width, 1.0f), plAudioEngineUi::Mix(Surface(), Line(), 0.6f));

    auto cell = [&](plUInt32 c)
    { return plRectFloat(xs[c] + 8.0f, y, ws[c] - 16.0f, s_fRow); };

    m_Writer.Text(cell(0), ShortName(voice.m_sEvent), bEnded ? Dim() : Foreground(), FontSize - 1);
    m_Writer.Text(cell(1), OwnerText(voice), voice.m_uiEntityId == 0 ? Dim() : text, FontSize - 1);

    // a stolen or killed voice says so in the danger colour
    const bool bForced = plAudioStopReason::IsForced(static_cast<plAudioStopReason::Enum>(voice.m_uiStopReason));
    const plColorGammaUB stateText = bForced ? Danger() : (bEnded ? Dim() : plAudioEngineUi::Mix(Foreground(), StateColor(voice.m_uiState), 0.82f));
    plAudioEngineUi::DotText(m_Writer, cell(2), StateText(voice), StateColor(voice.m_uiState), stateText, FontSize - 2);

    plStringBuilder s;
    s.SetFormat("{}", voice.m_uiPriority);
    m_Writer.Text(cell(3), s, text, FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);

    const plRectFloat audibility = cell(4);
    plAudioEngineUi::ValueBar(m_Writer, plRectFloat(audibility.x, y + 8.0f, plMath::Min(70.0f, audibility.width), 6.0f), voice.m_fAudibility, Muted());

    m_Writer.Text(cell(5), VolumeText(voice.m_fVolume), text, FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);

    if (voice.m_uiEntityId != 0)
      s.SetFormat("{} m", plArgF(voice.m_fDistance, 1));
    else
      s = "-";
    m_Writer.Text(cell(6), s, text, FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
    m_Writer.Text(cell(7), voice.m_sBus, text, FontSize - 1);
    m_Writer.Text(cell(8), CodecText(voice), text, FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
  }

  m_Writer.PopClip();

  if (m_VoiceRows.IsEmpty())
  {
    const char* szEmpty = "No data from the target: it publishes snapshots while a scene simulates or plays";
    if (m_bHasSnapshot)
      szEmpty = (m_Snapshot.m_Voices.IsEmpty() && m_Snapshot.m_Ended.IsEmpty()) ? "No voices" : "No voices match the filters";

    m_Writer.Text(plRectFloat(rows.x, rows.y + 8.0f, rows.width, 20.0f), szEmpty, Dim(), FontSize - 1, plUiTextHAlign::Center);
  }

  DrawVoiceDetails(GetDetailsRect());
}

void plEditorUiAudioProfilerView::DrawVoiceSummary()
{
  using namespace plEditorUi;

  const plAudioProfilingSnapshot& s = m_Snapshot;
  const plRectFloat summary = GetVoiceSummaryRect();
  m_Writer.Rect(summary, Line());

  plUInt32 uiStolen = 0, uiKilled = 0;
  for (const plAudioEndedVoiceSnapshotEntry& ended : s.m_Ended)
  {
    const auto reason = static_cast<plAudioStopReason::Enum>(ended.m_Voice.m_uiStopReason);
    if (reason == plAudioStopReason::StolenOldest || reason == plAudioStopReason::StolenQuietest)
      ++uiStolen;
    else if (plAudioStopReason::IsForced(reason))
      ++uiKilled;
  }

  for (plUInt32 i = 0; i < s_uiSummaryCards; ++i)
  {
    const plRectFloat card = GetVoiceCardRect(i);
    m_Writer.Rect(plRectFloat(card.x, card.y, card.width - (i + 1 < s_uiSummaryCards ? 1.0f : 0.0f), card.height), Surface());

    // title on the left, scope on the right
    plStringBuilder sTitle = s_szCardTitles[i];
    sTitle.ToUpper();
    m_Writer.Text(plRectFloat(card.x + 10.0f, card.y + 4.0f, card.width - 20.0f, 14.0f), sTitle, Dim(), MinFontSize);
    m_Writer.Text(plRectFloat(card.x + 10.0f, card.y + 4.0f, card.width - 20.0f, 14.0f), s_szCardScopes[i], Dim(), MinFontSize, plUiTextHAlign::Right);

    const plRectFloat value(card.x + 10.0f, card.y + 18.0f, card.width - 20.0f, 18.0f);
    const plRectFloat foot(card.x + 10.0f, card.y + 36.0f, card.width - 20.0f, 12.0f);

    if (!m_bHasSnapshot)
    {
      m_Writer.Text(value, "-", Muted(), FontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
      continue;
    }

    plStringBuilder sValue, sFoot;
    float fBar = -1.0f;
    plColorGammaUB bar = Accent();

    switch (i)
    {
      case 0:
        sValue.SetFormat("{} / {}", s.m_uiRealVoices, s.m_uiMaxReal);
        fBar = s.m_uiMaxReal > 0 ? static_cast<float>(s.m_uiRealVoices) / s.m_uiMaxReal : 0.0f;
        bar = fBar >= 1.0f ? Warning() : Accent();
        break;
      case 1:
        sValue.SetFormat("{} / {}", s.m_uiVirtualVoices, s.m_uiMaxVirtual);
        fBar = s.m_uiMaxVirtual > 0 ? static_cast<float>(s.m_uiVirtualVoices) / s.m_uiMaxVirtual : 0.0f;
        bar = plAudioEngineUi::CategoryColor("XR");
        break;
      case 2:
        sValue.SetFormat("{}", s.m_Ended.GetCount());
        sFoot.SetFormat("{} stolen \xC2\xB7 {} killed \xC2\xB7 {} other", uiStolen, uiKilled, s.m_Ended.GetCount() - uiStolen - uiKilled);
        break;
      case 3:
        sValue.SetFormat("{}%", plArgF(s.m_fDspLoad * 100.0f, 1));
        fBar = s.m_fDspLoad;
        bar = s.m_fDspLoad >= 0.8f ? Danger() : Accent();
        break;
      case 4:
        sValue.SetFormat("{} ms", plArgF(s.m_fUpdateMs, 2));
        sFoot.SetFormat("{} instances \xC2\xB7 {} streams", s.m_uiInstances, s.m_uiStreams);
        break;
      default:
      {
        plStringBuilder sBytes, sBudget;
        plAudioEngineUi::FormatBytes(s.m_uiMemoryBytes, sBytes);
        if (s.m_uiMemoryBudgetBytes > 0)
        {
          plAudioEngineUi::FormatBytes(s.m_uiMemoryBudgetBytes, sBudget);
          sValue.SetFormat("{} / {}", sBytes, sBudget);
          fBar = static_cast<float>(static_cast<double>(s.m_uiMemoryBytes) / static_cast<double>(s.m_uiMemoryBudgetBytes));
          bar = fBar > 1.0f ? Danger() : plAudioEngineUi::AudioColor();
        }
        else
        {
          sValue = sBytes;
          sFoot = "no bank budget";
        }
        break;
      }
    }

    m_Writer.Text(value, sValue, Foreground(), FontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);

    if (fBar >= 0.0f)
      plAudioEngineUi::ValueBar(m_Writer, plRectFloat(foot.x, foot.y + 4.0f, foot.width, 4.0f), plMath::Min(fBar, 1.0f), bar);
    else
      m_Writer.Text(foot, sFoot, (i == 2 && uiStolen + uiKilled > 0) ? Danger() : Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center);
  }
}

void plEditorUiAudioProfilerView::DrawVoiceDetails(const plRectFloat& details)
{
  using namespace plEditorUi;

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
  const bool bEnded = m_bSelectedVoiceEnded || voice.m_uiState == plAudioVoiceSnapshotEntry::Ended;
  plAudioEngineUi::SectionHeader(m_Writer, section, ShortName(voice.m_sEvent), plAudioEngineUi::AudioColor());

  // Select owner and Open event first, so a short panel shows them (the document's hit buttons sit on them)
  const float fButtonY = section.y + section.height + 6.0f;
  const float fOwner = plAudioEngineUi::TextButtonWidth("Select owner", false);
  plAudioEngineUi::TextButton(m_Writer, plRectFloat(details.x + 12.0f, fButtonY, fOwner, ControlHeight), "Select owner", {}, !voice.m_sEntity.IsEmpty());
  plAudioEngineUi::TextButton(m_Writer, plRectFloat(details.x + 12.0f + fOwner + 6.0f, fButtonY, plAudioEngineUi::TextButtonWidth("Open event", false), ControlHeight), "Open event");

  const auto reason = static_cast<plAudioStopReason::Enum>(voice.m_uiStopReason);

  plStringBuilder values[PL_ARRAY_SIZE(s_szDetailNames)];
  values[0] = OwnerText(voice);
  if (voice.m_uiEntityId != 0)
    values[1].SetFormat("#{}", voice.m_uiEntityId);
  else
    values[1] = "-";
  values[2] = bEnded ? plString("ended") : StateText(voice);
  values[3] = reason != plAudioStopReason::None ? plStringView(plAudioStopReason::GetName(reason)) : plStringView(bEnded ? "not recorded" : "-");
  values[4].SetFormat("{}", voice.m_uiPriority);
  values[5].SetFormat("{}", plArgF(voice.m_fAudibility, 3));
  values[6].SetFormat("{} \xC2\xB7 {}", VolumeText(voice.m_fVolume), plArgF(voice.m_fVolume, 3));
  if (voice.m_uiEntityId != 0)
    values[7].SetFormat("{} m", plArgF(voice.m_fDistance, 2));
  else
    values[7] = "-";
  values[8].SetFormat("{}", plArgF(voice.m_fOcclusion, 2));
  values[9] = voice.m_sBus;
  values[10] = CodecText(voice);
  values[11].SetFormat("{} s", plArgF(voice.m_fAge, 2));

  float y = fButtonY + ControlHeight + 6.0f;
  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_szDetailNames); ++i)
  {
    const plRectFloat line(details.x + 10.0f, y, details.width - 20.0f, 18.0f);
    m_Writer.Text(line, s_szDetailNames[i], Dim(), FontSize - 1);

    plColorGammaUB value = Muted();
    if (i == 2)
      value = bEnded ? Dim() : plAudioEngineUi::Mix(Foreground(), StateColor(voice.m_uiState), 0.82f);
    else if (i == 3 && plAudioStopReason::IsForced(reason))
      value = Danger();

    m_Writer.Text(line, values[i], value, FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
    y += 20.0f;
  }

  // the instance's parameter values, as bars across the parameter's last known range
  y += 4.0f;
  plStringBuilder sCount;
  sCount.SetFormat("{}", voice.m_Parameters.GetCount());
  plAudioEngineUi::SectionHeader(m_Writer, plRectFloat(details.x + 1.0f, y, details.width - 1.0f, plAudioEngineUi::SectionHeight), "Parameters", plAudioEngineUi::AudioColor(), sCount);
  y += plAudioEngineUi::SectionHeight + 4.0f;

  if (voice.m_Parameters.IsEmpty())
  {
    m_Writer.Text(plRectFloat(details.x + 10.0f, y, details.width - 20.0f, 18.0f), "The event has no parameters.", Dim(), FontSize - 2);
    return;
  }

  for (const plAudioVoiceParameterSnapshotEntry& parameter : voice.m_Parameters)
  {
    const plRectFloat line(details.x + 10.0f, y, details.width - 20.0f, 18.0f);
    m_Writer.Text(line, parameter.m_sName, Dim(), FontSize - 1);

    plStringBuilder sValue;
    sValue.SetFormat("{}", plArgF(parameter.m_fValue, 2));
    m_Writer.Text(line, sValue, Muted(), FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
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

    const plAudioProfilerLogLine& line = GetShownLog()[m_VisibleLog[r]];
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
    m_Writer.Text(plRectFloat(rows.x, rows.y + 8.0f, rows.width, 20.0f), GetShownLog().IsEmpty() ? (HasRange() ? "No log entries in the range" : "No log entries") : "No entries match the filter", Dim(), FontSize - 1, plUiTextHAlign::Center);
}

void plEditorUiAudioProfilerView::DrawStatus()
{
  using namespace plEditorUi;

  const plVec2 size = GetSurface()->GetLogicalSize();
  const plRectFloat status(0.0f, size.y - s_fStatus, size.x, s_fStatus);
  m_Writer.Rect(status, Background());
  m_Writer.Rect(plRectFloat(0.0f, status.y, status.width, 1.0f), Line());

  // "Real 5/32" ... with the values in the code face; GetToolTipAt says what each one measures
  plStringBuilder values[7];
  float xs[8] = {};
  const plUInt32 uiShown = LayoutStatus(m_Snapshot, m_bHasSnapshot, size.x, values, xs);

  for (plUInt32 i = 0; i < uiShown; ++i)
  {
    const float fLabel = MeasureText(s_szStatusLabels[i], FontSize - 1) + 5.0f;
    m_Writer.Text(plRectFloat(xs[i], status.y + 1.0f, fLabel, status.height - 1.0f), s_szStatusLabels[i], Dim(), FontSize - 1);
    m_Writer.Text(plRectFloat(xs[i] + fLabel, status.y + 1.0f, xs[i + 1] - xs[i] - fLabel, status.height - 1.0f), values[i], Muted(), FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
  }

  float x = xs[uiShown];

  // the capture at the right end, or a note that just happened
  plStringBuilder sCapture;
  plColorGammaUB captureColor = Dim();
  if (!m_sToast.IsEmpty() && plTime::Now() - m_ToastTime < plTime::MakeFromSeconds(5.0))
  {
    sCapture = m_sToast;
    captureColor = Muted();
  }
  else if (m_pCapture != nullptr && !m_pCapture->IsEmpty())
  {
    plUInt32 uiUnderruns = 0;
    for (const plAudioProfilerCapture::Sample& sample : m_pCapture->GetSamples())
      uiUnderruns += sample.m_uiUnderruns;

    sCapture.SetFormat("Capture {} s \xC2\xB7 {} samples \xC2\xB7 {} instances \xC2\xB7 {} underruns \xC2\xB7 {}", plArgF(m_pCapture->GetDuration(), 1), m_pCapture->GetSamples().GetCount(),
      m_pCapture->GetInstances().GetCount(), uiUnderruns, m_bRecording ? "recording" : (m_pCapture->IsFull() ? "full" : "stopped"));
  }

  if (!sCapture.IsEmpty())
  {
    const float fWidth = MeasureText(sCapture, FontSize - 2) + 4.0f;
    if (x + fWidth < size.x - 8.0f)
      m_Writer.Text(plRectFloat(size.x - 8.0f - fWidth, status.y + 1.0f, fWidth, status.height - 1.0f), sCapture, captureColor, FontSize - 2);
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
    case Tab::Instances:
      fContent = m_Lanes.GetCount() * s_fLaneRow + s_fTableHeader;
      break;
    case Tab::Voices:
      // the cap banner takes room from the rows
      fContent = m_VoiceRows.GetCount() * s_fRow + (GetVoiceRowsTop() - table.y - s_fTableHeader);
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

  // the capture's graphs: the pointer's time, and dragging out a range
  if (!IsGraphLive())
  {
    const plRectFloat graph = GetGraphRect();
    const bool bInGraph = plRectFloat(graph.x - 4.0f, graph.y - 4.0f, graph.width + 8.0f, graph.height + 20.0f).Contains(e.m_vPosition);

    if (e.m_Type == Type::Move || e.m_Type == Type::DragMove)
    {
      const double fHover = (bInGraph || m_bDraggingRange) ? GraphTimeAt(e.m_vPosition.x) : -1.0;
      if (m_bDraggingRange)
        m_fRangeB = fHover;

      if (fHover != m_fHoverTime || m_bDraggingRange)
      {
        m_fHoverTime = fHover;
        m_bDrawDirty = true;
      }
    }

    if (e.m_Type == Type::Press && e.m_Button == plEditorUiPointerEvent::Button::Left && bInGraph && e.m_sElement.IsEmpty())
    {
      m_bDraggingRange = true;
      m_fRangeA = m_fRangeB = GraphTimeAt(e.m_vPosition.x);
      m_bDrawDirty = true;
      return;
    }

    if (m_bDraggingRange && (e.m_Type == Type::Release || e.m_Type == Type::Drop || e.m_Type == Type::DragCancel))
    {
      // a click without a drag clears the range
      m_bDraggingRange = false;
      if (!HasRange())
        m_fRangeA = m_fRangeB = -1.0;

      m_iSelectedInstance = -1;
      m_bRebuild = true;
      ApplyRange();
      return;
    }
  }

  // the Instances tab: a click on a lane selects the instance under it
  if (m_Tab == Tab::Instances && e.m_Type == Type::Press && e.m_Button == plEditorUiPointerEvent::Button::Left && e.m_sElement.IsEmpty() && m_pCapture != nullptr)
  {
    const float fRowsTop = table.y + s_fTableHeader;
    if (table.Contains(e.m_vPosition) && e.m_vPosition.y >= fRowsTop)
    {
      double fStart = 0.0, fEnd = 0.0;
      GetRange(fStart, fEnd);
      fEnd = plMath::Max(fEnd, fStart + 0.1);

      const float fLeft = table.x + s_fGraphHead;
      const float fRight = table.x + table.width - 8.0f;
      const double fSlop = 3.0 / plMath::Max(fRight - fLeft, 1.0f) * (fEnd - fStart);
      const double fTime = fStart + (e.m_vPosition.x - fLeft) / plMath::Max(fRight - fLeft, 1.0f) * (fEnd - fStart);

      plInt32 iHit = -1;
      const plInt32 iLane = static_cast<plInt32>((e.m_vPosition.y - fRowsTop + m_fScroll) / s_fLaneRow);
      if (iLane >= 0 && iLane < static_cast<plInt32>(m_Lanes.GetCount()) && e.m_vPosition.x >= fLeft)
      {
        for (plUInt32 uiInstance : m_Lanes[iLane].m_Instances)
        {
          const plAudioProfilerCapture::Instance& instance = m_pCapture->GetInstances()[uiInstance];
          if (fTime >= instance.m_fStart - fSlop && fTime <= instance.m_fEnd + fSlop)
            iHit = static_cast<plInt32>(uiInstance);
        }
      }

      if (iHit != m_iSelectedInstance)
      {
        m_iSelectedInstance = iHit;
        m_bRebuild = true;
        m_bDrawDirty = true;
      }
      return;
    }
  }

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

  const float fRowsTop = GetVoiceRowsTop();
  if (!table.Contains(e.m_vPosition) || e.m_vPosition.y < fRowsTop)
    return;

  const plInt32 iRow = static_cast<plInt32>((e.m_vPosition.y - fRowsTop + m_fScroll) / s_fRow);
  if (iRow < 0 || iRow >= static_cast<plInt32>(m_VoiceRows.GetCount()))
    return;

  // the details' Select owner and Open event come with the first selection
  m_bRebuild |= !m_bHasSelection;

  m_uiSelectedId = m_VoiceRows[iRow]->m_uiId;
  m_SelectedVoice = *m_VoiceRows[iRow];
  m_bHasSelection = true;
  m_bSelectedVoiceEnded = m_SelectedVoice.m_uiState == plAudioVoiceSnapshotEntry::Ended;
  m_bDrawDirty = true;
}

plString plEditorUiAudioProfilerView::GetToolTipAt(const plVec2& vLogical) const
{
  const plString sElementTip = plEditorUiContent::GetToolTipAt(vLogical);
  if (!sElementTip.IsEmpty())
    return sElementTip;

  const plRectFloat table = GetTableRect();
  const plRectFloat tool = GetToolRowRect();

  // the status row: what each value measures, and where
  const plVec2 size = GetSurface()->GetLogicalSize();
  if (vLogical.y >= size.y - s_fStatus)
  {
    plStringBuilder values[7];
    float xs[8] = {};
    const plUInt32 uiShown = LayoutStatus(m_Snapshot, m_bHasSnapshot, size.x, values, xs);
    for (plUInt32 i = 0; i < uiShown; ++i)
    {
      if (vLogical.x >= xs[i] && vLogical.x < xs[i + 1])
        return s_szStatusTips[i];
    }
    return {};
  }

  if (m_Tab == Tab::Voices && GetVoiceSummaryRect().Contains(vLogical))
  {
    for (plUInt32 i = 0; i < s_uiSummaryCards; ++i)
    {
      if (GetVoiceCardRect(i).Contains(vLogical))
        return s_szCardTips[i];
    }
    return {};
  }

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

  const float fRowsTop = m_Tab == Tab::Voices ? GetVoiceRowsTop() : table.y + s_fTableHeader;
  if (!table.Contains(vLogical) || vLogical.y < fRowsTop)
    return {};

  const plInt32 iRow = static_cast<plInt32>((vLogical.y - fRowsTop + m_fScroll) / s_fRow);
  if (iRow < 0)
    return {};

  if (m_Tab == Tab::Voices && iRow < static_cast<plInt32>(m_VoiceRows.GetCount()))
  {
    const plAudioVoiceSnapshotEntry& voice = *m_VoiceRows[iRow];
    plStringBuilder sTip;
    sTip.SetFormat("{}\nOwner {}\nAudibility {} \xC2\xB7 volume {}", voice.m_sEvent, OwnerText(voice), plArgF(voice.m_fAudibility, 3), VolumeText(voice.m_fVolume));

    if (voice.m_uiStopReason != plAudioStopReason::None)
      sTip.AppendFormat("\nStop reason: {}", plAudioStopReason::GetName(static_cast<plAudioStopReason::Enum>(voice.m_uiStopReason)));

    return sTip;
  }

  if (m_Tab == Tab::Memory && iRow < static_cast<plInt32>(m_MemoryRows.GetCount()))
    return m_MemoryRows[iRow].m_sToolTip;

  if (m_Tab == Tab::Log && iRow < static_cast<plInt32>(m_VisibleLog.GetCount()))
  {
    const plAudioProfilerLogLine& line = GetShownLog()[m_VisibleLog[iRow]];
    if (line.m_uiType == plAudioProfilerLogLine::GapType)
      return {};

    plStringBuilder sTip = line.m_sName;
    if (!line.m_sDetail.IsEmpty())
      sTip.Append("\n", line.m_sDetail);
    return sTip;
  }

  return {};
}

//////////////////////////////////////////////////////////////////////////
// Capture: graphs, instance lanes and the instance card

namespace
{
  const plColorGammaUB s_StreamsColor(100, 190, 185);
  const plColorGammaUB s_MemoryColor(185, 150, 205);

  /// A step for the time axis that gives labels about 80 px apart.
  double AxisStep(double fDuration, float fWidth)
  {
    const double steps[] = {0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0};
    for (double fStep : steps)
    {
      if (fDuration / fStep * 80.0 <= fWidth)
        return fStep;
    }
    return 600.0;
  }
} // namespace

plRectFloat plEditorUiAudioProfilerView::GetGraphRect() const
{
  const plVec2 size = IsReady() ? GetSurface()->GetLogicalSize() : plVec2(800.0f, 400.0f);
  return plRectFloat(s_fGraphHead, s_fHeaderRow + 4.0f, plMath::Max(size.x - s_fGraphHead - 8.0f, 40.0f), GetGraphHeight() - 22.0f);
}

float plEditorUiAudioProfilerView::GetGraphHeight() const
{
  const float fHeight = IsReady() ? GetSurface()->GetLogicalSize().y : 600.0f;
  return plMath::Clamp(plMath::Round(fHeight * 0.4f), s_fMinGraphHeight, s_fGraphHeight);
}

double plEditorUiAudioProfilerView::GraphTimeAt(float x) const
{
  const plRectFloat graph = GetGraphRect();
  double fStart = 0.0, fEnd = 0.0;
  GetGraphSpan(fStart, fEnd);
  return fStart + plMath::Clamp(static_cast<double>(x - graph.x) / graph.width, 0.0, 1.0) * (fEnd - fStart);
}

float plEditorUiAudioProfilerView::GraphXAt(double fTime) const
{
  const plRectFloat graph = GetGraphRect();
  double fStart = 0.0, fEnd = 0.0;
  GetGraphSpan(fStart, fEnd);
  return graph.x + static_cast<float>((fTime - fStart) / plMath::Max(fEnd - fStart, 0.001)) * graph.width;
}

void plEditorUiAudioProfilerView::DrawGraphs()
{
  using namespace plEditorUi;

  const plVec2 size = GetSurface()->GetLogicalSize();
  const plRectFloat graph = GetGraphRect();
  m_Writer.Rect(plRectFloat(0.0f, s_fHeaderRow, size.x, GetGraphHeight()), Background());

  const bool bLive = IsGraphLive();
  const plDynamicArray<plAudioProfilerCapture::Sample>* pSamples = &GetGraphSamples();
  const plUInt32 uiCount = pSamples->GetCount();

  // the scale of each row: the capture's largest value with some headroom
  float fMaxVoices = 8.0f, fMaxStreams = 4.0f, fMaxMemory = 1.0f;
  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    const plAudioProfilerCapture::Sample& s = (*pSamples)[i];
    fMaxVoices = plMath::Max(fMaxVoices, static_cast<float>(s.m_uiReal + s.m_uiVirtual));
    fMaxStreams = plMath::Max(fMaxStreams, static_cast<float>(s.m_uiStreams));
    fMaxMemory = plMath::Max(fMaxMemory, static_cast<float>(plMath::Max(s.m_uiMemoryBytes, s.m_uiBudgetBytes) / (1024.0 * 1024.0)));
  }

  const char* szRows[4] = {"Voices", "Mix CPU", "Streams", "Audio data"};
  const float rowMax[4] = {fMaxVoices * 1.1f, 100.0f, fMaxStreams * 1.25f, fMaxMemory * 1.1f};
  const float fRowHeight = graph.height / 4.0f;

  for (plUInt32 r = 0; r < 4; ++r)
  {
    const float y0 = graph.y + r * fRowHeight;
    const plRectFloat plot(graph.x, y0, graph.width, fRowHeight - 5.0f);

    m_Writer.Rect(plRectFloat(6.0f, y0, s_fGraphHead - 12.0f, fRowHeight - 5.0f), Surface(), 3.0f);
    m_Writer.Rect(plot, Field(), 3.0f);

    // the range under the series (the drawing has no blending, so it is a tint of the well)
    if (!bLive && HasRange() && uiCount > 0)
    {
      double fStart = 0.0, fEnd = 0.0;
      GetRange(fStart, fEnd);
      m_Writer.Rect(plRectFloat(GraphXAt(fStart), plot.y, GraphXAt(fEnd) - GraphXAt(fStart), plot.height), plAudioEngineUi::Mix(Field(), Accent(), 0.16f));
    }
    m_Writer.Text(plRectFloat(14.0f, y0 + 2.0f, s_fGraphHead - 24.0f, 14.0f), szRows[r], Foreground(), FontSize - 2);

    if (uiCount == 0)
      continue;

    // the last values under the title
    const plAudioProfilerCapture::Sample& last = pSamples->PeekBack();
    plStringBuilder sLast;
    switch (r)
    {
      case 0:
        sLast.SetFormat("{} real \xC2\xB7 {} virtual", last.m_uiReal, last.m_uiVirtual);
        break;
      case 1:
        sLast.SetFormat("{}% of the mix period", plArgF(last.m_fDspLoad * 100.0f, 1));
        break;
      case 2:
        sLast.SetFormat("{} active", last.m_uiStreams);
        break;
      default:
        if (last.m_uiBudgetBytes > 0)
          sLast.SetFormat("{} of {} MB", plArgF(last.m_uiMemoryBytes / (1024.0 * 1024.0), 1), plArgF(last.m_uiBudgetBytes / (1024.0 * 1024.0), 1));
        else
          sLast.SetFormat("{} MB", plArgF(last.m_uiMemoryBytes / (1024.0 * 1024.0), 1));
        break;
    }
    m_Writer.Text(plRectFloat(14.0f, y0 + 17.0f, s_fGraphHead - 24.0f, 12.0f), sLast, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);

    // a series: the largest value of each pixel column
    auto Series = [&](auto value, const plColorGammaUB& color)
    {
      plHybridArray<plVec2, 512> line;
      plUInt32 uiSample = 0;
      for (float x = plot.x; x <= plot.x + plot.width && uiSample < uiCount; x += 1.0f)
      {
        const double fColumnEnd = GraphTimeAt(x + 1.0f);
        float fValue = -1.0f;
        while (uiSample < uiCount && (*pSamples)[uiSample].m_fTime <= fColumnEnd)
        {
          fValue = plMath::Max(fValue, value((*pSamples)[uiSample]));
          ++uiSample;
        }

        if (fValue >= 0.0f)
          line.PushBack(plVec2(x, plot.y + plot.height - 2.0f - plMath::Clamp(fValue / rowMax[r], 0.0f, 1.0f) * (plot.height - 4.0f)));
      }

      if (line.GetCount() > 1)
        m_Writer.Polyline(line, color, 1.3f);
    };

    m_Writer.PushClip(plot);
    switch (r)
    {
      case 0:
        Series([](const plAudioProfilerCapture::Sample& s)
          { return static_cast<float>(s.m_uiVirtual); },
          Dim());
        Series([](const plAudioProfilerCapture::Sample& s)
          { return static_cast<float>(s.m_uiReal); },
          plAudioEngineUi::AudioColor());
        break;

      case 1:
        Series([](const plAudioProfilerCapture::Sample& s)
          { return s.m_fDspLoad * 100.0f; },
          Accent());
        break;

      case 2:
        Series([](const plAudioProfilerCapture::Sample& s)
          { return static_cast<float>(s.m_uiStreams); },
          s_StreamsColor);

        // a red tick where streams ran dry
        for (const plAudioProfilerCapture::Sample& s : *pSamples)
        {
          if (s.m_uiUnderruns > 0)
            m_Writer.Rect(plRectFloat(GraphXAt(s.m_fTime) - 1.0f, plot.y, 2.0f, plot.height), Danger());
        }
        break;

      default:
        Series([](const plAudioProfilerCapture::Sample& s)
          { return static_cast<float>(s.m_uiMemoryBytes / (1024.0 * 1024.0)); },
          s_MemoryColor);

        // the budget the loaded banks carry
        if (last.m_uiBudgetBytes > 0)
        {
          const float y = plot.y + plot.height - 2.0f - plMath::Clamp(static_cast<float>(last.m_uiBudgetBytes / (1024.0 * 1024.0)) / rowMax[r], 0.0f, 1.0f) * (plot.height - 4.0f);
          m_Writer.Line(plVec2(plot.x, y), plVec2(plot.x + plot.width, y), Foreground(), 1.0f, 4.0f, 3.0f);
        }
        break;
    }
    m_Writer.PopClip();
  }

  if (uiCount == 0)
  {
    m_Writer.Text(graph, "Waiting for snapshots from the target: it sends them while a scene simulates or plays", Dim(), FontSize - 1, plUiTextHAlign::Center);
    return;
  }

  // the time axis: seconds since the capture's start, or seconds before the newest live sample
  double fStart = 0.0, fEnd = 0.0;
  GetGraphSpan(fStart, fEnd);
  const double fDuration = fEnd - fStart;
  const double fStep = AxisStep(fDuration, graph.width);
  for (double fOffset = 0.0; fOffset <= fDuration + 0.001; fOffset += fStep)
  {
    const float x = GraphXAt(fStart + fOffset);
    m_Writer.Rect(plRectFloat(x, graph.y, 1.0f, graph.height), Line());

    plStringBuilder sTime;
    if (!bLive)
      sTime.SetFormat("{}s", plArgF(fOffset, fStep < 1.0 ? 1 : 0));
    else if (fDuration - fOffset < 0.001)
      sTime = "now";
    else
      sTime.SetFormat("-{}s", plArgF(fDuration - fOffset, fStep < 1.0 ? 1 : 0));

    m_Writer.Text(plRectFloat(plMath::Min(x + 2.0f, graph.x + graph.width - 30.0f), graph.y + graph.height + 2.0f, 40.0f, 14.0f), sTime, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
  }

  if (bLive)
    return;

  // the range and the pointer's time
  if (HasRange())
  {
    double fStart = 0.0, fEnd = 0.0;
    GetRange(fStart, fEnd);
    const float x0 = GraphXAt(fStart);
    const float x1 = GraphXAt(fEnd);
    m_Writer.Rect(plRectFloat(x0, graph.y - 4.0f, 1.0f, graph.height + 4.0f), Accent());
    m_Writer.Rect(plRectFloat(x1, graph.y - 4.0f, 1.0f, graph.height + 4.0f), Accent());
  }

  if (m_fHoverTime >= 0.0)
    m_Writer.Rect(plRectFloat(GraphXAt(m_fHoverTime), graph.y - 4.0f, 1.0f, graph.height + 4.0f), Muted());
}

void plEditorUiAudioProfilerView::DrawInstances()
{
  using namespace plEditorUi;

  const plRectFloat table = GetTableRect();
  m_Writer.Rect(table, Surface());

  double fStart = 0.0, fEnd = 0.0;
  GetRange(fStart, fEnd);
  fEnd = plMath::Max(fEnd, fStart + 0.1);

  const float fLeft = table.x + s_fGraphHead;
  const float fRight = table.x + table.width - 8.0f;
  auto X = [&](double t)
  { return fLeft + static_cast<float>((t - fStart) / (fEnd - fStart)) * (fRight - fLeft); };

  // the header: how many instances the range has, and the legend
  plUInt32 uiInstances = 0;
  for (const Lane& lane : m_Lanes)
    uiInstances += lane.m_Instances.GetCount();

  plStringBuilder sHeader;
  sHeader.SetFormat("{} instances in {} - {} s", uiInstances, plArgF(fStart, 1), plArgF(fEnd, 1));
  m_Writer.Rect(plRectFloat(table.x, table.y + s_fTableHeader - 1.0f, table.width, 1.0f), Line());
  m_Writer.Text(plRectFloat(table.x + 10.0f, table.y, s_fGraphHead - 14.0f, s_fTableHeader), "EVENT", Dim(), MinFontSize);
  m_Writer.Text(plRectFloat(fLeft, table.y, 260.0f, s_fTableHeader), sHeader, Muted(), MinFontSize + 1, plUiTextHAlign::Left, plUiTextVAlign::Center, true);

  {
    float x = fRight - 230.0f;
    const char* szNames[] = {"real", "virtual", "stopping"};
    for (plUInt32 i = 0; i < 3; ++i)
    {
      const plRectFloat swatch(x, table.y + s_fTableHeader * 0.5f - 4.0f, 8.0f, 8.0f);
      if (i == 1)
        m_Writer.RectOutline(swatch, Dim());
      else
        m_Writer.Rect(swatch, i == 0 ? plAudioEngineUi::AudioColor() : Warning());
      m_Writer.Text(plRectFloat(x + 12.0f, table.y, 60.0f, s_fTableHeader), szNames[i], Dim(), MinFontSize + 1);
      x += 76.0f;
    }
  }

  const plRectFloat rows(table.x, table.y + s_fTableHeader, table.width, plMath::Max(table.height - s_fTableHeader, 1.0f));
  if (m_Lanes.IsEmpty())
  {
    m_Writer.Text(plRectFloat(rows.x, rows.y + 8.0f, rows.width, 20.0f), (m_pCapture == nullptr || m_pCapture->IsEmpty()) ? "No capture yet" : "No instances in the range", Dim(), FontSize - 1, plUiTextHAlign::Center);
  }

  m_Writer.PushClip(rows);
  for (plUInt32 j = 0; j < m_Lanes.GetCount(); ++j)
  {
    const float y = rows.y + j * s_fLaneRow - m_fScroll;
    if (y + s_fLaneRow < rows.y || y > rows.y + rows.height)
      continue;

    const Lane& lane = m_Lanes[j];
    if (j % 2 == 1)
      m_Writer.Rect(plRectFloat(rows.x, y, rows.width, s_fLaneRow), plAudioEngineUi::Mix(Surface(), Background(), 0.4f));

    plStringBuilder sName;
    sName.SetFormat("{} \xC3\x97{}", ShortName(lane.m_sEvent), lane.m_Instances.GetCount());
    m_Writer.Rect(plRectFloat(rows.x + 10.0f, y + 6.0f, 6.0f, 6.0f), plAudioEngineUi::AudioColor());
    m_Writer.Text(plRectFloat(rows.x + 22.0f, y, s_fGraphHead - 26.0f, s_fLaneRow), sName, Foreground(), FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);

    m_Writer.PushClip(plRectFloat(fLeft, y, fRight - fLeft, s_fLaneRow));
    for (plUInt32 uiInstance : lane.m_Instances)
    {
      const plAudioProfilerCapture::Instance& instance = m_pCapture->GetInstances()[uiInstance];
      const bool bSelected = static_cast<plInt32>(uiInstance) == m_iSelectedInstance;

      for (const plAudioProfilerCapture::Segment& segment : instance.m_Segments)
      {
        const float x0 = X(segment.m_fStart);
        const float x1 = plMath::Max(X(segment.m_fEnd), x0 + 1.5f);
        const plRectFloat bar(x0, y + 4.0f, x1 - x0, s_fLaneRow - 8.0f);

        switch (segment.m_uiState)
        {
          case plAudioVoiceSnapshotEntry::Real:
            m_Writer.Rect(bar, bSelected ? plAudioEngineUi::Mix(plAudioEngineUi::AudioColor(), plColorGammaUB(255, 255, 255), 0.45f) : plAudioEngineUi::AudioColor());
            break;
          case plAudioVoiceSnapshotEntry::Virtual:
            m_Writer.RectOutline(bar, bSelected ? Foreground() : Dim());
            break;
          default:
            m_Writer.Rect(bar, Warning());
            break;
        }
      }
    }
    m_Writer.PopClip();
  }
  m_Writer.PopClip();

  DrawInstanceCard(GetDetailsRect());
}

void plEditorUiAudioProfilerView::DrawInstanceCard(const plRectFloat& rect)
{
  using namespace plEditorUi;

  m_Writer.Rect(rect, Surface());
  m_Writer.Rect(plRectFloat(rect.x, rect.y, 1.0f, rect.height), Line());

  auto KeyValues = [&](float y, plArrayPtr<const plString> keys, plArrayPtr<const plString> values)
  {
    for (plUInt32 i = 0; i < keys.GetCount(); ++i)
    {
      m_Writer.Text(plRectFloat(rect.x + 12.0f, y, 90.0f, 20.0f), keys[i], Dim(), MinFontSize + 1, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
      m_Writer.Text(plRectFloat(rect.x + 104.0f, y, rect.width - 114.0f, 20.0f), values[i], Muted(), MinFontSize + 1, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
      y += 20.0f;
    }
  };

  if (m_pCapture == nullptr || m_iSelectedInstance < 0)
  {
    plAudioEngineUi::SectionHeader(m_Writer, plRectFloat(rect.x + 1.0f, rect.y, rect.width - 1.0f, plAudioEngineUi::SectionHeight), "Instance", plAudioEngineUi::AudioColor());
    m_Writer.Text(plRectFloat(rect.x + 12.0f, rect.y + 32.0f, rect.width - 20.0f, 18.0f), "Click a lane to inspect an instance.", Dim(), FontSize - 2);

    // the peaks of the range
    plAudioEngineUi::SectionHeader(m_Writer, plRectFloat(rect.x + 1.0f, rect.y + 62.0f, rect.width - 1.0f, plAudioEngineUi::SectionHeight), "Range summary", plAudioEngineUi::AudioColor());

    plUInt32 uiPeakReal = 0, uiPeakVirtual = 0, uiMaxReal = 0, uiMaxVirtual = 0, uiUnderruns = 0;
    float fPeakDsp = 0.0f;
    if (m_pCapture != nullptr)
    {
      double fStart = 0.0, fEnd = 0.0;
      GetRange(fStart, fEnd);
      for (const plAudioProfilerCapture::Sample& s : m_pCapture->GetSamples())
      {
        if (s.m_fTime < fStart || s.m_fTime > fEnd)
          continue;

        uiPeakReal = plMath::Max(uiPeakReal, s.m_uiReal);
        uiPeakVirtual = plMath::Max(uiPeakVirtual, s.m_uiVirtual);
        uiMaxReal = s.m_uiMaxReal;
        uiMaxVirtual = s.m_uiMaxVirtual;
        fPeakDsp = plMath::Max(fPeakDsp, s.m_fDspLoad);
        uiUnderruns += s.m_uiUnderruns;
      }
    }

    plStringBuilder values[4];
    values[0].SetFormat("{} / {}", uiPeakReal, uiMaxReal);
    values[1].SetFormat("{} / {}", uiPeakVirtual, uiMaxVirtual);
    values[2].SetFormat("{}%", plArgF(fPeakDsp * 100.0f, 1));
    values[3].SetFormat("{}", uiUnderruns);

    const plString keys[] = {"Peak real", "Peak virtual", "Peak DSP", "Underruns"};
    const plString shown[] = {values[0], values[1], values[2], uiUnderruns > 0 ? plString() : plString(values[3])};
    KeyValues(rect.y + 94.0f, keys, shown);

    // underruns in the error colour
    if (uiUnderruns > 0)
      m_Writer.Text(plRectFloat(rect.x + 104.0f, rect.y + 154.0f, 60.0f, 20.0f), values[3], Danger(), MinFontSize + 1, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    return;
  }

  const plAudioProfilerCapture::Instance& instance = m_pCapture->GetInstances()[m_iSelectedInstance];
  plAudioEngineUi::SectionHeader(m_Writer, plRectFloat(rect.x + 1.0f, rect.y, rect.width - 1.0f, plAudioEngineUi::SectionHeight), ShortName(instance.m_sEvent), plAudioEngineUi::AudioColor());

  plAudioVoiceSnapshotEntry codec;
  codec.m_uiCodec = instance.m_uiCodec;
  codec.m_uiLoadMode = instance.m_uiLoadMode;

  plStringBuilder sStart, sEnd, sVirtual, sAudibility, sSerial;
  sStart.SetFormat("{} s", plArgF(instance.m_fStart, 2));
  sEnd.SetFormat("{} s", plArgF(instance.m_fEnd, 2));
  sVirtual.SetFormat("{}\xC3\x97", instance.m_uiVirtualisations);
  sAudibility.SetFormat("{}", plArgF(instance.m_fPeakAudibility, 3));
  sSerial.SetFormat("#{}", instance.m_uiSerial);

  const plString keys[] = {"Entity", "Bus", "Codec", "Start", "End", "Virtualised", "Peak aud.", "Instance"};
  const plString values[] = {instance.m_sEntity.IsEmpty() ? plString("(global)") : instance.m_sEntity, instance.m_sBus.IsEmpty() ? plString("-") : instance.m_sBus,
    CodecText(codec), sStart, sEnd, sVirtual, sAudibility, sSerial};
  // Select entity and Open event first, so a short panel shows them (the document's hit buttons sit on them)
  KeyValues(rect.y + plAudioEngineUi::SectionHeight + 6.0f + plEditorUi::ControlHeight + 6.0f, keys, values);

  const float fY = rect.y + plAudioEngineUi::SectionHeight + 6.0f;
  const float fEntity = plAudioEngineUi::TextButtonWidth("Select entity", false);
  plAudioEngineUi::TextButton(m_Writer, plRectFloat(rect.x + 12.0f, fY, fEntity, ControlHeight), "Select entity", {}, !instance.m_sEntity.IsEmpty());
  plAudioEngineUi::TextButton(m_Writer, plRectFloat(rect.x + 12.0f + fEntity + 6.0f, fY, plAudioEngineUi::TextButtonWidth("Open event", false), ControlHeight), "Open event");
}

void plEditorUiAudioProfilerView::ShowTargetMenu()
{
  plEditorUiMenu menu;
  const bool bPlayer = m_sTargetText.StartsWith("Player");

  plEditorUiMenuItem& engine = menu.AddAction("Editor engine process", 0);
  engine.m_bCheckable = true;
  engine.m_bChecked = !bPlayer;

  plEditorUiMenuItem& player = menu.AddAction("Player on this computer (telemetry)", 1);
  player.m_bCheckable = true;
  player.m_bChecked = bPlayer;

  const float x = GetPauseButtonX() + plAudioEngineUi::TextButtonWidth("Resume view", false) + 14.0f;
  plEditorUiMenuView::Show(*GetSurface(), plVec2(x, 3.0f + plEditorUi::ControlHeight), menu, [this](plUInt32 uiId)
    {
      if (m_OnTargetChosen.IsValid())
        m_OnTargetChosen(uiId); });
}
