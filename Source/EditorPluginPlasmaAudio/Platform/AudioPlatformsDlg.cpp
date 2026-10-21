#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/Platform/AudioPlatformsDlg.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Time/Timestamp.h>
#include <GuiFoundation/UIServices/UIServices.moc.h>
#include <PlasmaAudioPlugin/Platform/AudioBackends.h>

#include <QTimer>

namespace
{
  /// Often enough to see a fade-out of the default length as its own state.
  constexpr int kPollIntervalMs = 100;

  /// A request without an answer after this long no longer blocks the next one.
  const plTime kRequestTimeout = plTime::MakeFromSeconds(2.0);

  /// How long Simulate suspend keeps the engine process suspended.
  constexpr int kSimulatedSuspendMs = 1500;

  /// The time each measurement of the benchmark takes.
  const plTime kBenchmarkBudget = plTime::MakeFromMilliseconds(400);

  constexpr const char* s_szDeviceStat = "PlasmaAudio/Output/Device";
  constexpr const char* s_szStateStat = "PlasmaAudio/Output/State";
  constexpr const char* s_szClose = "Close";

  void CopyConfig(const plPlasmaAudioPlatformConfig& from, plPlasmaAudioPlatformConfig& to)
  {
    to.m_sBackend = from.m_sBackend;
    to.m_eSpeakers = from.m_eSpeakers;
    to.m_uiSampleRate = from.m_uiSampleRate;
    to.m_uiPeriodMs = from.m_uiPeriodMs;
    to.m_bFollowDefaultDevice = from.m_bFollowDefaultDevice;
    to.m_eSuspendPolicy = from.m_eSuspendPolicy;
    to.m_fSuspendFadeMs = from.m_fSuspendFadeMs;
  }

  /// "12:04:31  " in front of a log line.
  plString Stamped(plStringView sText)
  {
    const plDateTime now = plDateTime::MakeFromTimestamp(plTimestamp::CurrentTimestamp());

    plStringBuilder s;
    s.SetFormat("{}:{}:{}  {}", plArgU(now.GetHour(), 2, true), plArgU(now.GetMinute(), 2, true), plArgU(now.GetSecond(), 2, true), sText);
    return s;
  }
} // namespace

plQtAudioPlatformsDlg::plQtAudioPlatformsDlg(QWidget* pParent)
  : plQtEngineDialog(pParent, "AudioPlatforms", "Audio Platforms")
{
  setWindowIcon(plQtUiServices::GetCachedIconResource(":/AssetIcons/AudioMixer.svg"));

  plAudioBackendRegistry::GetBackendNames(m_Backends);
  m_uiShownProfile = plAssetCurator::GetSingleton()->GetActiveAssetProfileIndex();

  if (!IsReady())
    return;

  plEditorUiDialogButton buttons[1];
  buttons[0].m_sId = s_szClose;
  buttons[0].m_sLabel = "Close";
  buttons[0].m_Role = plEditorUiDialogButton::Role::Accept;
  GetHost().SetButtons(buttons);
  GetHost().SetFooterHint("Changes save to the asset profile at once; the active profile also applies to the engine process");

  m_pView = PL_DEFAULT_NEW(plEditorUiAudioPlatformsView);
  m_pView->m_OnProfileChosen = [this](plUInt32 uiProfile)
  { ShowProfile(uiProfile); };
  m_pView->m_OnConfigChanged = [this](const plPlasmaAudioPlatformConfig& config)
  { OnConfigChanged(config); };
  m_pView->m_OnRunBenchmark = [this]()
  { RunBenchmark(); };
  m_pView->m_OnSimulateSuspend = [this]()
  { SimulateSuspend(); };
  m_pView->SetSimdName(plAudioCodecBenchmark::GetOpusSimdName());
  RefreshView();

  plEditorEngineProcessConnection::s_Events.AddEventHandler(plMakeDelegate(&plQtAudioPlatformsDlg::EngineProcessMsgHandler, this));

  m_pTimer = new QTimer(this);
  m_pTimer->setInterval(kPollIntervalMs);
  connect(m_pTimer, &QTimer::timeout, this, [this]()
    { OnTick(); });
  m_pTimer->start();
}

plQtAudioPlatformsDlg::~plQtAudioPlatformsDlg()
{
  if (m_pView == nullptr)
    return;

  plEditorEngineProcessConnection::s_Events.RemoveEventHandler(plMakeDelegate(&plQtAudioPlatformsDlg::EngineProcessMsgHandler, this));

  // a suspend the dialog started does not outlive it
  if (m_bSuspendRunning)
    EndSimulatedSuspend();

  if (m_pBenchmark != nullptr)
    plTaskSystem::WaitForGroup(m_BenchmarkTask);
}

int plQtAudioPlatformsDlg::Run(QWidget* pParent)
{
  if (!plQtEngineDialog::IsEngineEnabled())
  {
    plLog::Error("The audio platforms dialog needs the editor UI renderer.");
    return QDialog::Rejected;
  }

  plQtAudioPlatformsDlg dlg(pParent);
  return dlg.exec();
}

void plQtAudioPlatformsDlg::BuildBody(plUiElement* pBody, const plVec2& vSize)
{
  // an engine window shows the view over the body; the Qt frame (tests without a shell) has nothing to show it with
  if (IsInEngineWindow() && m_pView != nullptr)
  {
    SetBodyContent(m_pView.Borrow(), plRectFloat(0.0f, 0.0f, vSize.x, vSize.y));
    return;
  }

  plEditorUi::TextAt(pBody, "The audio platforms show in the editor's engine window.", plRectFloat(12.0f, 12.0f, vSize.x - 24.0f, 20.0f), plEditorUi::Dim(), plEditorUi::FontSize - 1);
}

plVec2 plQtAudioPlatformsDlg::GetPreferredSize() const
{
  return plVec2(940.0f, 560.0f);
}

plEditorEngineProcessConnection* plQtAudioPlatformsDlg::GetLiveConnection()
{
  plEditorEngineProcessConnection* pConnection = plEditorEngineProcessConnection::GetSingleton();
  if (pConnection == nullptr || pConnection->IsProcessCrashed() || !pConnection->IsEngineSetup())
    return nullptr;

  return pConnection;
}

plPlasmaAudioPlatformConfig* plQtAudioPlatformsDlg::GetConfig(plUInt32 uiProfile)
{
  plAssetCurator* pCurator = plAssetCurator::GetSingleton();
  if (uiProfile >= pCurator->GetNumAssetProfiles())
    return nullptr;

  plPlatformProfile* pProfile = pCurator->GetAssetProfile(uiProfile);

  // profiles saved before the package was added have no config of its type
  if (pProfile->GetTypeConfig<plPlasmaAudioPlatformConfig>() == nullptr)
    pProfile->AddMissingConfigs();

  return pProfile->GetTypeConfig<plPlasmaAudioPlatformConfig>();
}

//////////////////////////////////////////////////////////////////////////
// Engine process

void plQtAudioPlatformsDlg::OnTick()
{
  if (m_pBenchmark != nullptr && m_pBenchmark->m_bDone)
    FinishBenchmark();

  plEditorEngineProcessConnection* pConnection = GetLiveConnection();
  if (pConnection == nullptr)
  {
    m_bRequestPending = false;

    if (!m_sDevice.IsEmpty() || !m_sState.IsEmpty())
    {
      m_sDevice.Clear();
      m_sState.Clear();
      m_pView->SetOutput({}, {});
    }
    return;
  }

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

void plQtAudioPlatformsDlg::EngineProcessMsgHandler(const plEditorEngineProcessConnection::Event& e)
{
  switch (e.m_Type)
  {
    case plEditorEngineProcessConnection::Event::Type::ProcessMessage:
    {
      if (const auto* pMsg = plDynamicCast<const plStatsSnapshotResultMsgToEditor*>(e.m_pMsg))
      {
        m_bRequestPending = false;

        if (pMsg->m_Stats.IsValid())
          ApplyStats(pMsg->m_Stats);
      }
    }
    break;

    case plEditorEngineProcessConnection::Event::Type::ProcessStarted:
    case plEditorEngineProcessConnection::Event::Type::ProcessRestarted:
    case plEditorEngineProcessConnection::Event::Type::ProcessShutdown:
    case plEditorEngineProcessConnection::Event::Type::ProcessCrashed:
      m_bRequestPending = false;
      break;

    default:
      break;
  }
}

void plQtAudioPlatformsDlg::ApplyStats(const plVariant& stats)
{
  if (!stats.IsA<plVariantDictionary>())
    return;

  const plVariantDictionary& dict = stats.Get<plVariantDictionary>();

  plString sDevice;
  plString sState;
  const plVariant* pValue = nullptr;
  if (dict.TryGetValue(s_szDeviceStat, pValue))
    sDevice = pValue->ConvertTo<plString>();
  if (dict.TryGetValue(s_szStateStat, pValue))
    sState = pValue->ConvertTo<plString>();

  if (sDevice == m_sDevice && sState == m_sState)
    return;

  // the first answer is what the output runs on now; later ones are changes
  const bool bFirst = m_sDevice.IsEmpty() && m_sState.IsEmpty();

  if (sDevice != m_sDevice && !sDevice.IsEmpty())
  {
    plStringBuilder s(bFirst ? "output runs on: " : "output opened: ", sDevice);
    AddLog(s, plEditorUiAudioPlatformsView::LogKind::Ok);
  }

  if (sState != m_sState && !sState.IsEmpty() && !bFirst)
  {
    plStringBuilder s("output ", sState);
    AddLog(s, plStringView(sState).StartsWith("running") ? plEditorUiAudioPlatformsView::LogKind::Ok : plEditorUiAudioPlatformsView::LogKind::Warning);
  }

  m_sDevice = sDevice;
  m_sState = sState;
  m_pView->SetOutput(m_sDevice, m_sState);
}

void plQtAudioPlatformsDlg::SendCVar(plEditorEngineProcessConnection* pConnection, const char* szName, const plVariant& value)
{
  plChangeCVarMsgToEngine msg;
  msg.m_sCVarName = szName;
  msg.m_NewValue = value;
  pConnection->SendMessage(&msg);
}

void plQtAudioPlatformsDlg::PushActiveConfig()
{
  plEditorEngineProcessConnection* pConnection = GetLiveConnection();
  const plPlasmaAudioPlatformConfig* pConfig = GetConfig(plAssetCurator::GetSingleton()->GetActiveAssetProfileIndex());
  if (pConnection == nullptr || pConfig == nullptr)
    return;

  // the backend last: it may recreate the context, which then opens the device with the other settings
  SendCVar(pConnection, "PlasmaAudio.Speakers", static_cast<plInt32>(pConfig->m_eSpeakers.GetValue()));
  SendCVar(pConnection, "PlasmaAudio.SampleRate", static_cast<plInt32>(pConfig->m_uiSampleRate));
  SendCVar(pConnection, "PlasmaAudio.PeriodMs", static_cast<plInt32>(pConfig->m_uiPeriodMs));
  SendCVar(pConnection, "PlasmaAudio.FollowDefaultDevice", pConfig->m_bFollowDefaultDevice);
  SendCVar(pConnection, "PlasmaAudio.SuspendPolicy", static_cast<plInt32>(pConfig->m_eSuspendPolicy.GetValue()));
  SendCVar(pConnection, "PlasmaAudio.SuspendFadeMs", pConfig->m_fSuspendFadeMs);
  SendCVar(pConnection, "PlasmaAudio.Backend", pConfig->m_sBackend);
}

//////////////////////////////////////////////////////////////////////////
// Actions

void plQtAudioPlatformsDlg::ShowProfile(plUInt32 uiProfile)
{
  if (uiProfile == m_uiShownProfile)
    return;

  m_uiShownProfile = uiProfile;
  RefreshView();
}

void plQtAudioPlatformsDlg::OnConfigChanged(const plPlasmaAudioPlatformConfig& config)
{
  plAssetCurator* pCurator = plAssetCurator::GetSingleton();
  plPlasmaAudioPlatformConfig* pConfig = GetConfig(m_uiShownProfile);
  if (pConfig == nullptr)
    return;

  CopyConfig(config, *pConfig);

  if (pCurator->SaveAssetProfiles().Failed())
    AddLog("the asset profiles could not be saved", plEditorUiAudioPlatformsView::LogKind::Warning);

  pCurator->SaveRuntimeProfiles();

  plStringBuilder s;
  const plStringView sProfile = pCurator->GetAssetProfile(m_uiShownProfile)->GetConfigName();
  if (m_uiShownProfile == pCurator->GetActiveAssetProfileIndex())
  {
    PushActiveConfig();
    s.SetFormat("{} saved, applied to the engine process", sProfile);
  }
  else
  {
    s.SetFormat("{} saved", sProfile);
  }

  AddLog(s);
}

void plQtAudioPlatformsDlg::RunBenchmark()
{
  if (m_pBenchmark != nullptr)
    return;

  m_pBenchmark = PL_DEFAULT_NEW(BenchmarkJob);

  plSharedPtr<BenchmarkJob> pJob = m_pBenchmark;
  plSharedPtr<plDelegateTask<void>> pTask = PL_DEFAULT_NEW(plDelegateTask<void>, "AudioDecodeBenchmark", plTaskNesting::Never, [pJob]()
    {
      pJob->m_bSucceeded = plAudioCodecBenchmark::Run(kBenchmarkBudget, pJob->m_Result).Succeeded();
      pJob->m_bDone = true; });
  m_BenchmarkTask = plTaskSystem::StartSingleTask(pTask, plTaskPriority::LongRunning);

  m_pView->SetBenchmarkRunning(true);
}

void plQtAudioPlatformsDlg::FinishBenchmark()
{
  const BenchmarkJob& job = *m_pBenchmark;
  m_pView->SetBenchmarkRunning(false);

  if (job.m_bSucceeded)
  {
    m_pView->SetBenchmark(job.m_Result);

    plStringBuilder s;
    s.SetFormat("benchmark: Opus plain C {}, {} {}, ADPCM {} voices / 1 %", plArgF(job.m_Result.m_fOpusPlainVoices, 1), job.m_Result.m_sSimd, plArgF(job.m_Result.m_fOpusSimdVoices, 1), plArgF(job.m_Result.m_fAdpcmVoices, 1));
    AddLog(s);
  }
  else
  {
    AddLog("benchmark failed: the test signal could not be encoded", plEditorUiAudioPlatformsView::LogKind::Warning);
  }

  m_pBenchmark = nullptr;
}

void plQtAudioPlatformsDlg::SimulateSuspend()
{
  plEditorEngineProcessConnection* pConnection = GetLiveConnection();
  if (pConnection == nullptr)
  {
    AddLog("no engine process to suspend", plEditorUiAudioPlatformsView::LogKind::Warning);
    return;
  }

  // the policy of the shown profile, for this one suspend
  if (const plPlasmaAudioPlatformConfig* pConfig = GetConfig(m_uiShownProfile))
  {
    SendCVar(pConnection, "PlasmaAudio.SuspendPolicy", static_cast<plInt32>(pConfig->m_eSuspendPolicy.GetValue()));
    SendCVar(pConnection, "PlasmaAudio.SuspendFadeMs", pConfig->m_fSuspendFadeMs);
  }

  SendCVar(pConnection, "PlasmaAudio.Suspend", true);
  AddLog("suspend requested");

  m_bSuspendRunning = true;
  m_pView->SetSuspendRunning(true);

  QTimer::singleShot(kSimulatedSuspendMs, this, [this]()
    { EndSimulatedSuspend(); });
}

void plQtAudioPlatformsDlg::EndSimulatedSuspend()
{
  if (!m_bSuspendRunning)
    return;

  m_bSuspendRunning = false;
  m_pView->SetSuspendRunning(false);

  if (plEditorEngineProcessConnection* pConnection = GetLiveConnection())
  {
    SendCVar(pConnection, "PlasmaAudio.Suspend", false);
    AddLog("resume requested");

    // the shown profile may not be the active one: the engine process goes back to the active profile's policy
    PushActiveConfig();
  }
}

//////////////////////////////////////////////////////////////////////////
// View

void plQtAudioPlatformsDlg::AddLog(plStringView sText, plEditorUiAudioPlatformsView::LogKind kind)
{
  m_pView->AddLog(Stamped(sText), kind);
}

void plQtAudioPlatformsDlg::RefreshView()
{
  plAssetCurator* pCurator = plAssetCurator::GetSingleton();

  plHybridArray<plString, 8> names;
  for (plUInt32 i = 0; i < pCurator->GetNumAssetProfiles(); ++i)
    names.PushBack(pCurator->GetAssetProfile(i)->GetConfigName());

  m_uiShownProfile = plMath::Min(m_uiShownProfile, names.IsEmpty() ? 0u : names.GetCount() - 1);

  // a backend a profile names that this computer does not have stays choosable
  plHybridArray<plString, 8> backends = m_Backends;
  for (plUInt32 i = 0; i < pCurator->GetNumAssetProfiles(); ++i)
  {
    const plPlasmaAudioPlatformConfig* pConfig = GetConfig(i);
    if (pConfig != nullptr && !pConfig->m_sBackend.IsEmpty() && !backends.Contains(pConfig->m_sBackend))
      backends.PushBack(pConfig->m_sBackend);
  }

  m_pView->SetProfiles(names, m_uiShownProfile, pCurator->GetActiveAssetProfileIndex());
  m_pView->SetBackends(backends);

  if (const plPlasmaAudioPlatformConfig* pShown = GetConfig(m_uiShownProfile))
    m_pView->SetConfig(*pShown);
}
