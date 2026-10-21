#include <EnginePluginPlasmaAudio/EnginePluginPlasmaAudioPCH.h>

#include <AudioSystem/Core/AudioSystem.h>
#include <AudioSystem/Core/AudioSystemAllocator.h>
#include <AudioSystem/Core/AudioSystemRequests.h>
#include <EnginePluginPlasmaAudio/Banks/AudioBankContext.h>
#include <PlasmaAudioPlugin/Events/AudioAudition.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioBankContext, 1, plRTTIDefaultAllocator<plAudioBankContext>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_CONSTANT_PROPERTY("DocumentType", (const char*) "Audio Bank"),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plAudioBankContext::plAudioBankContext()
  : plEngineProcessDocumentContext(plEngineProcessDocumentContextFlags::None)
{
}

plAudioBankContext::~plAudioBankContext() = default;

void plAudioBankContext::OnInitialize()
{
  if (plAudioAudition* pAudition = plAudioAudition::GetSingleton())
  {
    pAudition->SetBankMonitor(true);
  }
}

void plAudioBankContext::OnDeinitialize()
{
  for (const plString& sBank : m_LoadedBanks)
  {
    SendBankRequest(sBank, false);
  }

  m_LoadedBanks.Clear();

  if (plAudioAudition* pAudition = plAudioAudition::GetSingleton())
  {
    pAudition->SetBankMonitor(false);
  }
}

void plAudioBankContext::HandleMessage(const plEditorEngineDocumentMsg* pMsg)
{
  // the base class handles object mirroring and views, which need the world this context does not have
  const auto* pBankMsg = plDynamicCast<const plAudioBankMsgToEngine*>(pMsg);
  if (pBankMsg == nullptr)
    return;

  if (plAudioAudition::GetSingleton() == nullptr)
  {
    SendStatus("The engine process has no PlasmaAudio output (another audio middleware is active, or no audio device).");
    return;
  }

  switch (pBankMsg->m_Command)
  {
    case plAudioBankCommand::Load:
      if (!pBankMsg->m_sBank.IsEmpty() && !m_LoadedBanks.Contains(pBankMsg->m_sBank))
      {
        SendBankRequest(pBankMsg->m_sBank, true);
        m_LoadedBanks.PushBack(pBankMsg->m_sBank);
      }
      break;

    case plAudioBankCommand::Unload:
      if (m_LoadedBanks.RemoveAndCopy(pBankMsg->m_sBank))
      {
        SendBankRequest(pBankMsg->m_sBank, false);
      }
      break;

    default:
      break;
  }

  SendStatus(plString());
}

void plAudioBankContext::SendBankRequest(plStringView sBank, bool bLoad)
{
  plAudioSystem* pAudioSystem = plAudioSystem::GetSingleton();
  if (pAudioSystem == nullptr)
    return;

  // the bank id a bank loader component uses for the same bank
  plAudioSystemDataID uiBankId = pAudioSystem->GetBankId(sBank);
  if (uiBankId == kInvalidAudioSystemId)
  {
    uiBankId = plHashHelper<plStringView>::Hash(sBank);
    pAudioSystem->RegisterSoundBank(uiBankId, PL_AUDIOSYSTEM_NEW(plAudioSystemStringBankData, uiBankId, sBank));
  }

  if (bLoad)
  {
    plAudioSystemRequestLoadBank request;
    request.m_uiObjectId = uiBankId;
    pAudioSystem->SendRequest(request);
  }
  else
  {
    plAudioSystemRequestUnloadBank request;
    request.m_uiObjectId = uiBankId;
    pAudioSystem->SendRequest(request);
  }
}

void plAudioBankContext::SendStatus(const plString& sError)
{
  plAudioBankStatusMsgToEditor msg;
  msg.m_DocumentGuid = GetDocumentGuid();
  msg.m_sError = sError;
  msg.m_LoadedHere = m_LoadedBanks;

  plAudioAudition* pAudition = plAudioAudition::GetSingleton();
  msg.m_bAvailable = pAudition != nullptr;

  if (pAudition != nullptr)
  {
    plAudioBankMonitorStatus status;
    pAudition->GetBankMonitor(status);

    for (const plAudioBankMonitorEntry& bank : status.m_Banks)
    {
      msg.m_Banks.PushBack(bank.m_sBank);
      msg.m_States.PushBack(bank.m_uiState);
      msg.m_ReadyItems.PushBack(bank.m_uiReadyItems);
      msg.m_Items.PushBack(bank.m_uiItems);
      msg.m_OverBudget.PushBack(bank.m_bOverBudget ? 1 : 0);
    }

    msg.m_uiDecodedBytes = status.m_Usage.m_uiDecodedBytes;
    msg.m_uiCompressedBytes = status.m_Usage.m_uiCompressedBytes;
    msg.m_uiPrimingBytes = status.m_Usage.m_uiPrimingBytes;
    msg.m_uiRingBytes = status.m_Usage.m_uiRingBytes;
    msg.m_uiBudgetTotalBytes = status.m_Budget.m_uiTotalBytes;
  }

  SendProcessMessage(&msg);
}
