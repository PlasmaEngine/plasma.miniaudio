#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Foundation/Reflection/Reflection.h>

/// \brief How the sample payload of an audio clip is encoded.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioClipCodec
{
  using StorageType = plUInt8;

  enum Enum : StorageType
  {
    Pcm16 = 0,    ///< interleaved signed 16 bit samples
    ImaAdpcm = 1, ///< standard IMA ADPCM WAV blocks (4:1), see plImaAdpcmCodec
    Opus = 2,     ///< Opus packets at 48 kHz

    Default = ImaAdpcm
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PLASMAAUDIOPLUGIN_DLL, plAudioClipCodec);

/// \brief Where the samples of an audio clip live while the clip is loaded.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioClipLoadMode
{
  using StorageType = plUInt8;

  enum Enum : StorageType
  {
    DecompressOnLoad = 0,   ///< the payload is decoded to 16 bit PCM when the resource loads
    CompressedInMemory = 1, ///< the encoded payload stays in memory and every voice decodes it
    Streamed = 2,           ///< only the priming data is in memory, the payload is read from disk while playing

    Default = DecompressOnLoad
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PLASMAAUDIOPLUGIN_DLL, plAudioClipLoadMode);

/// \brief Constants shared by the clip builder, the clip resource and the stream service.
struct plAudioClipConstants
{
  /// \brief Size of one disk read of a streamed clip. The priming data of a streamed clip is two chunks.
  static constexpr plUInt32 StreamChunkBytes = 64 * 1024;

  /// \brief Number of bytes stored as start priming and as loop priming for a streamed clip.
  static constexpr plUInt32 PrimingBytes = 2 * StreamChunkBytes;

  /// \brief Number of min/max buckets per channel in the waveform overview of a clip.
  static constexpr plUInt32 PeakBuckets = 1024;
};
