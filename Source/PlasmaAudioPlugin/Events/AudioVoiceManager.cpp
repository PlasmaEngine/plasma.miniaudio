#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <PlasmaAudioPlugin/Events/AudioVoiceManager.h>
#include <PlasmaAudioPlugin/Events/MiniAudioAudioEvents.h>

plCVarInt cvar_PlasmaAudioMaxRealVoices("PlasmaAudio.MaxRealVoices", 64, plCVarFlags::Default, "Number of event voices that are decoded and mixed at once; the others play virtually");
plCVarInt cvar_PlasmaAudioMaxVirtualVoices("PlasmaAudio.MaxVirtualVoices", 512, plCVarFlags::Default, "Number of virtual event voices; past it the least important non-looping virtual voices are stopped");
plCVarFloat cvar_PlasmaAudioVoiceHysteresis("PlasmaAudio.VoiceHysteresis", 1.25f, plCVarFlags::Default, "Audibility factor a real voice gets in the voice ranking, so voices of similar loudness do not swap every update");
plCVarInt cvar_PlasmaAudioVoiceMinDwellMs("PlasmaAudio.VoiceMinDwellMs", 100, plCVarFlags::Default, "Minimum time in ms a voice stays real or virtual before the voice ranking may change it again");
plCVarFloat cvar_PlasmaAudioVoiceAudibilityFloor("PlasmaAudio.VoiceAudibilityFloor", 0.001f, plCVarFlags::Default, "Voices quieter than this linear audibility are always virtual");

namespace
{
  /// budget overrides plus one; 0 selects the cvar value
  plAtomicInteger32 s_iMaxRealOverride;
  plAtomicInteger32 s_iMaxVirtualOverride;
} // namespace

void plAudioVoiceManager::SetBudgets(plInt32 iMaxRealVoices, plInt32 iMaxVirtualVoices)
{
  s_iMaxRealOverride.Set(iMaxRealVoices < 0 ? 0 : iMaxRealVoices + 1);
  s_iMaxVirtualOverride.Set(iMaxVirtualVoices < 0 ? 0 : iMaxVirtualVoices + 1);
}

plUInt32 plAudioVoiceManager::GetMaxRealVoices()
{
  const plInt32 iOverride = s_iMaxRealOverride;
  if (iOverride > 0)
    return static_cast<plUInt32>(iOverride - 1);

  return static_cast<plUInt32>(plMath::Max(cvar_PlasmaAudioMaxRealVoices.GetValue(), 0));
}

plUInt32 plAudioVoiceManager::GetMaxVirtualVoices()
{
  const plInt32 iOverride = s_iMaxVirtualOverride;
  if (iOverride > 0)
    return static_cast<plUInt32>(iOverride - 1);

  return static_cast<plUInt32>(plMath::Max(cvar_PlasmaAudioMaxVirtualVoices.GetValue(), 0));
}

float plAudioVoiceManager::GetAudibilityFloor()
{
  return plMath::Max(cvar_PlasmaAudioVoiceAudibilityFloor.GetValue(), 0.0f);
}

void plAudioVoiceManager::Reset()
{
  m_Ranked.Clear();
  m_uiRealCount = 0;
}

void plAudioVoiceManager::ForgetInstance(const plPlasmaAudioEventInstance* pInstance)
{
  for (Candidate& candidate : m_Ranked)
  {
    if (candidate.m_pInstance == pInstance)
    {
      candidate.m_pInstance = nullptr;
      candidate.m_bReal = false;
    }
  }
}

bool plAudioVoiceManager::IsCandidateValid(const Candidate& candidate)
{
  const plPlasmaAudioEventInstance* pInstance = candidate.m_pInstance;
  return pInstance != nullptr && !pInstance->IsStopping() && pInstance->IsVoiceActive(candidate.m_uiVoice) && pInstance->IsVoiceVirtual(candidate.m_uiVoice) != candidate.m_bReal;
}

void plAudioVoiceManager::Update(plArrayPtr<const plUniquePtr<plPlasmaAudioEventInstance>> instances, ma_uint64 uiNow, ma_uint32 uiSampleRate)
{
  PL_PROFILE_SCOPE("PlasmaAudio.VoiceRanking");

  m_Ranked.Clear();

  const plUInt32 uiMaxReal = GetMaxRealVoices();
  const plUInt32 uiMaxVirtual = GetMaxVirtualVoices();
  const float fFloor = GetAudibilityFloor();
  const float fHysteresis = plMath::Max(cvar_PlasmaAudioVoiceHysteresis.GetValue(), 1.0f);
  const ma_uint64 uiDwellFrames = static_cast<ma_uint64>(plMath::Max(cvar_PlasmaAudioVoiceMinDwellMs.GetValue(), 0)) * uiSampleRate / 1000;

  // real voices of stopping instances keep playing their fade and are not ranked
  plUInt32 uiStoppingReal = 0;

  for (const auto& pInstance : instances)
  {
    if (pInstance->GetResource() == nullptr)
      continue;

    const bool bStopping = pInstance->IsStopping();

    for (plUInt32 uiVoice = 0; uiVoice < pInstance->GetVoiceSlotCount(); ++uiVoice)
    {
      if (!pInstance->IsVoiceActive(uiVoice))
        continue;

      const bool bReal = !pInstance->IsVoiceVirtual(uiVoice);

      if (bStopping)
      {
        uiStoppingReal += bReal ? 1 : 0;
        continue;
      }

      Candidate& candidate = m_Ranked.ExpandAndGetRef();
      candidate.m_pInstance = pInstance.Borrow();
      candidate.m_uiInstanceStartFrame = pInstance->GetStartFrame();
      candidate.m_uiInstanceSerial = pInstance->m_uiSerial;
      candidate.m_fAudibility = pInstance->GetAudibility();
      candidate.m_fScore = bReal ? candidate.m_fAudibility * fHysteresis : candidate.m_fAudibility;
      candidate.m_uiVoice = static_cast<plUInt16>(uiVoice);
      candidate.m_uiPriority = pInstance->GetPriority();
      candidate.m_bReal = bReal;
      candidate.m_bLooping = pInstance->IsVoiceLooping(uiVoice);
    }
  }

  // a total order, so the ranking does not depend on the sort algorithm
  m_Ranked.Sort([](const Candidate& a, const Candidate& b)
    {
      if (a.m_uiPriority != b.m_uiPriority)
        return a.m_uiPriority > b.m_uiPriority;
      if (a.m_fScore != b.m_fScore)
        return a.m_fScore > b.m_fScore;
      if (a.m_uiInstanceStartFrame != b.m_uiInstanceStartFrame)
        return a.m_uiInstanceStartFrame < b.m_uiInstanceStartFrame;
      if (a.m_uiInstanceSerial != b.m_uiInstanceSerial)
        return a.m_uiInstanceSerial < b.m_uiInstanceSerial;
      return a.m_uiVoice < b.m_uiVoice;
    });

  // Per-bus voice limits would be applied here: a voice whose output bus is at its limit does not want to be real.
  const plUInt32 uiAvailable = (uiMaxReal > uiStoppingReal) ? (uiMaxReal - uiStoppingReal) : 0;
  plUInt32 uiWanted = 0;

  for (Candidate& candidate : m_Ranked)
  {
    candidate.m_bWantReal = uiWanted < uiAvailable && candidate.m_fAudibility >= fFloor;
    uiWanted += candidate.m_bWantReal ? 1 : 0;
  }

  // virtualize real voices that ranked out, once they dwelled long enough
  for (Candidate& candidate : m_Ranked)
  {
    if (!candidate.m_bReal || candidate.m_bWantReal || !IsCandidateValid(candidate))
      continue;

    if (!candidate.m_pInstance->CanVoiceFlip(candidate.m_uiVoice, uiNow, uiDwellFrames))
      continue;

    candidate.m_pInstance->VirtualizeVoice(candidate.m_uiVoice);
    candidate.m_bReal = false;
    ++m_uiFlipCount;
  }

  plUInt32 uiRealCount = uiStoppingReal;
  for (const Candidate& candidate : m_Ranked)
  {
    if (candidate.m_bReal && IsCandidateValid(candidate))
      ++uiRealCount;
  }

  // realize virtual voices that ranked in, best first, while real voices are left
  for (Candidate& candidate : m_Ranked)
  {
    if (uiRealCount >= uiMaxReal)
      break;

    if (candidate.m_bReal || !candidate.m_bWantReal || !IsCandidateValid(candidate))
      continue;

    if (!candidate.m_pInstance->CanVoiceFlip(candidate.m_uiVoice, uiNow, uiDwellFrames))
      continue;

    // a voice whose clip is not loaded or that gets no stream stays virtual and is tried again next update
    if (candidate.m_pInstance->RealizeVoice(candidate.m_uiVoice))
    {
      candidate.m_bReal = true;
      ++uiRealCount;
      ++m_uiFlipCount;
    }
  }

  // the budget is a hard limit: voices held real by the dwell time are virtualized when it is exceeded
  for (plUInt32 i = m_Ranked.GetCount(); i > 0 && uiRealCount > uiMaxReal; --i)
  {
    Candidate& candidate = m_Ranked[i - 1];
    if (!candidate.m_bReal || !IsCandidateValid(candidate))
      continue;

    candidate.m_pInstance->VirtualizeVoice(candidate.m_uiVoice);
    candidate.m_bReal = false;
    --uiRealCount;
    ++m_uiFlipCount;
  }

  // past the virtual budget the least important virtual one-shots are stopped; looping voices stay
  plUInt32 uiVirtualCount = 0;
  for (const Candidate& candidate : m_Ranked)
  {
    if (!candidate.m_bReal && IsCandidateValid(candidate))
      ++uiVirtualCount;
  }

  for (plUInt32 i = m_Ranked.GetCount(); i > 0 && uiVirtualCount > uiMaxVirtual; --i)
  {
    Candidate& candidate = m_Ranked[i - 1];
    if (candidate.m_bReal || candidate.m_bLooping || !IsCandidateValid(candidate))
      continue;

    candidate.m_pInstance->KillVirtualVoice(candidate.m_uiVoice);
    --uiVirtualCount;
  }

  m_uiRealCount = uiRealCount;
}

bool plAudioVoiceManager::AdmitNewVoice(plUInt8 uiPriority, float fAudibility)
{
  if (fAudibility < GetAudibilityFloor())
    return false;

  if (m_uiRealCount < GetMaxRealVoices())
  {
    ++m_uiRealCount;
    return true;
  }

  const float fHysteresis = plMath::Max(cvar_PlasmaAudioVoiceHysteresis.GetValue(), 1.0f);

  // the lowest-ranked voice that is still real
  for (plUInt32 i = m_Ranked.GetCount(); i > 0; --i)
  {
    Candidate& candidate = m_Ranked[i - 1];
    if (!candidate.m_bReal)
      continue;

    if (!IsCandidateValid(candidate))
    {
      candidate.m_bReal = false;
      continue;
    }

    const bool bOutranks = uiPriority > candidate.m_uiPriority || (uiPriority == candidate.m_uiPriority && fAudibility > candidate.m_fAudibility * fHysteresis);
    if (!bOutranks)
      return false;

    // the virtualized voice (or, for a Kill event, its stopped instance) un-counts itself
    candidate.m_pInstance->VirtualizeVoice(candidate.m_uiVoice);
    candidate.m_bReal = false;
    ++m_uiFlipCount;
    ++m_uiRealCount;
    return true;
  }

  return false;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Events_AudioVoiceManager);
