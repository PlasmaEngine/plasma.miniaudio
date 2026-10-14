#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/Constants.h>
#include <Foundation/Strings/String.h>
#include <PlasmaAudioPlugin/Codecs/AudioCodec.h>

class plStreamWriter;

/// \brief Decoded source audio a clip is built from.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioClipSourcePcm
{
  plDynamicArray<float> m_Samples; ///< interleaved, nominally in [-1, 1]
  plUInt32 m_uiSampleRate = 48000;
  plUInt8 m_uiChannels = 1;

  /// \brief Loop of the source file (WAV smpl chunk), in source frames. m_uiLoopEnd is exclusive.
  bool m_bHasLoop = false;
  plUInt64 m_uiLoopStart = 0;
  plUInt64 m_uiLoopEnd = 0;

  plUInt64 GetFrames() const { return m_uiChannels > 0 ? m_Samples.GetCount() / m_uiChannels : 0; }
};

/// \brief How the clip builder processes and encodes the source audio.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioClipBuildSettings
{
  plAudioClipCodec::Enum m_eCodec = plAudioClipCodec::Default;
  plAudioClipLoadMode::Enum m_eLoadMode = plAudioClipLoadMode::Default;

  /// \brief Total bitrate of an Opus clip, all channels together (a stereo clip spends about half on each channel).
  /// Opus clips are mono or stereo.
  plUInt32 m_uiOpusBitrateKbps = 96;

  /// \brief Output sample rate, 0 keeps the source rate. Opus always encodes at 48 kHz.
  plUInt32 m_uiTargetSampleRate = 0;

  /// \brief Averages all source channels into one.
  bool m_bForceMono = false;

  /// \brief Integrated loudness to normalise to (LUFS), 0 disables normalisation. The gain is limited so that the
  /// peak stays at or below 0 dBFS.
  float m_fNormaliseLufs = 0.0f;

  /// \brief Peak level (dBFS) that clips too short for a loudness measurement (< 400 ms) are normalised to.
  float m_fPeakFallbackDb = -1.0f;

  /// \brief Leading and trailing audio below this level (dBFS) is removed. -infinity disables trimming.
  float m_fTrimSilenceDb = -plMath::Infinity<float>();

  /// \brief Loop points in source frames. -1 takes the point from the source file's smpl chunk. A loop start
  /// without an end loops to the end of the clip.
  plInt64 m_iLoopStart = -1;
  plInt64 m_iLoopEnd = -1;

  /// \brief Language names of the per-language sources passed to plAudioClipBuilder::Build. Entry 0 must be ""
  /// (the default language). Empty means a single default language.
  plDynamicArray<plString> m_Languages;
};

/// \brief Turns decoded source audio into the .plBinAudioClip format (see plAudioClipHeader).
///
/// Pipeline per language: downmix -> resample -> trim silence -> loudness measurement and gain -> 16 bit
/// quantisation. Then all languages are padded with silence to the longest one, the waveform peaks are taken
/// from the default language, and the payloads are encoded, each with its seek table and priming data.
///
/// The builder writes the clip header and sections only; the caller writes the plAssetFileHeader before it.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioClipBuilder
{
public:
  static plResult Build(const plAudioClipSourcePcm& source, const plAudioClipBuildSettings& settings, plStreamWriter& inout_stream);

  /// \brief Builds a clip with one section per language. sources[i] is the audio of settings.m_Languages[i].
  static plResult Build(plArrayPtr<const plAudioClipSourcePcm* const> sources, const plAudioClipBuildSettings& settings, plStreamWriter& inout_stream);

  /// \brief Decodes any file miniaudio can read (WAV, FLAC, MP3, Vorbis) to float samples. For WAV files the
  /// first loop of the smpl chunk is read as well.
  static plResult DecodeSourceFile(plStringView sAbsolutePath, plAudioClipSourcePcm& out_source);

  /// \brief Resamples interleaved float audio with miniaudio's linear resampler, compensating its latency.
  static plResult Resample(plArrayPtr<const float> input, plUInt8 uiChannels, plUInt32 uiRateIn, plUInt32 uiRateOut, plDynamicArray<float>& out_output);
};
