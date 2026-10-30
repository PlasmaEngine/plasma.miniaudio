#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Profiling/AudioEndedVoices.h>

void plAudioEndedVoices::SetEnabled(bool bEnabled)
{
  if (m_bEnabled == bEnabled)
    return;

  m_bEnabled = bEnabled;
  m_uiTotalAdded = 0;

  if (bEnabled)
  {
    m_Ring.SetCount(Capacity);
  }
  else
  {
    m_Ring.Clear();
    m_Ring.Compact();
  }
}

void plAudioEndedVoices::Add(const plAudioVoiceSnapshotEntry& voice, double fEndTime)
{
  if (!m_bEnabled)
    return;

  plAudioEndedVoiceSnapshotEntry& entry = m_Ring[static_cast<plUInt32>(m_uiTotalAdded % Capacity)];
  entry.m_Voice = voice;
  entry.m_Voice.m_uiState = plAudioVoiceSnapshotEntry::Ended;
  entry.m_fEndTime = fEndTime;

  ++m_uiTotalAdded;
}

void plAudioEndedVoices::Collect(double fNow, double fWindow, plDynamicArray<plAudioEndedVoiceSnapshotEntry>& out_ended) const
{
  out_ended.Clear();

  if (!m_bEnabled)
    return;

  const plUInt64 uiStored = plMath::Min<plUInt64>(m_uiTotalAdded, Capacity);
  for (plUInt64 n = m_uiTotalAdded - uiStored; n < m_uiTotalAdded; ++n)
  {
    const plAudioEndedVoiceSnapshotEntry& entry = m_Ring[static_cast<plUInt32>(n % Capacity)];
    if (fNow - entry.m_fEndTime <= fWindow)
    {
      out_ended.PushBack(entry);
    }
  }
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Profiling_AudioEndedVoices);
