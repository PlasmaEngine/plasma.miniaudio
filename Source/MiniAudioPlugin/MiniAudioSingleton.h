#pragma once

#include <Core/World/Declarations.h>
#include <Foundation/Configuration/Plugin.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Types/UniquePtr.h>
#include <MiniAudio/miniaudio.h>
#include <MiniAudioPlugin/MiniAudioPluginDLL.h>

// TODO MiniAudio: Future Work
//
// * in MiniAudioResource Load sounds through the MA resource manager (redirect file hooks to our resource manager)
// * then decode one sound right away and use that as a template to copy from for future sound playback
// * Add preview playback to sound asset
// * Add max sound size, check whether MA adds FMOD-like attenuation models
// * skip sounds that are too far away

struct plMiniAudioSoundInstance
{
  ma_sound m_Sound;
  ma_decoder m_Decoder;
  plWorld* pWorld = nullptr;
  plComponentHandle m_hComponent;
  plUInt16 m_uiOwnIndex;
  bool m_bInUse = false;
};


class PL_MINIAUDIOPLUGIN_DLL plMiniAudioSingleton
{
  PL_DECLARE_SINGLETON(plMiniAudioSingleton);

public:
  plMiniAudioSingleton();
  ~plMiniAudioSingleton();

  void Startup();
  void Shutdown();

  ma_engine* GetEngine() { return &m_pData->m_Engine; }

  /// \brief Can be called before startup to load the configuration from a different file.
  /// Otherwise will automatically be loaded at startup with the default path.
  void LoadConfiguration(plStringView sFile);

  /// \brief By default the integration will auto-detect the platform (and thus the config) to use.
  /// Calling this before startup allows to override which configuration is used.
  void SetOverridePlatform(plStringView sPlatform);

  /// \brief Automatically called by the plugin every time plGameApplicationExecutionEvent::BeforeUpdatePlugins is fired.
  void UpdateSound();

  /// \brief Adjusts the master volume. This affects all sounds, with no exception. Value must be between 0.0f and 1.0f.
  void SetMasterChannelVolume(float fVolume);
  float GetMasterChannelVolume() const;

  /// \brief Allows to mute all sounds. Useful for when the application goes to a background state.
  void SetMasterChannelMute(bool bMute);
  bool GetMasterChannelMute() const;

  /// \brief Allows to pause all sounds. Useful for when the application goes to a background state and you want to pause all sounds,
  /// instead of mute them.
  void SetMasterChannelPaused(bool bPaused);
  bool GetMasterChannelPaused() const;

  /// \brief Specifies the volume for a sound group.
  ///
  /// This is used to control the volume of high level sound groups, such as 'Effects', 'Music', 'Ambiance or 'Speech'.
  void SetSoundGroupVolume(plStringView sGroupName, float fVolume);
  float GetSoundGroupVolume(plStringView sGroupName) const;

  struct SoundGroup
  {
    plString m_sName;
    float m_fVolume = 1.0f;
    plUniquePtr<ma_sound_group> m_pGroup;
  };

  SoundGroup& GetSoundGroup(plStringView sGroupName);

  /// \brief Default is 1. Allows to set how many virtual listeners the sound is mixed for (split screen game play).
  void SetNumListeners(plUInt8 uiNumListeners);
  plUInt8 GetNumListeners() const;

  void SetListenerOverrideMode(bool bEnabled);
  void SetListener(plInt32 iIndex, const plVec3& vPosition, const plVec3& vForward, const plVec3& vUp, const plVec3& vVelocity);
  plVec3 GetListenerPosition() const { return m_vListenerPosition; }

  plResult OneShotSound(plWorld* pWorld, plStringView sResourceID, const plTransform& globalPosition, float fPitch = 1.0f, float fVolume = 1.0f, bool bBlockIfNotLoaded = true);

  plMiniAudioSoundInstance* AllocateSoundInstance(const plDataBuffer& audioData, plWorld* pWorld, plComponentHandle hComponent, ma_sound_group* pGroup);
  void FreeSoundInstance(plMiniAudioSoundInstance*& ref_pInstance);
  void DetachSoundInstance(plMiniAudioSoundInstance*& ref_pInstance);

  void DetachAndFadeOutSoundInstance(plMiniAudioSoundInstance*& ref_pInstance, plTime fadeDuration);

  void SoundEnded(plMiniAudioSoundInstance* pInstance);

  void StopWorldSounds(plWorld* pWorld);

private:
  bool m_bInitialized = false;
  bool m_bListenerOverrideMode = false;
  plVec3 m_vListenerPosition;

  struct Data
  {
    plMutex m_Mutex;

    ma_engine m_Engine;
    plDeque<plMiniAudioSoundInstance> m_SoundInstancesStorage;
    plDeque<plUInt32> m_SoundInstanceFreeList;

    plDeque<plUInt32> m_FadingInstances;
    plDeque<plUInt32> m_FinishedInstances;

    plHybridArray<SoundGroup, 4> m_SoundGroups;
  };

  plUniquePtr<Data> m_pData;
};
