#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/IO/Stream.h>
#include <PlasmaAudioPlugin/Clips/AudioClipDesc.h>
#include <PlasmaAudioPlugin/Codecs/OpusCodec.h>

static constexpr plTypeVersion kAudioClipHeaderVersion = 3;

namespace
{
  void WriteSeekTable(plStreamWriter& inout_stream, const plDynamicArray<plAudioClipSeekEntry>& table)
  {
    inout_stream << table.GetCount();
    for (const plAudioClipSeekEntry& entry : table)
    {
      inout_stream << entry.m_uiByteOffset;
      inout_stream << entry.m_uiFrame;
    }
  }

  void ReadSeekTable(plStreamReader& inout_stream, plDynamicArray<plAudioClipSeekEntry>& out_table)
  {
    plUInt32 uiCount = 0;
    inout_stream >> uiCount;
    out_table.SetCount(uiCount);
    for (plAudioClipSeekEntry& entry : out_table)
    {
      inout_stream >> entry.m_uiByteOffset;
      inout_stream >> entry.m_uiFrame;
    }
  }
} // namespace

void plAudioClipHeader::Write(plStreamWriter& inout_stream) const
{
  inout_stream.WriteVersion(kAudioClipHeaderVersion);

  inout_stream << static_cast<plUInt8>(m_eCodec);
  inout_stream << static_cast<plUInt8>(m_eLoadMode);
  inout_stream << m_uiChannels;
  inout_stream << static_cast<plUInt8>(m_bHasLoop ? 1 : 0);

  inout_stream << m_uiSampleRate;
  inout_stream << m_uiFrames;
  inout_stream << m_uiLoopStart;
  inout_stream << m_uiLoopEnd;

  inout_stream << m_fMeasuredLufs;
  inout_stream << m_fAppliedGainDb;

  inout_stream << m_uiBlockFrames;
  inout_stream << m_uiBlockBytes;
  inout_stream << m_uiPreSkip;

  PL_ASSERT_DEV(m_Peaks.GetCount() == m_uiPeakBuckets * m_uiChannels * 2, "Peak data does not match the bucket count");
  inout_stream << m_uiPeakBuckets;
  inout_stream.WriteBytes(m_Peaks.GetData(), m_Peaks.GetCount()).AssertSuccess();

  inout_stream << m_Languages.GetCount();
  for (const plAudioClipLanguageEntry& entry : m_Languages)
  {
    inout_stream << entry.m_sLanguage;
    inout_stream << entry.m_uiPrimingBytes;
    inout_stream << entry.m_uiLoopPrimingBytes;
    inout_stream << entry.m_uiPayloadBytes;
    WriteSeekTable(inout_stream, entry.m_SeekTable);
  }
}

plResult plAudioClipHeader::Read(plStreamReader& inout_stream)
{
  const plTypeVersion uiVersion = inout_stream.ReadVersion(kAudioClipHeaderVersion);
  m_uiVersion = uiVersion;

  plUInt8 uiCodec = 0;
  plUInt8 uiLoadMode = 0;
  plUInt8 uiFlags = 0;
  inout_stream >> uiCodec;
  inout_stream >> uiLoadMode;
  inout_stream >> m_uiChannels;
  inout_stream >> uiFlags;

  if (uiCodec > plAudioClipCodec::Opus || uiLoadMode > plAudioClipLoadMode::Streamed || m_uiChannels == 0)
    return PL_FAILURE;

  m_eCodec = static_cast<plAudioClipCodec::Enum>(uiCodec);
  m_eLoadMode = static_cast<plAudioClipLoadMode::Enum>(uiLoadMode);
  m_bHasLoop = (uiFlags & 1) != 0;

  inout_stream >> m_uiSampleRate;
  inout_stream >> m_uiFrames;
  inout_stream >> m_uiLoopStart;
  inout_stream >> m_uiLoopEnd;

  inout_stream >> m_fMeasuredLufs;
  inout_stream >> m_fAppliedGainDb;

  inout_stream >> m_uiBlockFrames;
  inout_stream >> m_uiBlockBytes;

  m_uiPreSkip = 0;
  if (uiVersion >= 2)
  {
    inout_stream >> m_uiPreSkip;
  }

  inout_stream >> m_uiPeakBuckets;
  const plUInt32 uiPeakBytes = m_uiPeakBuckets * m_uiChannels * 2;
  m_Peaks.SetCountUninitialized(uiPeakBytes);
  if (inout_stream.ReadBytes(m_Peaks.GetData(), uiPeakBytes) != uiPeakBytes)
    return PL_FAILURE;

  // before version 3 all languages share one seek table
  plDynamicArray<plAudioClipSeekEntry> sharedSeekTable;
  if (uiVersion < 3)
  {
    ReadSeekTable(inout_stream, sharedSeekTable);
  }

  plUInt32 uiCount = 0;
  inout_stream >> uiCount;
  if (uiCount == 0)
    return PL_FAILURE;

  m_Languages.SetCount(uiCount);
  for (plAudioClipLanguageEntry& entry : m_Languages)
  {
    inout_stream >> entry.m_sLanguage;
    inout_stream >> entry.m_uiPrimingBytes;
    inout_stream >> entry.m_uiLoopPrimingBytes;
    inout_stream >> entry.m_uiPayloadBytes;

    if (uiVersion >= 3)
    {
      ReadSeekTable(inout_stream, entry.m_SeekTable);
    }
    else
    {
      entry.m_SeekTable = sharedSeekTable;
    }
  }

  SelectLanguage(0);
  return PL_SUCCESS;
}

void plAudioClipHeader::SelectLanguage(plUInt32 uiLanguage)
{
  m_SeekTable = m_Languages[uiLanguage].m_SeekTable;
}

plUInt32 plAudioClipHeader::FindLanguage(plStringView sLanguage) const
{
  for (plUInt32 i = 0; i < m_Languages.GetCount(); ++i)
  {
    if (m_Languages[i].m_sLanguage.IsEqual_NoCase(sLanguage))
      return i;
  }

  return 0;
}

plUInt64 plAudioClipHeader::GetPayloadOffsetForFrame(plUInt64 uiFrame) const
{
  switch (m_eCodec)
  {
    case plAudioClipCodec::Pcm16:
      return uiFrame * GetPcmFrameBytes();

    case plAudioClipCodec::ImaAdpcm:
      return m_uiBlockFrames > 0 ? (uiFrame / m_uiBlockFrames) * m_uiBlockBytes : 0;

    default:
      break;
  }

  const plUInt64 uiSeekFrame = m_eCodec == plAudioClipCodec::Opus ? plOpusCodec::GetDecodeStartPacket(uiFrame, m_uiPreSkip) * plOpusCodec::FramesPerPacket : uiFrame;

  plUInt64 uiOffset = 0;
  for (const plAudioClipSeekEntry& entry : m_SeekTable)
  {
    if (entry.m_uiFrame > uiSeekFrame)
      break;

    uiOffset = entry.m_uiByteOffset;
  }

  return uiOffset;
}

plUInt64 plAudioClipHeader::GetDecodeStartFrame(plUInt64 uiFrame) const
{
  switch (m_eCodec)
  {
    case plAudioClipCodec::Pcm16:
      return uiFrame;

    case plAudioClipCodec::ImaAdpcm:
      return m_uiBlockFrames > 0 ? (uiFrame / m_uiBlockFrames) * m_uiBlockFrames : 0;

    default:
      break;
  }

  const plUInt64 uiSeekFrame = m_eCodec == plAudioClipCodec::Opus ? plOpusCodec::GetDecodeStartPacket(uiFrame, m_uiPreSkip) * plOpusCodec::FramesPerPacket : uiFrame;

  plUInt64 uiStart = 0;
  for (const plAudioClipSeekEntry& entry : m_SeekTable)
  {
    if (entry.m_uiFrame > uiSeekFrame)
      break;

    uiStart = entry.m_uiFrame;
  }

  return uiStart;
}

plUInt64 plAudioClipHeader::GetLoopPrimingOffset() const
{
  if (m_uiVersion >= 3 || m_eCodec != plAudioClipCodec::ImaAdpcm || m_SeekTable.IsEmpty())
    return GetPayloadOffsetForFrame(m_uiLoopStart);

  // ADPCM clips before version 3 copied the loop priming data from the seek entry at or before the loop start
  plUInt64 uiOffset = 0;
  for (const plAudioClipSeekEntry& entry : m_SeekTable)
  {
    if (entry.m_uiFrame > m_uiLoopStart)
      break;

    uiOffset = entry.m_uiByteOffset;
  }

  return uiOffset;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Clips_AudioClipDesc);
