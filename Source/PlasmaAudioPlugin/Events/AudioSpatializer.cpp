#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Events/AudioSpatializer.h>

namespace
{
  plAudioSpatializer* s_pActiveSpatializer = nullptr;
} // namespace

void plAudioSpatializer::SetActive(plAudioSpatializer* pSpatializer)
{
  s_pActiveSpatializer = pSpatializer;
}

plAudioSpatializer* plAudioSpatializer::GetActive()
{
  return s_pActiveSpatializer;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Events_AudioSpatializer);
