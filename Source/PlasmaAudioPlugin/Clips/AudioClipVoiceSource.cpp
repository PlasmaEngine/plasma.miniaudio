#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Clips/AudioClipVoiceSource.h>
#include <PlasmaAudioPlugin/Streaming/AudioStreamService.h>

plAudioClipVoiceSource::plAudioClipVoiceSource()
{
  plMemoryUtils::ZeroFill(&m_PcmRef, 1);
}

plAudioClipVoiceSource::~plAudioClipVoiceSource()
{
  Uninit();
}

void plAudioClipVoiceSource::Uninit()
{
  switch (m_Kind)
  {
    case Kind::PcmBuffer:
      ma_audio_buffer_ref_uninit(&m_PcmRef);
      plMemoryUtils::ZeroFill(&m_PcmRef, 1);
      break;

    case Kind::Adpcm:
      plAdpcmDataSource_Uninit(&m_Adpcm);
      break;

    case Kind::PcmBytes:
      plPcmByteDataSource_Uninit(&m_PcmBytes);
      break;

    case Kind::Opus:
      plOpusDataSource_Uninit(&m_Opus);
      break;

    case Kind::None:
      break;
  }

  // after the decoder, which reads from the stream
  if (m_pStream != nullptr)
  {
    m_pStreamService->ReleaseStream(m_pStream);
    m_pStream = nullptr;
    m_pStreamService = nullptr;
  }

  m_MemBytes.SetData({});
  m_pSource = nullptr;
  m_Kind = Kind::None;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Clips_AudioClipVoiceSource);
