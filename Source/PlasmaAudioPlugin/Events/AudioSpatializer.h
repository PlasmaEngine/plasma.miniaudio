#pragma once

#include <Foundation/Math/Vec3.h>
#include <Foundation/Strings/StringView.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

struct ma_engine;
using ma_node = void;

/// \brief Where a voice plays from and where it is heard, handed to the active spatializer by every update.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioSpatialVoiceState
{
  plVec3 m_vEmitterPosition = plVec3::MakeZero();
  plVec3 m_vEmitterForward = plVec3::MakeZero(); ///< zero when the emitter has no direction
  plVec3 m_vListenerPosition = plVec3::MakeZero();
  plVec3 m_vListenerForward = plVec3(1, 0, 0);
  plVec3 m_vListenerUp = plVec3(0, 0, 1);
  float m_fSpread = 0.0f; ///< degrees, 0..360 (plAudioEventDesc::m_fSpread)
};

/// \brief A spatializer replaces miniaudio's panning for spatialized voices, e.g. with HRTF rendering.
///
/// While one is active, every spatialized voice that starts gets a node from CreateVoiceNode between its sound and its
/// occlusion filter, and miniaudio's own panning is off for that voice. The distance curve, cone, occlusion and Doppler
/// stay with the event system; the spatializer only places the sound. CreateVoiceNode returning null leaves the voice to
/// miniaudio. All functions are called on the audio thread; SetActive must happen while no voice plays (at startup).
class PL_PLASMAAUDIOPLUGIN_DLL plAudioSpatializer
{
public:
  virtual ~plAudioSpatializer() = default;

  virtual plStringView GetName() const = 0;

  /// \brief The node one voice plays through: one input and one output, both at the engine's channel count.
  virtual ma_node* CreateVoiceNode(ma_engine* pEngine) = 0;
  virtual void DestroyVoiceNode(ma_node* pNode) = 0;

  /// \brief The voice's current placement; called by every audio update while the voice is real.
  virtual void UpdateVoice(ma_node* pNode, const plAudioSpatialVoiceState& state) = 0;

  /// \brief The spatializer new voices use; null (the default) uses miniaudio's panning.
  static void SetActive(plAudioSpatializer* pSpatializer);
  static plAudioSpatializer* GetActive();
};
