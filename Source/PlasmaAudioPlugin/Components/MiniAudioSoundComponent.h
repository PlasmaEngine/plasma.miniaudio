#pragma once

#include <AudioSystem/Core/AudioSystemData.h>
#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

using plMiniAudioSoundResourceHandle = plTypedResourceHandle<class plMiniAudioSoundResource>;

class plMiniAudioSoundComponentManager : public plComponentManager<class plMiniAudioSoundComponent, plBlockStorageType::FreeList>
{
public:
  plMiniAudioSoundComponentManager(plWorld* pWorld);

  virtual void Initialize() override;
  virtual void Deinitialize() override;

private:
  friend class plMiniAudioSoundComponent;

  void UpdateEvents(const plWorldModule::UpdateContext& context);

  plUInt32 m_uiFirstComponentIndex = 0;
};

//////////////////////////////////////////////////////////////////////////

/// \brief Legacy sound component, hidden from the Add Component menu. New content uses plAudioTriggerComponent.
///
/// Playback goes through plAudioSystem requests: the component registers its own audio entity, loads and activates
/// the ATL trigger that its plMiniAudioSoundResource registers, and the PlasmaAudio middleware plays the sound as a
/// generated audio event. Volume and pitch are sent as per-entity multipliers.
///
/// Differences to the direct-playback version:
/// * Pause() stops the sound; Play() after a pause restarts it from the beginning.
/// * FadeOut() and deactivation stop the event with its fixed stop fade instead of the requested duration.
/// * The end of a non-looping sound is detected by polling the event state in the component update.
class PL_PLASMAAUDIOPLUGIN_DLL plMiniAudioSoundComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plMiniAudioSoundComponent, plComponent, plMiniAudioSoundComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void Initialize() override;
  virtual void Deinitialize() override;
  virtual void OnSimulationStarted() override;
  virtual void OnDeactivated() override;


  //////////////////////////////////////////////////////////////////////////
  // plMiniAudioComponent

private:
  friend class plComponentManagerSimple<class plMiniAudioSoundComponent, plComponentUpdateType::WhenSimulating>;


  //////////////////////////////////////////////////////////////////////////
  // plMiniAudioSoundComponent

public:
  plMiniAudioSoundComponent();
  ~plMiniAudioSoundComponent();

  void SetPaused(bool b);                                // [ property ]
  bool GetPaused() const { return m_bPaused; }           // [ property ]

  void SetPitch(float f);                                // [ property ]
  float GetPitch() const { return m_fPitch; }            // [ property ]

  void SetVolume(float f);                               // [ property ]
  float GetVolume() const { return m_fComponentVolume; } // [ property ]

  /// \brief If set, the global game speed does not affect the pitch of this event.
  ///
  /// This is important for global sounds, such as music or UI effects, so that they always play at their regular speed,
  /// even when the game is in slow motion.
  void SetNoGlobalPitch(bool bEnable);                     // [ property ]
  bool GetNoGlobalPitch() const;                           // [ property ]

  plEnum<plOnComponentFinishedAction2> m_OnFinishedAction; // [ property ]

  /// \brief Makes the sound play.
  ///
  /// If it was not yet playing, it starts playing a new sound.
  /// If it was already playing, but paused, playback is resumed.
  /// If it was already playing, there is no change.
  void Play(); // [ scriptable ]

  /// \brief If a sound is playing, it is stopped.
  ///
  /// Call Play() to start it again from the beginning.
  void Pause(); // [ scriptable ]

  /// \brief Stops the sound playback.
  void Stop(); // [ scriptable ]

  /// \brief Stops the sound with the event's fixed stop fade. The duration is ignored.
  void FadeOut(plTime fadeDuration); // [ scriptable ]

  /// \brief Plays a completely new sound at the location of this component and with all its current properties.
  ///
  /// Pitch, volume, position and direction are copied to the new sound instance.
  /// The new sound then plays to the end and cannot be controlled through this component any further.
  /// No sound should be playing on this component, when using this function.
  void StartOneShot();                                    // [ scriptable ]

protected:
  void OnMsgDeleteGameObject(plMsgDeleteGameObject& msg); // [ msg handler ]

  void Update();
  void SoundFinished();

  /// \brief Blocks until the sound resource is loaded (which registers its ATL trigger) and returns the trigger id,
  /// or kInvalidAudioSystemId.
  plAudioSystemDataID PrepareTrigger(bool& out_bLooping) const;

  plAudioSystemTransform ComputeTransform() const;
  float ComputeVolume() const;
  float ComputePitch() const;

  void EnsureEntityRegistered();
  void EnsureTriggerLoaded(plAudioSystemDataID uiTriggerId);
  void SendTransformAndMultipliers(bool bForce);
  void ReleaseAudioObjects();

  plMiniAudioSoundResourceHandle m_hSound;
  float m_fComponentVolume = 1.0f;
  float m_fPitch = 1.0f;
  bool m_bPaused = false;

  plAudioSystemDataID m_uiEntityId = kInvalidAudioSystemId;
  plAudioSystemDataID m_uiEventId = kInvalidAudioSystemId;
  plAudioSystemDataID m_uiLoadedTriggerId = kInvalidAudioSystemId;
  plAudioSystemTransform m_LastTransform;
  float m_fLastSentVolume = -1.0f;
  float m_fLastSentPitch = -1.0f;
  bool m_bEntityRegistered = false;
  bool m_bPlaying = false;          ///< an activation was sent and not stopped yet
  bool m_bWaitingForFinish = false; ///< the activation succeeded and the sound does not loop
};
