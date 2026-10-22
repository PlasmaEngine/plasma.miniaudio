#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <AudioSystem/Core/AudioMiddleware.h>
#include <AudioSystem/Core/AudioSystem.h>
#include <AudioSystem/Core/AudioSystemRequests.h>
#include <AudioSystem/Mixer/AudioMixerService.h>
#include <Foundation/Threading/Lock.h>
#include <PlasmaAudioPlugin/Events/AudioAudition.h>
#include <PlasmaAudioPlugin/Events/MiniAudioAudioEvents.h>
#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioPlugin/Mixer/MiniAudioBusGraph.h>

namespace
{
  /// How long an audition waits for the mixer it requested before it plays without it.
  constexpr plTime s_MixerWait = plTime::MakeFromSeconds(3.0);
} // namespace

struct plAudioAudition::Command
{
  enum class Type : plUInt8
  {
    Play,
    Stop,
    SetParameters,
    KeyOff,
    Remove
  };

  Type m_Type = Type::Play;
  plUInt64 m_uiKey = 0;
  plAudioAuditionRequest m_Request;
  plDynamicArray<plString> m_ParameterNames;
  plDynamicArray<float> m_ParameterValues;
};

struct plAudioAudition::Slot
{
  explicit Slot(plUInt64 uiEntityId)
    : m_Entity(uiEntityId, true)
  {
  }

  plPlasmaAudioEntityData m_Entity;
  plPlasmaAudioEventData m_EventData;

  plAudioAuditionRequest m_Request;
  bool m_bPendingStart = false; ///< waits for the requested mixer
  plTime m_MixerDeadline;
};

plAudioAudition* plAudioAudition::s_pSingleton = nullptr;

plAudioAudition::plAudioAudition() = default;
plAudioAudition::~plAudioAudition() = default;

void plAudioAudition::Play(plUInt64 uiKey, const plAudioAuditionRequest& request)
{
  PL_LOCK(m_Mutex);

  Command& cmd = m_Commands.ExpandAndGetRef();
  cmd.m_Type = Command::Type::Play;
  cmd.m_uiKey = uiKey;
  cmd.m_Request = request;

  // until the audio update ran, the audition counts as waiting to start
  plAudioAuditionStatus& status = m_Status[uiKey];
  status.m_bPlaying = true;
  status.m_bWaiting = true;
  status.m_fPosition = request.m_StartOffset.GetSeconds();
}

void plAudioAudition::Stop(plUInt64 uiKey)
{
  PL_LOCK(m_Mutex);

  Command& cmd = m_Commands.ExpandAndGetRef();
  cmd.m_Type = Command::Type::Stop;
  cmd.m_uiKey = uiKey;
}

void plAudioAudition::SetParameters(plUInt64 uiKey, plArrayPtr<const plString> names, plArrayPtr<const float> values)
{
  PL_LOCK(m_Mutex);

  Command& cmd = m_Commands.ExpandAndGetRef();
  cmd.m_Type = Command::Type::SetParameters;
  cmd.m_uiKey = uiKey;
  cmd.m_ParameterNames = names;
  cmd.m_ParameterValues = values;
}

void plAudioAudition::KeyOff(plUInt64 uiKey)
{
  PL_LOCK(m_Mutex);

  Command& cmd = m_Commands.ExpandAndGetRef();
  cmd.m_Type = Command::Type::KeyOff;
  cmd.m_uiKey = uiKey;
}

void plAudioAudition::TakeMarkers(plUInt64 uiKey, plDynamicArray<plString>& out_markers)
{
  PL_LOCK(m_Mutex);

  plAudioAuditionStatus* pStatus = m_Status.GetValue(uiKey);
  if (pStatus == nullptr)
    return;

  for (plString& sMarker : pStatus->m_Markers)
  {
    out_markers.PushBack(std::move(sMarker));
  }

  pStatus->m_Markers.Clear();
}

void plAudioAudition::Remove(plUInt64 uiKey)
{
  PL_LOCK(m_Mutex);

  Command& cmd = m_Commands.ExpandAndGetRef();
  cmd.m_Type = Command::Type::Remove;
  cmd.m_uiKey = uiKey;

  m_Status.Remove(uiKey);
}

bool plAudioAudition::GetStatus(plUInt64 uiKey, plAudioAuditionStatus& out_status) const
{
  PL_LOCK(m_Mutex);

  const plAudioAuditionStatus* pStatus = nullptr;
  if (!m_Status.TryGetValue(uiKey, pStatus))
    return false;

  out_status = *pStatus;
  return true;
}

void plAudioAudition::SetMixerMonitor(bool bWatch)
{
  PL_LOCK(m_Mutex);

  if (bWatch)
  {
    ++m_uiMixerMonitors;
  }
  else if (m_uiMixerMonitors > 0)
  {
    --m_uiMixerMonitors;
  }
}

void plAudioAudition::GetMixerMonitor(plAudioMixerMonitorStatus& out_status) const
{
  PL_LOCK(m_Mutex);
  out_status = m_MixerMonitor;
}

void plAudioAudition::SetTestSignal(bool bEnable)
{
  PL_LOCK(m_Mutex);
  m_iTestSignalRequest = bEnable ? 1 : 0;
}

void plAudioAudition::SetBankMonitor(bool bWatch)
{
  PL_LOCK(m_Mutex);

  if (bWatch)
  {
    ++m_uiBankMonitors;
  }
  else if (m_uiBankMonitors > 0)
  {
    --m_uiBankMonitors;
  }
}

void plAudioAudition::GetBankMonitor(plAudioBankMonitorStatus& out_status) const
{
  PL_LOCK(m_Mutex);
  out_status = m_BankMonitor;
}

bool plAudioAudition::IsBankMonitorWatched() const
{
  PL_LOCK(m_Mutex);
  return m_uiBankMonitors > 0;
}

void plAudioAudition::SetBankMonitorStatus(plAudioBankMonitorStatus&& status)
{
  PL_LOCK(m_Mutex);
  status.m_uiRefreshCount = m_BankMonitor.m_uiRefreshCount + 1;
  m_BankMonitor = std::move(status);
}

void plAudioAudition::UpdateMixerMonitor(const plMiniAudioBusGraph& busGraph, const plPlasmaAudioEventManager& events)
{
  plAudioMixerMonitorStatus& status = m_MixerMonitor;

  ++status.m_uiRefreshCount;
  status.m_bHasMixer = busGraph.IsBuilt();
  status.m_bTestSignal = busGraph.IsTestSignalOn();
  status.m_uiRealVoices = events.GetRealVoiceCount();

  const plAudioMixerDesc& desc = busGraph.GetDesc();
  const plUInt32 uiBusCount = busGraph.IsBuilt() ? desc.m_Buses.GetCount() : 0;

  status.m_BusNames.SetCount(uiBusCount);
  status.m_BusPeaks.SetCount(uiBusCount);
  status.m_BusRms.SetCount(uiBusCount);
  status.m_BusMuted.SetCount(uiBusCount);
  status.m_BusSoloed.SetCount(uiBusCount);
  status.m_EffectGainReductions.Clear();

  const plAudioMixerService* pService = plAudioSystem::GetSingleton() != nullptr ? &plAudioSystem::GetSingleton()->GetMixerService() : nullptr;

  for (plUInt32 i = 0; i < uiBusCount; ++i)
  {
    status.m_BusNames[i] = desc.m_Buses[i].m_sName;
    busGraph.GetBusMeter(i, status.m_BusPeaks[i], status.m_BusRms[i]);
    status.m_BusMuted[i] = pService != nullptr && pService->IsBusMuted(i);
    status.m_BusSoloed[i] = pService != nullptr && pService->IsBusSoloed(i);

    for (plUInt32 e = 0; e < desc.m_Buses[i].m_Effects.GetCount(); ++e)
    {
      status.m_EffectGainReductions.PushBack(busGraph.GetEffectGainReductionDb(i, e));
    }
  }

  status.m_sMixerResourceId.Clear();
  status.m_uiLayoutRebuilds = 0;
  status.m_SnapshotNames.Clear();
  status.m_SnapshotIntensities.Clear();

  if (pService != nullptr && pService->IsLoaded())
  {
    status.m_sMixerResourceId = pService->GetMixerResourceId();
    status.m_uiLayoutRebuilds = pService->GetLayoutRebuildCount();

    const plAudioMixerDesc& serviceDesc = pService->GetDesc();
    for (plUInt32 s = 0; s < serviceDesc.m_Snapshots.GetCount(); ++s)
    {
      status.m_SnapshotNames.PushBack(serviceDesc.m_Snapshots[s].m_sName);
      status.m_SnapshotIntensities.PushBack(pService->GetSnapshotIntensity(s));
    }
  }
}

void plAudioAudition::Activate(Slot& ref_slot, plPlasmaAudioEventManager& ref_events, plMiniAudioBusGraph& ref_busGraph, ma_engine* pEngine)
{
  ref_slot.m_bPendingStart = false;

  ref_slot.m_EventData.m_bSeeded = ref_slot.m_Request.m_bSeeded;
  ref_slot.m_EventData.m_uiSeed = ref_slot.m_Request.m_uiSeed;
  ref_slot.m_EventData.m_StartOffset = ref_slot.m_Request.m_StartOffset;
  ref_slot.m_Entity.m_fAuditionDistance = ref_slot.m_Request.m_fDistance;

  ref_slot.m_Entity.m_SwitchStates.Clear();
  for (const plString& sState : ref_slot.m_Request.m_SwitchStates)
  {
    const char* szSeparator = sState.FindLastSubString("/");
    if (szSeparator != nullptr)
    {
      ref_slot.m_Entity.SetSwitchState(plStringView(sState.GetData(), szSeparator), sState);
    }
  }

  if (ref_events.ActivateEvent(ref_slot.m_Request.m_sEventResourceId, &ref_slot.m_Entity, &ref_slot.m_EventData, &ref_busGraph, pEngine).Failed())
  {
    plLog::Warning("PlasmaAudio: the audition of '{}' could not start", ref_slot.m_Request.m_sEventResourceId);
  }
}

void plAudioAudition::Update(plPlasmaAudioEventManager& ref_events, plMiniAudioBusGraph& ref_busGraph, ma_engine* pEngine, float fMasterPeak)
{
  plDynamicArray<Command> commands;
  plInt8 iTestSignal = -1;
  {
    PL_LOCK(m_Mutex);
    commands.Swap(m_Commands);
    iTestSignal = m_iTestSignalRequest;
    m_iTestSignalRequest = -1;
  }

  if (iTestSignal >= 0)
  {
    ref_busGraph.SetTestSignal(iTestSignal != 0);
  }

  const plTime tNow = plTime::Now();

  for (Command& cmd : commands)
  {
    plUniquePtr<Slot>* pSlot = m_Slots.GetValue(cmd.m_uiKey);

    switch (cmd.m_Type)
    {
      case Command::Type::Play:
      {
        if (pSlot == nullptr)
        {
          m_Slots.Insert(cmd.m_uiKey, PL_DEFAULT_NEW(Slot, m_uiNextEntityId++));
          pSlot = m_Slots.GetValue(cmd.m_uiKey);
        }

        Slot& slot = **pSlot;

        // the previous audition ends right away and keeps playing nothing of its own
        ref_events.StopEvent(&slot.m_EventData, true).IgnoreResult();
        ref_events.DetachEventData(&slot.m_EventData);

        slot.m_Request = std::move(cmd.m_Request);

        // loading a mixer stops everything that plays, so the event starts once the mixer is built
        if (!ref_busGraph.IsBuilt() && !slot.m_Request.m_sMixerResourceId.IsEmpty())
        {
          if (plAudioSystem* pAudioSystem = plAudioSystem::GetSingleton())
          {
            plAudioSystemRequestSetMixerControl request;
            request.m_uiOp = plAudioMixerControlOp::LoadMixer;
            request.m_sTarget = slot.m_Request.m_sMixerResourceId;
            pAudioSystem->SendRequest(request);
          }

          slot.m_bPendingStart = true;
          slot.m_MixerDeadline = tNow + s_MixerWait;
        }
        else
        {
          Activate(slot, ref_events, ref_busGraph, pEngine);
        }
        break;
      }

      case Command::Type::Stop:
        if (pSlot != nullptr)
        {
          (*pSlot)->m_bPendingStart = false;
          ref_events.StopEvent(&(*pSlot)->m_EventData, false).IgnoreResult();
        }
        break;

      case Command::Type::SetParameters:
        if (pSlot != nullptr)
        {
          for (plUInt32 i = 0; i < cmd.m_ParameterNames.GetCount() && i < cmd.m_ParameterValues.GetCount(); ++i)
          {
            // the built-in Distance follows the audition's distance
            if (cmd.m_ParameterNames[i].IsEqual_NoCase("Distance"))
            {
              (*pSlot)->m_Request.m_fDistance = cmd.m_ParameterValues[i];
              (*pSlot)->m_Entity.m_fAuditionDistance = cmd.m_ParameterValues[i];
              continue;
            }

            // music: a state or a stinger by name, as the game sends them
            plStringView sName = cmd.m_ParameterNames[i];
            if (sName.StartsWith(plAudioSystemReservedParameter::MusicStatePrefix))
            {
              sName.Shrink(plStringUtils::GetCharacterCount(plAudioSystemReservedParameter::MusicStatePrefix), 0);
              ref_events.SetMusicState(&(*pSlot)->m_Entity, sName);
              continue;
            }

            if (sName.StartsWith(plAudioSystemReservedParameter::StingerPrefix))
            {
              sName.Shrink(plStringUtils::GetCharacterCount(plAudioSystemReservedParameter::StingerPrefix), 0);
              ref_events.PlayStinger(&(*pSlot)->m_Entity, sName);
              continue;
            }

            ref_events.SetEventParameter(&(*pSlot)->m_Entity, cmd.m_ParameterNames[i], cmd.m_ParameterValues[i]);
          }
        }
        break;

      case Command::Type::KeyOff:
        if (pSlot != nullptr)
        {
          ref_events.KeyOff(&(*pSlot)->m_Entity);
        }
        break;

      case Command::Type::Remove:
        if (pSlot != nullptr)
        {
          // severs the instances' links to the slot's entity and event data, which are deleted with the slot
          ref_events.StopAllOnEntity(&(*pSlot)->m_Entity, true);
          ref_events.DetachEventData(&(*pSlot)->m_EventData);
          m_Slots.Remove(cmd.m_uiKey);
        }
        break;
    }
  }

  // auditions waiting for their mixer start once it is built, or without it after a while
  for (auto it = m_Slots.GetIterator(); it.IsValid(); ++it)
  {
    Slot& slot = *it.Value();
    if (slot.m_bPendingStart && (ref_busGraph.IsBuilt() || tNow >= slot.m_MixerDeadline))
    {
      Activate(slot, ref_events, ref_busGraph, pEngine);
    }
  }

  // the status of every audition
  PL_LOCK(m_Mutex);

  if (m_uiMixerMonitors > 0)
  {
    UpdateMixerMonitor(ref_busGraph, ref_events);
  }

  for (auto it = m_Slots.GetIterator(); it.IsValid(); ++it)
  {
    Slot& slot = *it.Value();

    // a key removed since the commands were taken has no status any more
    plAudioAuditionStatus* pStatus = m_Status.GetValue(it.Key());
    if (pStatus == nullptr)
      continue;

    // the markers wait for the editor to take them; an editor that stopped asking does not grow the list forever
    ref_events.TakeMarkers(&slot.m_EventData, pStatus->m_Markers);
    if (pStatus->m_Markers.GetCount() > MaxQueuedMarkers)
    {
      pStatus->m_Markers.RemoveAtAndCopy(0, pStatus->m_Markers.GetCount() - MaxQueuedMarkers);
    }

    // a play requested since the commands were taken keeps its waiting state until the next update
    bool bQueued = false;
    for (const Command& cmd : m_Commands)
    {
      bQueued |= cmd.m_uiKey == it.Key() && cmd.m_Type == Command::Type::Play;
    }
    if (bQueued)
      continue;

    bool bWaiting = false;
    pStatus->m_bPlaying = ref_events.GetEventState(&slot.m_EventData, bWaiting, pStatus->m_fPosition, pStatus->m_InstrumentSounding, pStatus->m_InstrumentRangeGains);
    ref_events.GetSustainState(&slot.m_EventData, pStatus->m_uiSustainsLeft, pStatus->m_bHolding);
    pStatus->m_bWaiting = bWaiting || slot.m_bPendingStart;
    pStatus->m_bPlaying |= slot.m_bPendingStart;
    pStatus->m_bHasMixer = ref_busGraph.IsBuilt();
    pStatus->m_fPeak = fMasterPeak;

    // music: the position, and how far off a decided change is
    pStatus->m_bMusic = ref_events.GetMusicPosition(&slot.m_EventData, pStatus->m_Music);
    pStatus->m_fMusicPendingIn = -1.0;
    if (pStatus->m_bMusic && pStatus->m_Music.m_uiPendingFrame != 0 && pEngine != nullptr)
    {
      const ma_uint64 uiNow = ma_engine_get_time_in_pcm_frames(pEngine);
      const ma_uint64 uiAhead = (pStatus->m_Music.m_uiPendingFrame > uiNow) ? pStatus->m_Music.m_uiPendingFrame - uiNow : 0;
      pStatus->m_fMusicPendingIn = static_cast<double>(uiAhead) / plMath::Max(ma_engine_get_sample_rate(pEngine), 1u);
    }
  }
}

void plAudioAudition::StopAll(plPlasmaAudioEventManager& ref_events)
{
  for (auto it = m_Slots.GetIterator(); it.IsValid(); ++it)
  {
    ref_events.StopAllOnEntity(&it.Value()->m_Entity, true);
    ref_events.DetachEventData(&it.Value()->m_EventData);
  }

  m_Slots.Clear();

  PL_LOCK(m_Mutex);
  m_Commands.Clear();
  m_Status.Clear();
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Events_AudioAudition);
