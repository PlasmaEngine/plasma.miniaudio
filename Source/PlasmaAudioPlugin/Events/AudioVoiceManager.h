#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Types/UniquePtr.h>
#include <MiniAudio/miniaudio.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

class plPlasmaAudioEventInstance;

/// \brief Decides which voices of the playing event instances are real (decoded and mixed) and which are virtual.
///
/// A virtual voice keeps its timeline (start frame, pitch, length, loop info) but owns no decoder, clip source,
/// stream or miniaudio nodes once its short fade-out after the virtualization ended; it counts as virtual from the
/// virtualization on. Every Update ranks all voices of the instances that are not stopping: event priority
/// first (higher wins), then audibility (a real voice's audibility is scaled by PlasmaAudio.VoiceHysteresis), then
/// instance start frame and start order, then voice slot. The first PlasmaAudio.MaxRealVoices voices that are at
/// least PlasmaAudio.VoiceAudibilityFloor loud should be real, the rest virtual. A voice changes state at most once
/// per PlasmaAudio.VoiceMinDwellMs, except when the real budget has to be enforced. Past
/// PlasmaAudio.MaxVirtualVoices, the lowest-ranked virtual voices of non-looping instruments are stopped.
///
/// Real voices of stopping instances are never virtualized; they count against the real budget until their fade
/// ends. Per-bus voice limits are not implemented; they would cap the real voices per output bus during the rank walk.
///
/// Owned by plPlasmaAudioEventManager and used under the resource manager mutex, like the instances.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioVoiceManager
{
public:
  /// \brief Overrides the voice budgets. A negative value selects the cvar value (PlasmaAudio.MaxRealVoices,
  /// PlasmaAudio.MaxVirtualVoices).
  static void SetBudgets(plInt32 iMaxRealVoices, plInt32 iMaxVirtualVoices);

  PL_NODISCARD static plUInt32 GetMaxRealVoices();
  PL_NODISCARD static plUInt32 GetMaxVirtualVoices();
  PL_NODISCARD static float GetAudibilityFloor();

  /// \brief Drops the candidates of an instance that is about to be reset or destroyed, so the ranking of the last
  /// Update stays usable for admissions until the next Update.
  void ForgetInstance(const plPlasmaAudioEventInstance* pInstance);

  /// \brief Forgets the ranking and the real voice count, after all instances were destroyed.
  void Reset();

  /// \brief Ranks all voices and virtualizes, realizes or stops voices accordingly.
  void Update(plArrayPtr<const plUniquePtr<plPlasmaAudioEventInstance>> instances, ma_uint64 uiNow, ma_uint32 uiSampleRate);

  /// \brief Whether a voice that starts outside of a ranking may be real.
  ///
  /// Admitted when it is at least as loud as the audibility floor and the live real voice count is below the budget,
  /// or when it outranks the lowest-ranked voice of the last ranking that is still real, which is then virtualized.
  /// The ranking of the last Update stays valid until the next one (see ForgetInstance), so voices that start while
  /// the instances update (delayed instruments, loop re-triggers, loop region re-arms) are admitted the same way.
  bool AdmitNewVoice(plUInt8 uiPriority, float fAudibility);

  /// \brief Counts a real voice that started without admission (the follow-up voice of a real looping instrument).
  void NoteRealVoiceStarted() { ++m_uiRealCount; }

  /// \brief Un-counts a real voice that stopped or was virtualized.
  void NoteRealVoiceEnded()
  {
    if (m_uiRealCount > 0)
      --m_uiRealCount;
  }

  /// \brief Number of voice state changes (virtualizations and realizations) since construction.
  PL_NODISCARD plUInt64 GetFlipCount() const { return m_uiFlipCount; }

private:
  struct Candidate
  {
    plPlasmaAudioEventInstance* m_pInstance = nullptr;
    ma_uint64 m_uiInstanceStartFrame = 0;
    plUInt64 m_uiInstanceSerial = 0;
    float m_fAudibility = 0.0f;
    float m_fScore = 0.0f;
    plUInt16 m_uiVoice = 0;
    plUInt8 m_uiPriority = 0;
    bool m_bReal = false;
    bool m_bWantReal = false;
    bool m_bLooping = false;
  };

  /// \brief Whether the candidate's voice still exists in the state the candidate records.
  static bool IsCandidateValid(const Candidate& candidate);

  plDynamicArray<Candidate> m_Ranked; ///< the voices of the last Update, best first
  plUInt32 m_uiRealCount = 0;         ///< live real voices: set by Update, then kept up to date by the instances
  plUInt64 m_uiFlipCount = 0;
};
