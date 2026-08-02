#pragma once

#include <Core/World/Component.h>
#include <Core/World/ComponentManager.h>
#include <MiniAudioPlugin/MiniAudioPluginDLL.h>

struct plMiniAudioSoundInstance;
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

class PL_MINIAUDIOPLUGIN_DLL plMiniAudioSoundComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plMiniAudioSoundComponent, plComponent, plMiniAudioSoundComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
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

  /// \brief If a sound is playing, it pauses at the current play position.
  ///
  /// Call Play() to resume playing.
  void Pause(); // [ scriptable ]

  /// \brief Interrupts the sound playback abruptly.
  void Stop(); // [ scriptable ]

  /// \brief Stops the sound, by fading it out over a short period.
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
  void UpdateParameters(plMiniAudioSoundInstance* pInstance, float fVolume, float fPitch) const;


  friend class plMiniAudioSingleton;
  void SoundFinished();

  plMiniAudioSoundResourceHandle m_hSound;
  float m_fComponentVolume = 1.0f;
  float m_fPitch = 1.0f;
  float m_fResourceVolume = 1.0f;
  float m_fResourcePitch = 1.0f;
  bool m_bPaused = false;

  plMiniAudioSoundInstance* m_pInstance = nullptr;

};
