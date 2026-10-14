#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Codecs/AudioCodec.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioClipCodec, 1)
  PL_ENUM_CONSTANTS(plAudioClipCodec::Pcm16, plAudioClipCodec::ImaAdpcm, plAudioClipCodec::Opus)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioClipLoadMode, 1)
  PL_ENUM_CONSTANTS(plAudioClipLoadMode::DecompressOnLoad, plAudioClipLoadMode::CompressedInMemory, plAudioClipLoadMode::Streamed)
PL_END_STATIC_REFLECTED_ENUM;
// clang-format on

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Codecs_AudioCodec);
