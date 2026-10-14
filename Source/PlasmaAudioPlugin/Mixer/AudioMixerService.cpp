#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Mixer/AudioMixerService.h>
#include <PlasmaAudioPlugin/Mixer/MiniAudioBusGraph.h>

void plAudioMixerService::InitFromDesc(const plAudioMixerResourceHandle& hResource, const plAudioMixerDesc& desc)
{
  m_hResource = hResource;
  m_Desc = desc;
  ResetRuntimeState();
  m_bLoaded = true;
}

void plAudioMixerService::UnloadMixer(plMiniAudioBusGraph* pGraph)
{
  PL_IGNORE_UNUSED(pGraph);

  if (!m_bLoaded)
    return;

  m_bLoaded = false;
  m_hResource.Invalidate();
  m_Desc = plAudioMixerDesc();
  ResetRuntimeState();
}

void plAudioMixerService::ResetRuntimeState()
{
  m_BusStates.Clear();
  m_BusStates.SetCount(m_Desc.m_Buses.GetCount());

  for (plUInt32 i = 0; i < m_Desc.m_Buses.GetCount(); ++i)
  {
    m_BusStates[i].m_fUserVolumeDb = m_Desc.m_Buses[i].m_fVolumeDb;
    m_BusStates[i].m_LastSentSends.SetCount(m_Desc.m_Buses[i].m_Sends.GetCount());
    for (float& f : m_BusStates[i].m_LastSentSends)
      f = -1.0f;
    m_BusStates[i].m_LastSentEffectParams.SetCount(m_Desc.m_Buses[i].m_Effects.GetCount() * plAudioEffectMaxParams);
    for (float& f : m_BusStates[i].m_LastSentEffectParams)
      f = plMath::MaxValue<float>();
  }

  m_VcaStates.Clear();
  m_VcaStates.SetCount(m_Desc.m_Vcas.GetCount());
  for (plUInt32 i = 0; i < m_Desc.m_Vcas.GetCount(); ++i)
  {
    m_VcaStates[i].m_fUserVolumeDb = m_Desc.m_Vcas[i].m_fVolumeDb;
  }

  m_SnapshotStates.Clear();
  m_SnapshotStates.SetCount(m_Desc.m_Snapshots.GetCount());

  m_bDirty = true;
}

void plAudioMixerService::SetBusVolume(plStringView sBusName, float fVolumeDb)
{
  const plUInt32 uiBus = m_Desc.FindBusByName(sBusName);
  if (uiBus == plInvalidIndex)
    return;

  m_BusStates[uiBus].m_fUserVolumeDb = fVolumeDb;
  m_bDirty = true;
}

void plAudioMixerService::SetVcaVolume(plStringView sVcaName, float fVolumeDb)
{
  const plUInt32 uiVca = m_Desc.FindVcaByName(sVcaName);
  if (uiVca == plInvalidIndex)
    return;

  m_VcaStates[uiVca].m_fUserVolumeDb = fVolumeDb;
  m_bDirty = true;
}

void plAudioMixerService::SetSnapshotIntensity(plStringView sSnapshotName, float fIntensity)
{
  const plUInt32 uiSnapshot = m_Desc.FindSnapshotByName(sSnapshotName);
  if (uiSnapshot == plInvalidIndex)
  {
    plLog::Warning("Audio snapshot '{0}' does not exist in the loaded mixer.", sSnapshotName);
    return;
  }

  m_SnapshotStates[uiSnapshot].m_fTarget = plMath::Clamp(fIntensity, 0.0f, 1.0f);
}

void plAudioMixerService::StopAllSnapshots()
{
  for (auto& state : m_SnapshotStates)
  {
    state.m_fCurrent = 0.0f;
    state.m_fTarget = 0.0f;
  }

  m_bDirty = true;
}

bool plAudioMixerService::SetGroupVolume(plStringView sGroupName, float fLinearVolume)
{
  const float fDb = plAudioMixerUtils::LinearToDb(fLinearVolume);

  if (m_Desc.FindBusByName(sGroupName) != plInvalidIndex)
  {
    SetBusVolume(sGroupName, fDb);
    return true;
  }

  if (m_Desc.FindVcaByName(sGroupName) != plInvalidIndex)
  {
    SetVcaVolume(sGroupName, fDb);
    return true;
  }

  return false;
}

float plAudioMixerService::GetGroupVolume(plStringView sGroupName) const
{
  if (const plUInt32 uiBus = m_Desc.FindBusByName(sGroupName); uiBus != plInvalidIndex)
    return plAudioMixerUtils::DbToLinear(m_BusStates[uiBus].m_fUserVolumeDb);

  if (const plUInt32 uiVca = m_Desc.FindVcaByName(sGroupName); uiVca != plInvalidIndex)
    return plAudioMixerUtils::DbToLinear(m_VcaStates[uiVca].m_fUserVolumeDb);

  return 1.0f;
}

void plAudioMixerService::Update(plTime delta, plMiniAudioBusGraph* pGraph)
{
  if (!m_bLoaded || pGraph == nullptr || !pGraph->IsBuilt())
    return;

  // advance snapshot fades
  bool bFading = false;

  for (plUInt32 i = 0; i < m_SnapshotStates.GetCount(); ++i)
  {
    auto& state = m_SnapshotStates[i];

    if (state.m_fCurrent == state.m_fTarget)
      continue;

    const auto& desc = m_Desc.m_Snapshots[i];
    const plTime fadeTime = (state.m_fTarget > state.m_fCurrent) ? desc.m_FadeIn : desc.m_FadeOut;

    if (fadeTime <= plTime::MakeZero())
    {
      state.m_fCurrent = state.m_fTarget;
    }
    else
    {
      const float fStep = static_cast<float>(delta.GetSeconds() / fadeTime.GetSeconds());
      state.m_fCurrent = (state.m_fTarget > state.m_fCurrent)
                           ? plMath::Min(state.m_fCurrent + fStep, state.m_fTarget)
                           : plMath::Max(state.m_fCurrent - fStep, state.m_fTarget);
    }

    bFading = true;
  }

  if (!m_bDirty && !bFading)
    return;

  m_bDirty = false;

  PushValues(pGraph, false);
}

void plAudioMixerService::PushValues(plMiniAudioBusGraph* pGraph, bool bForce)
{
  if (!m_bLoaded || pGraph == nullptr || !pGraph->IsBuilt())
    return;

  // snapshots sorted by priority: lower priority first, higher priority last (wins)
  plHybridArray<plUInt32, 8> activeSnapshots;
  for (plUInt32 i = 0; i < m_SnapshotStates.GetCount(); ++i)
  {
    if (m_SnapshotStates[i].m_fCurrent > 0.0f)
      activeSnapshots.PushBack(i);
  }

  activeSnapshots.Sort([this](plUInt32 lhs, plUInt32 rhs)
    { return m_Desc.m_Snapshots[lhs].m_uiPriority < m_Desc.m_Snapshots[rhs].m_uiPriority; });

  for (plUInt32 uiBus = 0; uiBus < m_Desc.m_Buses.GetCount(); ++uiBus)
  {
    const auto& busDesc = m_Desc.m_Buses[uiBus];
    auto& busState = m_BusStates[uiBus];

    float fVolumeDb = busState.m_fUserVolumeDb;
    float fPitch = busDesc.m_fPitch;

    plHybridArray<float, 2> sendLevelsDb;
    sendLevelsDb.SetCount(busDesc.m_Sends.GetCount());
    for (plUInt32 s = 0; s < busDesc.m_Sends.GetCount(); ++s)
      sendLevelsDb[s] = busDesc.m_Sends[s].m_fLevelDb;

    plHybridArray<float, 8> effectParams;
    effectParams.SetCount(busDesc.m_Effects.GetCount() * plAudioEffectMaxParams);
    for (plUInt32 e = 0; e < busDesc.m_Effects.GetCount(); ++e)
    {
      for (plUInt32 p = 0; p < plAudioEffectMaxParams; ++p)
        effectParams[e * plAudioEffectMaxParams + p] = busDesc.m_Effects[e].m_fParams[p];
    }

    // apply snapshot overrides (priority order, blended by intensity)
    for (plUInt32 uiSnapshot : activeSnapshots)
    {
      const float fIntensity = m_SnapshotStates[uiSnapshot].m_fCurrent;

      for (const auto& ovr : m_Desc.m_Snapshots[uiSnapshot].m_Overrides)
      {
        if (ovr.m_uiBus != uiBus)
          continue;

        switch (ovr.m_eTarget)
        {
          case plAudioSnapshotTarget::BusVolume:
            fVolumeDb = plMath::Lerp(fVolumeDb, ovr.m_fValue, fIntensity);
            break;
          case plAudioSnapshotTarget::BusPitch:
            fPitch = plMath::Lerp(fPitch, ovr.m_fValue, fIntensity);
            break;
          case plAudioSnapshotTarget::SendLevel:
            if (ovr.m_uiIndexA < sendLevelsDb.GetCount())
              sendLevelsDb[ovr.m_uiIndexA] = plMath::Lerp(sendLevelsDb[ovr.m_uiIndexA], ovr.m_fValue, fIntensity);
            break;
          case plAudioSnapshotTarget::EffectParam:
          {
            const plUInt32 uiSlot = ovr.m_uiIndexA * plAudioEffectMaxParams + ovr.m_uiIndexB;
            if (uiSlot < effectParams.GetCount())
              effectParams[uiSlot] = plMath::Lerp(effectParams[uiSlot], ovr.m_fValue, fIntensity);
            break;
          }
        }
      }
    }

    // apply VCAs (pure gain on top)
    float fGain = plAudioMixerUtils::DbToLinear(fVolumeDb);

    for (plUInt32 uiVca = 0; uiVca < m_Desc.m_Vcas.GetCount(); ++uiVca)
    {
      if (m_Desc.m_Vcas[uiVca].m_Buses.Contains(static_cast<plUInt8>(uiBus)))
      {
        fGain *= plAudioMixerUtils::DbToLinear(m_VcaStates[uiVca].m_fUserVolumeDb);
      }
    }

    // push changed values only
    if (bForce || plMath::Abs(busState.m_fLastSentGain - fGain) > 0.0001f)
    {
      busState.m_fLastSentGain = fGain;
      pGraph->SetBusVolume(uiBus, fGain);
    }

    if (bForce || plMath::Abs(busState.m_fLastSentPitch - fPitch) > 0.0001f)
    {
      busState.m_fLastSentPitch = fPitch;
      pGraph->SetBusPitch(uiBus, fPitch);
    }

    for (plUInt32 s = 0; s < sendLevelsDb.GetCount(); ++s)
    {
      const float fLevel = plAudioMixerUtils::DbToLinear(sendLevelsDb[s]);
      if (bForce || plMath::Abs(busState.m_LastSentSends[s] - fLevel) > 0.0001f)
      {
        busState.m_LastSentSends[s] = fLevel;
        pGraph->SetBusSendLevel(uiBus, s, fLevel);
      }
    }

    for (plUInt32 e = 0; e < busDesc.m_Effects.GetCount(); ++e)
    {
      for (plUInt32 p = 0; p < plAudioEffectMaxParams; ++p)
      {
        const plUInt32 uiSlot = e * plAudioEffectMaxParams + p;
        const float fValue = effectParams[uiSlot];

        if (bForce || plMath::Abs(busState.m_LastSentEffectParams[uiSlot] - fValue) > 0.0001f)
        {
          busState.m_LastSentEffectParams[uiSlot] = fValue;
          pGraph->SetBusEffectParameter(uiBus, e, p, fValue);
        }
      }
    }
  }
}

float plAudioMixerService::GetEffectiveBusGain(plUInt32 uiBus) const
{
  if (!m_bLoaded)
    return 1.0f;

  float fGain = 1.0f;

  // the depth bound guards against a cyclic parent chain in malformed data
  for (plUInt32 uiDepth = 0; uiBus < m_BusStates.GetCount() && uiDepth < m_BusStates.GetCount(); ++uiDepth)
  {
    const float fBusGain = m_BusStates[uiBus].m_fLastSentGain;
    if (fBusGain >= 0.0f)
    {
      fGain *= fBusGain;
    }

    uiBus = m_Desc.m_Buses[uiBus].m_uiParent;
  }

  return fGain;
}

float plAudioMixerService::GetBusGain(plUInt32 uiBus) const
{
  if (!m_bLoaded || uiBus >= m_BusStates.GetCount() || m_BusStates[uiBus].m_fLastSentGain < 0.0f)
    return 1.0f;

  return m_BusStates[uiBus].m_fLastSentGain;
}

float plAudioMixerService::GetSnapshotIntensity(plUInt32 uiSnapshot) const
{
  if (!m_bLoaded || uiSnapshot >= m_SnapshotStates.GetCount())
    return 0.0f;

  return m_SnapshotStates[uiSnapshot].m_fCurrent;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Mixer_AudioMixerService);
