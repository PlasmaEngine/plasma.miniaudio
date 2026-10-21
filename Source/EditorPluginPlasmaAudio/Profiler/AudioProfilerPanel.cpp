#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/Profiler/AudioProfilerPanel.moc.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Types/VariantType.h>
#include <GuiFoundation/EngineUi/ApplicationPanels.h>
#include <GuiFoundation/EngineUi/EngineUiContentWidget.h>
#include <GuiFoundation/UIServices/UIServices.moc.h>

#include <QTimer>

PL_IMPLEMENT_SINGLETON(plQtAudioProfilerPanel);

namespace
{
  constexpr int kPollIntervalMs = 250;
  constexpr plInt32 kSnapshotHz = 10;

  /// A request without an answer after this long no longer blocks the next one.
  const plTime kRequestTimeout = plTime::MakeFromSeconds(2.0);
} // namespace

plQtAudioProfilerPanel::plQtAudioProfilerPanel()
  : plQtApplicationPanel("AudioProfilerPanel")
  , m_SingletonRegistrar(this)
{
  setWindowTitle("Audio Profiler");
  setIcon(plQtUiServices::GetCachedIconResource(":/AssetIcons/AudioMixer.svg"));

  plQtEngineUiContentWidget* pViewWidget = nullptr;
  m_pView = plQtEngineUiContentWidget::Create<plEditorUiAudioProfilerView>(this, "AudioProfiler", &pViewWidget);
  m_pView->m_OnPauseToggled = [this](bool bPaused)
  { OnPauseToggled(bPaused); };
  setWidget(pViewWidget, ads::CDockWidget::ForceNoScrollArea);

  // the engine shell shows a view of its own, fed by this panel
  plEditorUiApplicationPanelDesc desc;
  desc.m_sId = ShellPanelId;
  desc.m_CreateContent = [](QWidget*) -> plUniquePtr<plEditorUiContent>
  {
    plQtAudioProfilerPanel* pPanel = plQtAudioProfilerPanel::GetSingleton();
    return pPanel != nullptr ? pPanel->CreateShellView() : plUniquePtr<plEditorUiContent>();
  };
  plEditorUiApplicationPanels::SetIcon(ShellPanelId, plAudioEngineUi::IconSliders);
  plEditorUiApplicationPanels::Register(desc);

  m_pTimer = new QTimer(this);
  m_pTimer->setInterval(kPollIntervalMs);
  connect(m_pTimer, &QTimer::timeout, this, &plQtAudioProfilerPanel::OnTick);
  m_pTimer->start();

  connect(this, &ads::CDockWidget::viewToggled, this, [this](bool) { OnTick(); });

  plEditorEngineProcessConnection::s_Events.AddEventHandler(plMakeDelegate(&plQtAudioProfilerPanel::EngineProcessMsgHandler, this));

  UpdateLinkIndicator();
}

plQtAudioProfilerPanel::~plQtAudioProfilerPanel()
{
  // the shell destroys its view now, while the panel still exists
  plEditorUiApplicationPanels::Unregister(ShellPanelId);

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

  // the engine process produces snapshots only while the panel is open, here or in the engine shell
  const bool bWantSnapshots = pConnection != nullptr && (!isClosed() || !m_ShellViews.IsEmpty());
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

  if (m_bPaused || !IsShown())
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

  if (newState == LinkState::Live)
  {
    plHybridArray<LogLine, 32> newLines;
    CollectNewLogLines(newLines);

    plHybridArray<plEditorUiAudioProfilerView*, 2> views;
    GatherViews(views);
    for (plEditorUiAudioProfilerView* pView : views)
      pView->SetSnapshot(m_Snapshot, newLines);
  }
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

void plQtAudioProfilerPanel::UpdateLinkIndicator()
{
  using ViewState = plEditorUiAudioProfilerView::LinkState;
  const ViewState state = m_LinkState == LinkState::Live ? ViewState::Live : (m_LinkState == LinkState::NoData ? ViewState::NoData : ViewState::NoEngine);

  plHybridArray<plEditorUiAudioProfilerView*, 2> views;
  GatherViews(views);
  for (plEditorUiAudioProfilerView* pView : views)
    pView->SetLinkState(state);
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

plUniquePtr<plEditorUiContent> plQtAudioProfilerPanel::CreateShellView()
{
  plEditorUiAudioProfilerView* pView = PL_DEFAULT_NEW(plEditorUiAudioProfilerView);
  pView->m_OnPauseToggled = [this](bool bPaused)
  { OnPauseToggled(bPaused); };
  pView->m_OnDestroyed = plMakeDelegate(&plQtAudioProfilerPanel::OnShellViewDestroyed, this);
  m_ShellViews.PushBack(pView);

  // the state so far; the next poll brings a snapshot
  UpdateLinkIndicator();
  if (m_bHasSnapshot)
    pView->SetSnapshot(m_Snapshot, {});

  return plUniquePtr<plEditorUiContent>(pView, plFoundation::GetDefaultAllocator());
}

void plQtAudioProfilerPanel::OnShellViewDestroyed(plEditorUiAudioProfilerView* pView)
{
  m_ShellViews.RemoveAndCopy(pView);
}

bool plQtAudioProfilerPanel::IsShown() const
{
  return (!isClosed() && isVisible()) || !m_ShellViews.IsEmpty();
}

void plQtAudioProfilerPanel::GatherViews(plHybridArray<plEditorUiAudioProfilerView*, 2>& out_views) const
{
  out_views.Clear();
  if (m_pView != nullptr)
    out_views.PushBack(m_pView);
  out_views.PushBackRange(m_ShellViews);
}
