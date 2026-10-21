#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Vec3.h>
#include <Foundation/Strings/String.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

/// \brief One playing spatialized event, as the Audio view mode draws it.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioDebugEmitter
{
  plString m_sEvent;
  plVec3 m_vPosition = plVec3::MakeZero();
  plVec3 m_vForward = plVec3::MakeZero();
  plVec3 m_vListener = plVec3::MakeZero(); ///< the closest listener, which the event is heard from
  float m_fMinDistance = 0.0f;
  float m_fMaxDistance = 0.0f;
  float m_fConeInnerAngle = 360.0f; ///< degrees
  float m_fConeOuterAngle = 360.0f; ///< degrees
  float m_fDistance = 0.0f;
  float m_fGain = 0.0f;      ///< distance curve, occlusion and cone, linear
  float m_fOcclusion = 0.0f; ///< 0..1
  float m_fAudibility = 0.0f;
  bool m_bReal = false;      ///< at least one voice is real

  /// \brief The environments the event sends to, with their amounts.
  plHybridArray<plString, 2> m_EnvironmentNames;
  plHybridArray<float, 2> m_EnvironmentAmounts;
};

/// \brief What the audio thread last saw, for the Audio view mode. Filled while a view or the Audio.Debug.Draw cvar
/// asks for it.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioDebugSnapshot
{
  plHybridArray<plVec3, 4> m_Listeners;
  plDynamicArray<plAudioDebugEmitter> m_Emitters;
  plUInt32 m_uiRealVoices = 0;
  plUInt32 m_uiVirtualVoices = 0;
  plUInt32 m_uiMaxRealVoices = 0;
};
