#include <EnginePluginPlasmaAudio/EnginePluginPlasmaAudioPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <Core/ResourceManager/ResourceTypeLoader.h>
#include <EnginePluginPlasmaAudio/Audition/AudioEventContext.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Types/ScopeExit.h>
#include <PlasmaAudioPlugin/Events/AudioAudition.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioEventContext, 1, plRTTIDefaultAllocator<plAudioEventContext>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_CONSTANT_PROPERTY("DocumentType", (const char*) "Audio Event"),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plAudioEventContext::plAudioEventContext()
  : plEngineProcessDocumentContext(plEngineProcessDocumentContextFlags::None)
{
}

plAudioEventContext::~plAudioEventContext() = default;

void plAudioEventContext::OnInitialize()
{
  m_uiKey = plHashingUtils::xxHash64(&GetDocumentGuid(), sizeof(plUuid));

  plStringBuilder sGuid;
  plConversionUtils::ToString(GetDocumentGuid(), sGuid);
  m_sResourceId = plStringBuilder("PlasmaAudio.Audition.", sGuid);
}

void plAudioEventContext::OnDeinitialize()
{
  if (plAudioAudition* pAudition = plAudioAudition::GetSingleton())
  {
    pAudition->Remove(m_uiKey);
  }

  m_hEvent.Invalidate();
}

void plAudioEventContext::HandleMessage(const plEditorEngineDocumentMsg* pMsg)
{
  // the base class handles object mirroring and views, which need the world this context does not have
  const auto* pAuditionMsg = plDynamicCast<const plAudioAuditionMsgToEngine*>(pMsg);
  if (pAuditionMsg == nullptr)
    return;

  plAudioAudition* pAudition = plAudioAudition::GetSingleton();
  if (pAudition == nullptr)
  {
    SendStatus("The engine process has no PlasmaAudio output (another audio middleware is active, or no audio device).");
    return;
  }

  plString sError;

  switch (pAuditionMsg->m_Command)
  {
    case plAudioAuditionCommand::Play:
    {
      sError = LoadEvent(pAuditionMsg->m_EventData);
      if (!sError.IsEmpty())
        break;

      plAudioAuditionRequest request;
      request.m_sEventResourceId = m_sResourceId;
      request.m_sMixerResourceId = pAuditionMsg->m_sMixer;
      request.m_StartOffset = plTime::MakeFromSeconds(plMath::Max(pAuditionMsg->m_fStartOffset, 0.0));
      request.m_bSeeded = pAuditionMsg->m_bSeeded;
      request.m_uiSeed = pAuditionMsg->m_uiSeed;
      request.m_SwitchStates = pAuditionMsg->m_SwitchStates;
      pAudition->Play(m_uiKey, request);
      break;
    }

    case plAudioAuditionCommand::Stop:
      pAudition->Stop(m_uiKey);
      break;

    case plAudioAuditionCommand::SetParameters:
      pAudition->SetParameters(m_uiKey, pAuditionMsg->m_ParameterNames, pAuditionMsg->m_ParameterValues);
      break;

    default:
      break;
  }

  SendStatus(sError);
}

plString plAudioEventContext::LoadEvent(const plDataBuffer& data)
{
  if (data.IsEmpty())
    return "The audition sent no event.";

  if (!m_hEvent.IsValid())
  {
    m_hEvent = plResourceManager::LoadResource<plAudioEventResource>(m_sResourceId);
  }

  // a reload unloads the previous content first, which stops the voices that still read it
  plUniquePtr<plResourceLoaderFromMemory> pLoader(PL_DEFAULT_NEW(plResourceLoaderFromMemory));
  pLoader->m_ModificationTimestamp = plTimestamp::CurrentTimestamp();
  pLoader->m_sResourceDescription = "Audio Event Audition";

  plMemoryStreamWriter writer(&pLoader->m_CustomData);
  writer.WriteBytes(data.GetData(), data.GetCount()).IgnoreResult();

  plResourceManager::UpdateResourceWithCustomLoader(m_hEvent, std::move(pLoader));
  plResourceManager::ForceLoadResourceNow(m_hEvent);

  plResourceLock<plAudioEventResource> pEvent(m_hEvent, plResourceAcquireMode::PointerOnly);
  if (pEvent.GetAcquireResult() != plResourceAcquireResult::Final || pEvent->GetLoadingState() != plResourceState::Loaded)
    return "The engine process could not read the event.";

  return plString();
}

void plAudioEventContext::SendStatus(const plString& sError)
{
  plAudioAuditionStatusMsgToEditor msg;
  msg.m_DocumentGuid = GetDocumentGuid();
  msg.m_sError = sError;

  plAudioAudition* pAudition = plAudioAudition::GetSingleton();
  msg.m_bAvailable = pAudition != nullptr;

  plAudioAuditionStatus status;
  if (pAudition != nullptr && pAudition->GetStatus(m_uiKey, status))
  {
    msg.m_bPlaying = status.m_bPlaying;
    msg.m_bWaiting = status.m_bWaiting;
    msg.m_bHasMixer = status.m_bHasMixer;
    msg.m_fPosition = status.m_fPosition;
    msg.m_fPeak = status.m_fPeak;
    msg.m_InstrumentRangeGains = status.m_InstrumentRangeGains;

    msg.m_InstrumentSounding.SetCount(status.m_InstrumentSounding.GetCount());
    for (plUInt32 i = 0; i < status.m_InstrumentSounding.GetCount(); ++i)
    {
      msg.m_InstrumentSounding[i] = status.m_InstrumentSounding[i] ? 1 : 0;
    }
  }

  SendProcessMessage(&msg);
}
