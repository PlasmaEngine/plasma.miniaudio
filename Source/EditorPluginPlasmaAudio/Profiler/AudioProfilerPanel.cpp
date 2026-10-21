#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/EditorApp/EditorApp.moc.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/Profiler/AudioProfilerPanel.moc.h>
#include <EditorUi/FileDialog.h>
#include <Foundation/Communication/Telemetry.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Time/Timestamp.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.moc.h>
#include <GuiFoundation/EngineUi/ApplicationPanels.h>
#include <GuiFoundation/EngineUi/EngineUiContentWidget.h>
#include <GuiFoundation/UIServices/UIServices.moc.h>
#include <ToolsFoundation/Project/ToolsProject.h>

#include <QTimer>

PL_IMPLEMENT_SINGLETON(plQtAudioProfilerPanel);

namespace
{
  constexpr int kPollIntervalMs = 250;
  constexpr plInt32 kSnapshotHz = 10;

  /// A request without an answer after this long no longer blocks the next one.
  const plTime kRequestTimeout = plTime::MakeFromSeconds(2.0);

  /// How often an unanswered Player connection is tried again, and how long a connected Player may stay silent.
  const plTime kReconnectInterval = plTime::MakeFromSeconds(3.0);
  const plTime kPlayerSilence = plTime::MakeFromSeconds(2.0);

  constexpr const char* s_szSnapshotStat = "PlasmaAudio/VoiceSnapshot";
  constexpr const char* s_szSnapshotCVar = "PlasmaAudio.ProfilerSnapshotHz";
  constexpr const char* s_szCaptureFilter = "Audio captures (*.plAudioCapture)";

  /// The scene document the user works in: the visible scene window.
  plDocument* FindActiveSceneDocument()
  {
    for (plQtDocumentWindow* pWindow : plQtDocumentWindow::GetAllDocumentWindows())
    {
      plDocument* pDocument = pWindow->GetDocument();
      if (pDocument != nullptr && pWindow->isVisible() && pDocument->GetDocumentTypeName() == "Scene")
        return pDocument;
    }

    return nullptr;
  }

  /// Adds the objects of the subtree whose Name is sName.
  void FindObjectsByName(const plDocumentObject* pObject, plStringView sName, plDeque<const plDocumentObject*>& inout_objects)
  {
    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      const plAbstractProperty* pName = pChild->GetTypeAccessor().GetType()->FindPropertyByName("Name");
      if (pName != nullptr && pChild->GetTypeAccessor().GetValue("Name").ConvertTo<plString>() == sName)
        inout_objects.PushBack(pChild);

      FindObjectsByName(pChild, sName, inout_objects);
    }
  }
} // namespace

plQtAudioProfilerPanel::plQtAudioProfilerPanel()
  : plQtApplicationPanel(ShellPanelId) // the Panels menu opens the engine shell's panel of the same name
  , m_SingletonRegistrar(this)
{
  setWindowTitle("Audio Profiler");
  setIcon(plQtUiServices::GetCachedIconResource(":/AssetIcons/AudioMixer.svg"));

  plQtEngineUiContentWidget* pViewWidget = nullptr;
  m_pView = plQtEngineUiContentWidget::Create<plEditorUiAudioProfilerView>(this, "AudioProfiler", &pViewWidget);
  ConnectView(m_pView);
  setWidget(pViewWidget, ads::CDockWidget::ForceNoScrollArea);

  RegisterShellPanel();

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

  if (m_bTelemetryOpen)
  {
    plTelemetry::AcceptMessagesForSystem('STAT', false);
    plTelemetry::CloseConnection();
  }
}

void plQtAudioProfilerPanel::RegisterShellPanel()
{
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
}

void plQtAudioProfilerPanel::ToolsProjectEventHandler(const plToolsProjectEvent& e)
{
  // the shell translates the panel's title when it is registered; the package's strings load with the project
  if (e.m_Type == plToolsProjectEvent::Type::ProjectOpened)
    RegisterShellPanel();

  plQtApplicationPanel::ToolsProjectEventHandler(e);
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
  if (m_Target == Target::Player)
  {
    // the Player's Inspector plugin sets cvars it is sent
    if (!plTelemetry::IsConnectedToServer())
      return;

    plTelemetryMessage msg;
    msg.SetMessageID('SVAR', ' SET');
    msg.GetWriter() << plString(s_szSnapshotCVar);
    msg.GetWriter() << static_cast<plUInt8>(plCVarType::Int);
    msg.GetWriter() << iHz;
    plTelemetry::SendToServer(msg);
    return;
  }

  plEditorEngineProcessConnection* pConnection = GetLiveConnection();
  if (pConnection == nullptr)
    return;

  plChangeCVarMsgToEngine msg;
  msg.m_sCVarName = s_szSnapshotCVar;
  msg.m_NewValue = iHz;
  pConnection->SendMessage(&msg);
}

void plQtAudioProfilerPanel::OnTick()
{
  if (m_Target == Target::Player)
    TickPlayer();
  else
    TickEngineProcess();
}

void plQtAudioProfilerPanel::TickEngineProcess()
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
    SetLinkState(LinkState::NoEngine);
    return;
  }

  if (!IsShown())
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

void plQtAudioProfilerPanel::TickPlayer()
{
  const plTime tNow = plTime::Now();

  // connect, and try again while the Player does not answer
  if (!plTelemetry::IsConnectedToServer())
  {
    m_bSnapshotsEnabled = false;
    SetLinkState(LinkState::NoEngine);

    if (!m_bTelemetryOpen || tNow - m_LastConnectAttempt > kReconnectInterval)
    {
      m_LastConnectAttempt = tNow;
      m_bTelemetryOpen = plTelemetry::ConnectToServer(m_sPlayerAddress).Succeeded();
      plTelemetry::AcceptMessagesForSystem('STAT', true);
    }
    return;
  }

  if (!m_bSnapshotsEnabled)
  {
    SendSnapshotRate(kSnapshotHz);
    m_bSnapshotsEnabled = true;
    m_LastPlayerData = tNow;
  }

  // every stat change the Player sends; only the snapshot matters here
  plTelemetryMessage msg;
  while (plTelemetry::RetrieveMessage('STAT', msg).Succeeded())
  {
    if (msg.GetMessageID() != ' SET')
      continue;

    plString sName;
    plVariant value;
    msg.GetReader() >> sName;
    msg.GetReader() >> value;

    if (sName == s_szSnapshotStat && value.IsA<plDataBuffer>())
    {
      m_LastPlayerData = tNow;
      ApplySnapshotData(value.Get<plDataBuffer>());
    }
  }

  if (tNow - m_LastPlayerData > kPlayerSilence)
    SetLinkState(LinkState::NoData);
}

void plQtAudioProfilerPanel::EngineProcessMsgHandler(const plEditorEngineProcessConnection::Event& e)
{
  if (m_Target != Target::EngineProcess)
    return;

  switch (e.m_Type)
  {
    case plEditorEngineProcessConnection::Event::Type::ProcessMessage:
    {
      if (const auto* pMsg = plDynamicCast<const plStatsSnapshotResultMsgToEditor*>(e.m_pMsg))
      {
        m_bRequestPending = false;

        if (pMsg->m_Stats.IsValid())
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
      SetLinkState(LinkState::NoEngine);
    }
    break;

    default:
      break;
  }
}

void plQtAudioProfilerPanel::ApplyStats(const plVariant& stats)
{
  if (stats.IsA<plVariantDictionary>())
  {
    const plVariantDictionary& dict = stats.Get<plVariantDictionary>();

    const plVariant* pValue = nullptr;
    if (dict.TryGetValue(s_szSnapshotStat, pValue) && pValue->IsA<plDataBuffer>())
    {
      ApplySnapshotData(pValue->Get<plDataBuffer>());
      return;
    }
  }

  SetLinkState(LinkState::NoData);
}

void plQtAudioProfilerPanel::ApplySnapshotData(const plDataBuffer& data)
{
  {
    plRawMemoryStreamReader reader(data);
    if (m_Snapshot.Read(reader).Failed())
    {
      SetLinkState(LinkState::NoData);
      return;
    }
  }

  m_bHasSnapshot = true;
  SetLinkState(LinkState::Live);

  plHybridArray<LogLine, 32> newLines;
  CollectNewLogLines(newLines);

  plHybridArray<plEditorUiAudioProfilerView*, 2> views;
  GatherViews(views);
  for (plEditorUiAudioProfilerView* pView : views)
    pView->SetSnapshot(m_Snapshot, newLines);

  // the same snapshot can arrive twice (a poll before the next one was taken); a capture takes each one once
  const bool bRepeat = m_Snapshot.m_fTime > 0.0 && m_Snapshot.m_fTime == m_fLastCapturedTime;
  if (m_bRecording && !bRepeat)
  {
    m_fLastCapturedTime = m_Snapshot.m_fTime;

    if (m_Capture.AddSnapshot(plArrayPtr<const plUInt8>(data.GetData(), data.GetCount()), (plTime::Now() - m_CaptureStart).GetSeconds()).Failed() && m_Capture.IsFull())
    {
      SetRecording(false);
      ShowToast("The capture is full (10 minutes): save it, or press Record for a new one");
    }

    CaptureChanged();
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

void plQtAudioProfilerPanel::SetLinkState(LinkState state)
{
  if (state == m_LinkState)
    return;

  m_LinkState = state;
  UpdateLinkIndicator();
}

void plQtAudioProfilerPanel::UpdateLinkIndicator()
{
  using ViewState = plEditorUiAudioProfilerView::LinkState;
  const ViewState state = m_LinkState == LinkState::Live ? ViewState::Live : (m_LinkState == LinkState::NoData ? ViewState::NoData : ViewState::NoEngine);

  plStringBuilder sTarget = "Editor engine process";
  if (m_Target == Target::Player)
    sTarget.SetFormat("Player \xC2\xB7 {}", m_sPlayerAddress);

  plHybridArray<plEditorUiAudioProfilerView*, 2> views;
  GatherViews(views);
  for (plEditorUiAudioProfilerView* pView : views)
  {
    pView->SetLinkState(state);
    pView->SetTargetText(sTarget);
  }
}

//////////////////////////////////////////////////////////////////////////
// Capture

void plQtAudioProfilerPanel::SetTarget(Target target)
{
  if (target == m_Target)
    return;

  // the old target stops producing snapshots
  if (m_bSnapshotsEnabled)
    SendSnapshotRate(0);

  if (m_Target == Target::Player && m_bTelemetryOpen)
  {
    plTelemetry::AcceptMessagesForSystem('STAT', false);
    plTelemetry::CloseConnection();
    m_bTelemetryOpen = false;
  }

  m_Target = target;
  m_bSnapshotsEnabled = false;
  m_bRequestPending = false;
  m_bLogSequenceKnown = false;
  m_LinkState = LinkState::NoEngine;
  m_LastConnectAttempt = plTime::MakeZero();

  // another target is another recording
  if (m_bRecording)
  {
    m_Capture.Clear();
    m_bCaptureFromFile = false;
    m_CaptureStart = plTime::Now();
    CaptureChanged();
  }

  UpdateLinkIndicator();
  OnTick();
}

void plQtAudioProfilerPanel::SetRecording(bool bRecording)
{
  // a capture from a file or a full one is not continued
  if (bRecording && (m_bCaptureFromFile || m_Capture.IsFull()))
  {
    m_Capture.Clear();
    m_bCaptureFromFile = false;
    CaptureChanged();
  }

  if (bRecording && m_Capture.IsEmpty())
    m_CaptureStart = plTime::Now();

  m_bRecording = bRecording;

  plHybridArray<plEditorUiAudioProfilerView*, 2> views;
  GatherViews(views);
  for (plEditorUiAudioProfilerView* pView : views)
    pView->SetRecording(m_bRecording);
}

void plQtAudioProfilerPanel::CaptureChanged()
{
  plHybridArray<plEditorUiAudioProfilerView*, 2> views;
  GatherViews(views);
  for (plEditorUiAudioProfilerView* pView : views)
    pView->OnCaptureChanged();
}

void plQtAudioProfilerPanel::ShowToast(plStringView sText)
{
  plHybridArray<plEditorUiAudioProfilerView*, 2> views;
  GatherViews(views);
  for (plEditorUiAudioProfilerView* pView : views)
    pView->ShowToast(sText);
}

void plQtAudioProfilerPanel::SaveCapture()
{
  if (m_Capture.IsEmpty())
    return;

  // "<project>/Captures/Audio_2026-10-21_1412.plAudioCapture"
  const plDateTime now = plDateTime::MakeFromTimestamp(plTimestamp::CurrentTimestamp());
  plStringBuilder sStart = plToolsProject::GetSingleton()->GetProjectDirectory();
  sStart.AppendFormat("/Captures/Audio_{}-{}-{}_{}{}.plAudioCapture", now.GetYear(), plArgU(now.GetMonth(), 2, true), plArgU(now.GetDay(), 2, true), plArgU(now.GetHour(), 2, true), plArgU(now.GetMinute(), 2, true));

  plEditorUiFileDialog::Desc desc;
  desc.m_Mode = plEditorUiFileDialog::Mode::SaveFile;
  desc.m_sCaption = "Save Audio Capture";
  desc.m_sStart = sStart;
  desc.m_sFilter = s_szCaptureFilter;
  desc.m_sFallback = plToolsProject::GetSingleton()->GetProjectDirectory();

  plEditorUiFileDialog::Result result;
  if (!plEditorUiFileDialog::Run(nullptr, desc, result) || result.m_Paths.IsEmpty())
    return;

  plStringBuilder sPath = result.m_Paths[0];
  if (!sPath.HasExtension("plAudioCapture"))
    sPath.ChangeFileExtension("plAudioCapture");

  plDynamicArray<plUInt8> bytes;
  plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&bytes);
  plMemoryStreamWriter writer(&storage);

  plOSFile file;
  if (m_Capture.Save(writer).Failed() || plOSFile::CreateDirectoryStructure(sPath.GetFileDirectory()).Failed() || file.Open(sPath, plFileOpenMode::Write).Failed() ||
      file.Write(bytes.GetData(), bytes.GetCount()).Failed())
  {
    plQtUiServices::MessageBoxWarning(plFmt("Saving the audio capture to '{}' failed.", sPath));
    return;
  }

  file.Close();

  plStringBuilder sToast;
  sToast.SetFormat("Saved {} ({} KB)", sPath.GetFileNameAndExtension(), bytes.GetCount() / 1024);
  ShowToast(sToast);
}

void plQtAudioProfilerPanel::OpenCapture()
{
  plEditorUiFileDialog::Desc desc;
  desc.m_Mode = plEditorUiFileDialog::Mode::OpenFile;
  desc.m_sCaption = "Open Audio Capture";
  desc.m_sStart = plStringBuilder(plToolsProject::GetSingleton()->GetProjectDirectory(), "/Captures");
  desc.m_sFilter = s_szCaptureFilter;
  desc.m_sFallback = plToolsProject::GetSingleton()->GetProjectDirectory();

  plEditorUiFileDialog::Result result;
  if (!plEditorUiFileDialog::Run(nullptr, desc, result) || result.m_Paths.IsEmpty())
    return;

  plOSFile file;
  plDynamicArray<plUInt8> content;
  if (file.Open(result.m_Paths[0], plFileOpenMode::Read).Failed())
  {
    plQtUiServices::MessageBoxWarning(plFmt("The audio capture '{}' could not be opened.", result.m_Paths[0]));
    return;
  }

  file.ReadAll(content);
  file.Close();

  SetRecording(false);

  plRawMemoryStreamReader reader(content.GetData(), content.GetCount());
  if (m_Capture.Load(reader).Failed())
  {
    plQtUiServices::MessageBoxWarning(plFmt("'{}' is not an audio capture this version can read.", result.m_Paths[0]));
  }

  m_bCaptureFromFile = true;
  CaptureChanged();

  plStringBuilder sToast;
  sToast.SetFormat("Opened {}", plStringView(result.m_Paths[0]).GetFileNameAndExtension());
  ShowToast(sToast);
}

void plQtAudioProfilerPanel::SelectEntity(plStringView sEntity)
{
  plDocument* pScene = FindActiveSceneDocument();
  if (pScene == nullptr || sEntity.IsEmpty())
  {
    ShowToast("Open the scene the capture was recorded in to select its objects");
    return;
  }

  plDeque<const plDocumentObject*> objects;
  FindObjectsByName(pScene->GetObjectManager()->GetRootObject(), sEntity, objects);

  plStringBuilder sToast;
  if (objects.IsEmpty())
  {
    sToast.SetFormat("No object named '{}' in {}", sEntity, plStringView(pScene->GetDocumentPath()).GetFileName());
  }
  else
  {
    pScene->GetSelectionManager()->SetSelection(objects);
    sToast.SetFormat("Selected {} object(s) named '{}' in {}", objects.GetCount(), sEntity, plStringView(pScene->GetDocumentPath()).GetFileName());
  }

  ShowToast(sToast);
}

void plQtAudioProfilerPanel::OpenEvent(plStringView sEvent)
{
  // the event's resource id or description: its GUID, or the path of its built file
  plString sPath;
  {
    const auto pSubAsset = plAssetCurator::GetSingleton()->FindSubAsset(sEvent);
    if (pSubAsset.isValid() && pSubAsset->m_pAssetInfo != nullptr)
      sPath = pSubAsset->m_pAssetInfo->m_Path.GetAbsolutePath();
  }

  // a built file names the asset it was built from
  if (sPath.IsEmpty())
  {
    const plStringView sName = sEvent.GetFileName();
    const auto subAssets = plAssetCurator::GetSingleton()->GetKnownSubAssets();
    for (auto it = subAssets->GetIterator(); it.IsValid(); ++it)
    {
      if (it.Value().m_Data.m_sSubAssetsDocumentTypeName.GetView() == "Audio Event" && it.Value().GetName() == sName && it.Value().m_pAssetInfo != nullptr)
        sPath = it.Value().m_pAssetInfo->m_Path.GetAbsolutePath();
    }
  }

  if (sPath.IsEmpty())
  {
    plStringBuilder sToast;
    sToast.SetFormat("The event '{}' is not an asset of this project", sEvent.GetFileName());
    ShowToast(sToast);
    return;
  }

  plQtEditorApp::GetSingleton()->OpenDocument(sPath, plDocumentFlags::RequestWindow | plDocumentFlags::AddToRecentFilesList);
}

//////////////////////////////////////////////////////////////////////////
// Views

void plQtAudioProfilerPanel::ConnectView(plEditorUiAudioProfilerView* pView)
{
  pView->m_OnRecordToggled = [this](bool bRecording)
  { SetRecording(bRecording); };
  pView->m_OnSave = [this]()
  { SaveCapture(); };
  pView->m_OnOpen = [this]()
  { OpenCapture(); };
  pView->m_OnTargetChosen = [this](plUInt32 uiTarget)
  { SetTarget(uiTarget == 1 ? Target::Player : Target::EngineProcess); };
  pView->m_OnSelectEntity = [this](plStringView sEntity)
  { SelectEntity(sEntity); };
  pView->m_OnOpenEvent = [this](plStringView sEvent)
  { OpenEvent(sEvent); };

  pView->SetCapture(&m_Capture);
  pView->SetRecording(m_bRecording);
}

plUniquePtr<plEditorUiContent> plQtAudioProfilerPanel::CreateShellView()
{
  plEditorUiAudioProfilerView* pView = PL_DEFAULT_NEW(plEditorUiAudioProfilerView);
  ConnectView(pView);
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
