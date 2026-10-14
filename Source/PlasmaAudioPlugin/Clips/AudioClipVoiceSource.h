#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <MiniAudio/miniaudio.h>
#include <PlasmaAudioPlugin/Codecs/AdpcmDataSource.h>
#include <PlasmaAudioPlugin/Codecs/AudioByteSource.h>
#include <PlasmaAudioPlugin/Codecs/OpusDataSource.h>

class plAudioStream;
class plAudioStreamService;

/// \brief The miniaudio data source a voice plays an audio clip through.
///
/// Filled by plAudioClipResource::CreateVoiceSource(). Only one of the embedded sources is in use, m_pSource points
/// at it. The voice source reads the clip resource's memory, so the resource must stay acquired (and loaded) until
/// Uninit(). The struct must not move after it was filled (miniaudio keeps pointers into it).
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioClipVoiceSource
{
  PL_DISALLOW_COPY_AND_ASSIGN(plAudioClipVoiceSource);

public:
  enum class Kind : plUInt8
  {
    None,
    PcmBuffer,  ///< m_PcmRef over the decoded PCM of the resource
    Adpcm,      ///< m_Adpcm over m_MemBytes (resident) or a stream
    PcmBytes,   ///< m_PcmBytes over m_MemBytes (resident) or a stream
    Opus,       ///< m_Opus over m_MemBytes (resident) or a stream
  };

  plAudioClipVoiceSource();
  ~plAudioClipVoiceSource();

  /// \brief Releases the embedded source and, for a streamed clip, the stream. Safe to call repeatedly.
  void Uninit();

  bool IsValid() const { return m_pSource != nullptr; }

  ma_data_source* m_pSource = nullptr;
  Kind m_Kind = Kind::None;

  ma_audio_buffer_ref m_PcmRef;
  plAdpcmDataSource m_Adpcm;
  plPcmByteDataSource m_PcmBytes;
  plOpusDataSource m_Opus;
  plMemoryByteSource m_MemBytes;

  /// \brief Stream of a streamed clip (the byte source of m_Adpcm / m_PcmBytes / m_Opus): null for resident clips. The
  /// stream service owns the object; Uninit() hands it back to m_pStreamService.
  plAudioStream* m_pStream = nullptr;
  plAudioStreamService* m_pStreamService = nullptr;
};
