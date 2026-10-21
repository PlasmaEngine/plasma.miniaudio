#include <EnginePluginPlasmaAudio/EnginePluginPlasmaAudioPCH.h>

#include <AudioSystem/Core/AudioSystem.h>
#include <AudioSystem/Core/AudioSystemRequests.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Core/ResourceManager/ResourceTypeLoader.h>
#include <EnginePluginPlasmaAudio/Mixer/AudioMixerContext.h>
#include <Foundation/IO/MemoryStream.h>
#include <PlasmaAudioPlugin/Events/AudioAudition.h>

namespace
{
  /// How long a load request of the Preview source is given before it is sent again.
  constexpr plTime s_LoadRetry = plTime::MakeFromSeconds(3.0);
} // namespace

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerContext, 1, plRTTIDefaultAllocator<plAudioMixerContext>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_CONSTANT_PROPERTY("DocumentType", (const char*) "Audio Mixer"),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plAudioMixerContext::plAudioMixerContext()
  : plEngineProcessDocumentContext(plEngineProcessDocumentContextFlags::None)
{
}

plAudioMixerContext::~plAudioMixerContext() = default;

void plAudioMixerContext::OnInitialize()
{
  plStringBuilder sGuid;
  m_sResourceId = plConversionUtils::ToString(GetDocumentGuid(), sGuid);

  if (plAudioAudition* pAudition = plAudioAudition::GetSingleton())
  {
    pAudition->SetMixerMonitor(true);
  }
}

void plAudioMixerContext::OnDeinitialize()
{
  plAudioAudition* pAudition = plAudioAudition::GetSingleton();

  if (pAudition != nullptr)
  {
    pAudition->SetMixerMonitor(false);

    if (m_bTestSignal)
    {
      pAudition->SetTestSignal(false);
    }
  }

  if (m_bTouchedState)
  {
    SendMixerControl(plAudioMixerControlOp::ClearMuteAndSolo, {}, 0.0f);
    SendMixerControl(plAudioMixerControlOp::StopAllSnapshots, {}, 0.0f);
  }

  // the engine process plays the saved mixer again; a loaded one applies it in place
  if (m_bCustomized && m_hMixer.IsValid())
  {
    plResourceManager::RestoreResource(m_hMixer);
  }

  m_hMixer.Invalidate();
}

void plAudioMixerContext::HandleMessage(const plEditorEngineDocumentMsg* pMsg)
{
  // the base class handles object mirroring and views, which need the world this context does not have
  const auto* pConsoleMsg = plDynamicCast<const plAudioMixerConsoleMsgToEngine*>(pMsg);
  if (pConsoleMsg == nullptr)
    return;

  plAudioAudition* pAudition = plAudioAudition::GetSingleton();
  if (pAudition == nullptr)
  {
    SendStatus("The engine process has no PlasmaAudio output (another audio middleware is active, or no audio device).");
    return;
  }

  plString sError;

  switch (pConsoleMsg->m_Command)
  {
    case plAudioMixerConsoleCommand::Apply:
      sError = ApplyMixer(pConsoleMsg->m_MixerData);
      m_bWantLoad = pConsoleMsg->m_bLoad;
      break;

    case plAudioMixerConsoleCommand::SetBusMute:
      SendMixerControl(plAudioMixerControlOp::SetBusMute, pConsoleMsg->m_sTarget, pConsoleMsg->m_fValue);
      m_bTouchedState = true;
      break;

    case plAudioMixerConsoleCommand::SetBusSolo:
      SendMixerControl(plAudioMixerControlOp::SetBusSolo, pConsoleMsg->m_sTarget, pConsoleMsg->m_fValue);
      m_bTouchedState = true;
      break;

    case plAudioMixerConsoleCommand::ClearMuteAndSolo:
      SendMixerControl(plAudioMixerControlOp::ClearMuteAndSolo, {}, 0.0f);
      break;

    case plAudioMixerConsoleCommand::SetSnapshot:
      SendMixerControl(plAudioMixerControlOp::SetSnapshotIntensityNow, pConsoleMsg->m_sTarget, pConsoleMsg->m_fValue);
      m_bTouchedState = true;
      break;

    case plAudioMixerConsoleCommand::SetTestSignal:
      m_bTestSignal = pConsoleMsg->m_fValue != 0.0f;
      pAudition->SetTestSignal(m_bTestSignal);
      break;

    default:
      break;
  }

  SendStatus(sError);
}

plString plAudioMixerContext::ApplyMixer(const plDataBuffer& data)
{
  if (data.IsEmpty())
    return "The mixer console sent no mixer.";

  if (!m_hMixer.IsValid())
  {
    m_hMixer = plResourceManager::LoadResource<plAudioMixerResource>(m_sResourceId);
  }

  plUniquePtr<plResourceLoaderFromMemory> pLoader(PL_DEFAULT_NEW(plResourceLoaderFromMemory));
  pLoader->m_ModificationTimestamp = plTimestamp::CurrentTimestamp();
  pLoader->m_sResourceDescription = "Audio Mixer Console";

  plMemoryStreamWriter writer(&pLoader->m_CustomData);
  writer.WriteBytes(data.GetData(), data.GetCount()).IgnoreResult();

  plResourceManager::UpdateResourceWithCustomLoader(m_hMixer, std::move(pLoader));
  plResourceManager::ForceLoadResourceNow(m_hMixer);
  m_bCustomized = true;

  plResourceLock<plAudioMixerResource> pMixer(m_hMixer, plResourceAcquireMode::PointerOnly);
  if (pMixer.GetAcquireResult() != plResourceAcquireResult::Final || pMixer->GetLoadingState() != plResourceState::Loaded)
    return "The engine process could not read the mixer.";

  return plString();
}

void plAudioMixerContext::SendMixerControl(plUInt8 uiOp, plStringView sTarget, float fValue)
{
  plAudioSystem* pAudioSystem = plAudioSystem::GetSingleton();
  if (pAudioSystem == nullptr)
    return;

  plAudioSystemRequestSetMixerControl request;
  request.m_uiOp = uiOp;
  request.m_sTarget = sTarget;
  request.m_fValue = fValue;
  pAudioSystem->SendRequest(request);
}

void plAudioMixerContext::LoadIfWanted(bool bIsThisMixer, bool bMonitorRefreshed)
{
  if (!m_bWantLoad || bIsThisMixer || !bMonitorRefreshed || !m_hMixer.IsValid())
    return;

  // loading a mixer stops everything that plays, so it is requested once and only repeated when it did not happen
  const plTime tNow = plTime::Now();
  if (m_LastLoadRequest.IsPositive() && tNow - m_LastLoadRequest < s_LoadRetry)
    return;

  m_LastLoadRequest = tNow;
  SendMixerControl(plAudioMixerControlOp::LoadMixer, m_sResourceId, 0.0f);
}

void plAudioMixerContext::SendStatus(const plString& sError)
{
  plAudioMixerConsoleStatusMsgToEditor msg;
  msg.m_DocumentGuid = GetDocumentGuid();
  msg.m_sError = sError;

  plAudioAudition* pAudition = plAudioAudition::GetSingleton();
  msg.m_bAvailable = pAudition != nullptr;

  if (pAudition != nullptr)
  {
    plAudioMixerMonitorStatus status;
    pAudition->GetMixerMonitor(status);

    msg.m_bHasMixer = status.m_bHasMixer;
    msg.m_sLoadedMixer = status.m_sMixerResourceId;
    msg.m_bIsThisMixer = status.m_bHasMixer && status.m_sMixerResourceId.IsEqual_NoCase(m_sResourceId);
    msg.m_bTestSignal = status.m_bTestSignal;
    msg.m_uiLayoutRebuilds = status.m_uiLayoutRebuilds;
    msg.m_uiRealVoices = status.m_uiRealVoices;
    msg.m_BusNames = status.m_BusNames;
    msg.m_BusPeaks = status.m_BusPeaks;
    msg.m_BusRms = status.m_BusRms;
    msg.m_EffectGainReductions = status.m_EffectGainReductions;
    msg.m_SnapshotNames = status.m_SnapshotNames;
    msg.m_SnapshotIntensities = status.m_SnapshotIntensities;

    msg.m_BusMuted.SetCount(status.m_BusMuted.GetCount());
    msg.m_BusSoloed.SetCount(status.m_BusSoloed.GetCount());
    for (plUInt32 i = 0; i < status.m_BusMuted.GetCount(); ++i)
    {
      msg.m_BusMuted[i] = status.m_BusMuted[i] ? 1 : 0;
      msg.m_BusSoloed[i] = status.m_BusSoloed[i] ? 1 : 0;
    }

    LoadIfWanted(msg.m_bIsThisMixer, status.m_uiRefreshCount > 0);
  }

  SendProcessMessage(&msg);
}
