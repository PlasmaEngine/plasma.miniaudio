#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Profiling/AudioProfilerVoiceQuery.h>

namespace
{
  /// The default order's groups: real voices, then virtual and stopping ones, then ended ones.
  int GetDefaultRank(const plAudioVoiceSnapshotEntry& voice)
  {
    switch (voice.m_uiState)
    {
      case plAudioVoiceSnapshotEntry::Real:
        return 0;
      case plAudioVoiceSnapshotEntry::Ended:
        return 2;
      default:
        return 1;
    }
  }

  template <typename T>
  int Compare(const T& a, const T& b)
  {
    return a < b ? -1 : (b < a ? 1 : 0);
  }

  int CompareText(const plString& a, const plString& b)
  {
    return a.Compare_NoCase(b);
  }

  /// The order of a sort key, ascending.
  int CompareBy(plAudioProfilerVoiceQuery::SortKey key, const plAudioVoiceSnapshotEntry& a, const plAudioVoiceSnapshotEntry& b)
  {
    using SortKey = plAudioProfilerVoiceQuery::SortKey;

    switch (key)
    {
      case SortKey::Event:
        return CompareText(a.m_sEvent, b.m_sEvent);
      case SortKey::Owner:
        return CompareText(a.m_sEntity, b.m_sEntity);
      case SortKey::State:
        if (const int i = Compare(a.m_uiState, b.m_uiState); i != 0)
          return i;
        return Compare(a.m_bVirtual, b.m_bVirtual);
      case SortKey::Priority:
        return Compare(a.m_uiPriority, b.m_uiPriority);
      case SortKey::Audibility:
        return Compare(a.m_fAudibility, b.m_fAudibility);
      case SortKey::Volume:
        return Compare(a.m_fVolume, b.m_fVolume);
      case SortKey::Distance:
        return Compare(a.m_fDistance, b.m_fDistance);
      case SortKey::Bus:
        return CompareText(a.m_sBus, b.m_sBus);
      case SortKey::Codec:
        if (const int i = Compare(a.m_uiCodec, b.m_uiCodec); i != 0)
          return i;
        return Compare(a.m_uiLoadMode, b.m_uiLoadMode);
      default:
        return 0;
    }
  }
} // namespace

bool plAudioProfilerVoiceQuery::Matches(const plAudioVoiceSnapshotEntry& voice) const
{
  if (!m_StateFilter[plMath::Min<plUInt8>(voice.m_uiState, plAudioVoiceSnapshotEntry::Ended)])
    return false;

  if (!m_sBus.IsEmpty() && !voice.m_sBus.IsEqual_NoCase(m_sBus))
    return false;

  if (m_sText.IsEmpty())
    return true;

  return voice.m_sEvent.FindSubString_NoCase(m_sText) != nullptr || voice.m_sEntity.FindSubString_NoCase(m_sText) != nullptr || voice.m_sBus.FindSubString_NoCase(m_sText) != nullptr;
}

plUInt32 plAudioProfilerVoiceQuery::Run(const plAudioProfilingSnapshot& snapshot, plDynamicArray<const plAudioVoiceSnapshotEntry*>& out_rows) const
{
  out_rows.Clear();

  for (const plAudioVoiceSnapshotEntry& voice : snapshot.m_Voices)
  {
    if (Matches(voice))
      out_rows.PushBack(&voice);
  }

  for (const plAudioEndedVoiceSnapshotEntry& ended : snapshot.m_Ended)
  {
    if (Matches(ended.m_Voice))
      out_rows.PushBack(&ended.m_Voice);
  }

  const SortKey key = m_SortKey;
  const bool bAscending = m_bAscending;

  // the id keeps the order stable between snapshots
  out_rows.Sort([key, bAscending](const plAudioVoiceSnapshotEntry* a, const plAudioVoiceSnapshotEntry* b)
    {
      if (key == SortKey::Default)
      {
        if (const int iRank = Compare(GetDefaultRank(*a), GetDefaultRank(*b)); iRank != 0)
          return iRank < 0;
        if (a->m_fAudibility != b->m_fAudibility)
          return a->m_fAudibility > b->m_fAudibility;
        return a->m_uiId < b->m_uiId;
      }

      const int i = CompareBy(key, *a, *b);
      if (i != 0)
        return bAscending ? i < 0 : i > 0;
      return a->m_uiId < b->m_uiId; });

  const plUInt32 uiMatched = out_rows.GetCount();
  if (uiMatched > m_uiMaxRows)
  {
    out_rows.SetCount(m_uiMaxRows);
  }

  return uiMatched;
}

void plAudioProfilerVoiceQuery::CountStates(const plAudioProfilingSnapshot& snapshot, plUInt32 out_counts[4])
{
  for (plUInt32 i = 0; i < 4; ++i)
    out_counts[i] = 0;

  for (const plAudioVoiceSnapshotEntry& voice : snapshot.m_Voices)
  {
    ++out_counts[plMath::Min<plUInt8>(voice.m_uiState, plAudioVoiceSnapshotEntry::Stopping)];
  }

  out_counts[plAudioVoiceSnapshotEntry::Ended] = snapshot.m_Ended.GetCount();
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Profiling_AudioProfilerVoiceQuery);
