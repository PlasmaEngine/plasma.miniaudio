#pragma once

#include <Foundation/Basics.h>
#include <Foundation/Logging/Log.h>
#include <MiniAudio/miniaudio.h>
#include <MiniAudioPlugin/MiniAudioPluginDLL.h>

PL_DEFINE_AS_POD_TYPE(ma_sound); // note: ma_sound_group is a typedef of ma_sound
PL_DEFINE_AS_POD_TYPE(ma_lpf_node);
PL_DEFINE_AS_POD_TYPE(ma_hpf_node);
PL_DEFINE_AS_POD_TYPE(ma_loshelf_node);
PL_DEFINE_AS_POD_TYPE(ma_peak_node);
PL_DEFINE_AS_POD_TYPE(ma_hishelf_node);
PL_DEFINE_AS_POD_TYPE(ma_delay_node);
PL_DEFINE_AS_POD_TYPE(ma_splitter_node);

// <StaticLinkUtil::StartHere>
// all include's before this will be left alone and not replaced by the StaticLinkUtil
// all include's AFTER this will be removed by the StaticLinkUtil and updated by what is actually used throughout the library

#define PL_MA_CHECK(x)                               \
  do                                                 \
  {                                                  \
    if (int res = (x); res != MA_SUCCESS)            \
    {                                                \
      PL_REPORT_FAILURE("MiniAudio error: {}", res); \
    }                                                \
  } while (false)