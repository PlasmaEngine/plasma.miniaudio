#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/Threading/Lock.h>
#include <PlasmaAudioPlugin/Profiling/AudioEventLog.h>

namespace
{
  /// Copies sText into the buffer; text that does not fit keeps its end, cut at a UTF-8 character boundary.
  void CopyTail(char* pBuffer, plUInt32 uiBufferBytes, plStringView sText)
  {
    const char* pStart = sText.GetStartPointer();
    plUInt32 uiBytes = sText.GetElementCount();

    if (uiBytes >= uiBufferBytes)
    {
      pStart += uiBytes - (uiBufferBytes - 1);
      uiBytes = uiBufferBytes - 1;

      // skip UTF-8 continuation bytes of a character that was cut
      while (uiBytes > 0 && (static_cast<plUInt8>(*pStart) & 0xC0u) == 0x80u)
      {
        ++pStart;
        --uiBytes;
      }
    }

    if (uiBytes > 0)
    {
      plMemoryUtils::Copy(pBuffer, pStart, uiBytes);
    }
    pBuffer[uiBytes] = '\0';
  }
} // namespace

plAudioEventLog::plAudioEventLog()
{
  m_Entries.SetCount(Capacity);
  m_StartTime = plTime::Now();
}

void plAudioEventLog::Add(plAudioLogEventType::Enum type, plStringView sName, plStringView sDetail)
{
  const double fTime = (plTime::Now() - m_StartTime).GetSeconds();

  PL_LOCK(m_Mutex);

  Entry& entry = m_Entries[static_cast<plUInt32>(m_uiNextSequence % Capacity)];
  entry.m_fTime = fTime;
  entry.m_uiType = static_cast<plUInt8>(type);
  CopyTail(entry.m_szName, NameBytes, sName);
  CopyTail(entry.m_szDetail, DetailBytes, sDetail);

  ++m_uiNextSequence;
}

plUInt64 plAudioEventLog::Collect(plUInt64 uiFirstSequence, plDynamicArray<plAudioEventLogSnapshotEntry>& out_entries) const
{
  PL_LOCK(m_Mutex);

  const plUInt64 uiOldest = m_uiNextSequence > Capacity ? m_uiNextSequence - Capacity : 0;
  const plUInt64 uiFirst = plMath::Min(plMath::Max(uiFirstSequence, uiOldest), m_uiNextSequence);

  out_entries.SetCount(static_cast<plUInt32>(m_uiNextSequence - uiFirst));

  for (plUInt64 uiSequence = uiFirst; uiSequence < m_uiNextSequence; ++uiSequence)
  {
    const Entry& entry = m_Entries[static_cast<plUInt32>(uiSequence % Capacity)];
    plAudioEventLogSnapshotEntry& out = out_entries[static_cast<plUInt32>(uiSequence - uiFirst)];
    out.m_fTime = entry.m_fTime;
    out.m_uiType = entry.m_uiType;
    out.m_sName = entry.m_szName;
    out.m_sDetail = entry.m_szDetail;
  }

  return uiFirst;
}

plUInt64 plAudioEventLog::GetNextSequence() const
{
  PL_LOCK(m_Mutex);
  return m_uiNextSequence;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Profiling_AudioEventLog);
