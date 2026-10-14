#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <MiniAudio/miniaudio.h>

class plAudioByteSource;

/// \brief A miniaudio data source that decodes IMA ADPCM blocks (see plImaAdpcmCodec) from a plAudioByteSource.
///
/// Outputs s16 frames. One decoded block is cached, a seek only moves the cursor. When the byte source cannot
/// deliver the block the cursor is in (a streamed clip whose data has not arrived yet), the read outputs silence
/// for all requested frames, leaves the cursor where it is and counts an underrun, so playback resumes at the
/// same position once the data is there.
///
/// The struct must not move after plAdpcmDataSource_Init (miniaudio keeps a pointer to it). Its arrays keep their
/// memory across plAdpcmDataSource_Uninit, so re-initializing a source does not allocate unless it needs more.
struct PL_PLASMAAUDIOPLUGIN_DLL plAdpcmDataSource
{
  ma_data_source_base m_Base;
  plAudioByteSource* m_pBytes = nullptr;
  plUInt32 m_uiBlockFrames = 0;
  plUInt32 m_uiBlockBytes = 0;
  plUInt8 m_uiChannels = 0;
  plUInt32 m_uiSampleRate = 0;
  plUInt64 m_uiFrames = 0;
  plUInt64 m_uiCursor = 0;
  plUInt64 m_uiCachedBlock = 0xFFFFFFFFFFFFFFFFull;
  plAtomicInteger32 m_iUnderruns;
  plDynamicArray<plInt16> m_BlockPcm;  ///< decoded frames of m_uiCachedBlock
  plDynamicArray<plUInt8> m_BlockData; ///< encoded bytes of the block being decoded
};

/// \brief Sets up the source. Memory is only allocated here, and only when the arrays kept from a previous use are
/// too small; reads do not allocate.
PL_PLASMAAUDIOPLUGIN_DLL ma_result plAdpcmDataSource_Init(plAudioByteSource* pBytes, plUInt32 uiBlockFrames, plUInt8 uiChannels, plUInt32 uiSampleRate, plUInt64 uiFrames, plAdpcmDataSource* pSource);

PL_PLASMAAUDIOPLUGIN_DLL void plAdpcmDataSource_Uninit(plAdpcmDataSource* pSource);

/// \brief A miniaudio data source that reads interleaved s16 frames from a plAudioByteSource.
///
/// Used for streamed PCM16 clips. Behaves like plAdpcmDataSource when data is missing: silence, cursor kept,
/// underrun counted. The struct must not move after plPcmByteDataSource_Init.
struct PL_PLASMAAUDIOPLUGIN_DLL plPcmByteDataSource
{
  ma_data_source_base m_Base;
  plAudioByteSource* m_pBytes = nullptr;
  plUInt8 m_uiChannels = 0;
  plUInt32 m_uiSampleRate = 0;
  plUInt64 m_uiFrames = 0;
  plUInt64 m_uiCursor = 0;
  plAtomicInteger32 m_iUnderruns;
};

PL_PLASMAAUDIOPLUGIN_DLL ma_result plPcmByteDataSource_Init(plAudioByteSource* pBytes, plUInt8 uiChannels, plUInt32 uiSampleRate, plUInt64 uiFrames, plPcmByteDataSource* pSource);

PL_PLASMAAUDIOPLUGIN_DLL void plPcmByteDataSource_Uninit(plPcmByteDataSource* pSource);
