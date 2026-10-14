#pragma once

#include <Core/World/Declarations.h>
#include <Foundation/Configuration/Plugin.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Memory/CommonAllocators.h>
#include <Foundation/Types/UniquePtr.h>
#include <MiniAudio/miniaudio.h>
#include <PlasmaAudioPlugin/Mixer/MiniAudioDspNodes.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <atomic>

struct plCVarEvent;

/// \brief Owns the miniaudio context and engine: output device selection, master volume / mute / pause,
/// legacy sound groups and the listener slots.
///
/// Voices are owned by plPlasmaAudioEventManager; this class does not play sounds itself.
/// The playback device is chosen by the PlasmaAudio.OutputDevice cvar (empty = system default).
/// When the device is lost or the cvar changes, IsDeviceReinitRequired() turns true; the middleware then
/// releases everything attached to the engine and calls ReinitDevice().
///
/// On Windows, every entry point that creates or queries the context, engine or device first initializes COM on the
/// calling thread (once per thread, multithreaded apartment unless the thread already has a single-threaded one) and
/// keeps it for the thread's lifetime, so Startup() and ReinitDevice() work on any thread.
class PL_PLASMAAUDIOPLUGIN_DLL plMiniAudioSingleton
{
  PL_DECLARE_SINGLETON(plMiniAudioSingleton);

public:
  /// \brief Number of miniaudio listener slots the engine is created with.
  static constexpr plUInt8 MaxListeners = 4;

  plMiniAudioSingleton();
  ~plMiniAudioSingleton();

  void Startup();
  void Shutdown();

  /// \brief The miniaudio engine, or nullptr while none is usable (startup or a device re-init failed to create even
  /// an engine without output). Callers must not touch the engine when this returns nullptr.
  ma_engine* GetEngine() { return m_bInitialized ? &m_pData->m_Engine : nullptr; }

  /// \brief Whether the engine exists. False after a failed startup or a failed ReinitDevice(); it stays false then.
  bool IsEngineValid() const { return m_bInitialized; }

  /// \brief Whether the engine has to be recreated: the device stopped without us stopping it, or the
  /// requested output device changed. While no device is available the engine runs without output and this
  /// checks every few seconds whether a playback device is back. Audio thread only.
  bool IsDeviceReinitRequired();

  /// \brief Recreates the engine on the requested (or default) playback device.
  ///
  /// Every ma_sound / ma_node created on the old engine is invalid afterwards, so the caller must uninit
  /// them before this call. Legacy sound groups, master state and listener slots are restored here.
  /// Without a usable device the engine is recreated without output. Fails only when not even that engine
  /// can be created; the engine is then gone for good and GetEngine() returns nullptr.
  plResult ReinitDevice();

  /// \brief Names of the playback devices the context currently reports.
  void GetPlaybackDeviceNames(plDynamicArray<plString>& out_names) const;

  /// \brief Can be called before startup to load the configuration from a different file.
  /// Otherwise will automatically be loaded at startup with the default path.
  void LoadConfiguration(plStringView sFile);

  /// \brief By default the integration will auto-detect the platform (and thus the config) to use.
  /// Calling this before startup allows to override which configuration is used.
  void SetOverridePlatform(plStringView sPlatform);

  /// \brief Called by the middleware on every audio update with the time since the previous one.
  ///
  /// Applies master volume / mute / pause. Without a playback device nothing pulls audio from the engine, so this
  /// mixes and discards delta worth of frames: the engine clock, fades and voice ends then advance as with a device.
  void UpdateSound(plTime delta);

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

  /// \brief Default is 1, clamped to [1, MaxListeners]. Sets how many listener slots may be active (split screen game play).
  /// Slots at or above the count are disabled and ignore SetListener().
  void SetNumListeners(plUInt8 uiNumListeners);
  plUInt8 GetNumListeners() const;

  /// \brief In override mode only index -1 is accepted and drives slot 0.
  void SetListenerOverrideMode(bool bEnabled);

  /// \brief Updates listener slot iIndex. A slot takes part in spatialization (each voice uses the closest
  /// enabled slot) from its first update until ReleaseListenerSlot(); slot 0 is always enabled.
  void SetListener(plInt32 iIndex, const plVec3& vPosition, const plVec3& vForward, const plVec3& vUp, const plVec3& vVelocity);

  /// \brief Disables a listener slot until it is updated again. Slot 0 stays enabled.
  void ReleaseListenerSlot(plUInt8 uiSlot);

  /// \brief Position of listener slot 0.
  plVec3 GetListenerPosition() const { return m_Listeners[0].m_vPosition; }

  /// \brief Share of real time spent mixing (0 = idle, 1 = mixing takes as long as the audio it produces), smoothed.
  ///
  /// With a device: time spent in the device data callback divided by the duration of the frames it produced. Without
  /// one: time UpdateSound() spends mixing divided by the update delta. Any thread.
  float GetDspLoad() const { return m_fDspLoad.load(std::memory_order_relaxed); }

  /// \brief Duration of the last mix (device data callback, or the mix in UpdateSound() without a device) in
  /// microseconds. Any thread.
  float GetLastMixMicroseconds() const { return m_fLastMixMicroseconds.load(std::memory_order_relaxed); }

  /// \brief The allocator the heap memory of the miniaudio context, engine, device and sounds comes from, or nullptr
  /// before Startup(). Named "PlasmaAudio/miniaudio" in the memory tracker.
  plAllocator* GetMiniAudioAllocator() { return m_pData != nullptr ? &m_pData->m_Allocator : nullptr; }

  /// \brief miniaudio allocation callbacks backed by GetMiniAudioAllocator(), or nullptr before Startup(). Nodes created
  /// with null callbacks use miniaudio's default heap instead; pass these (to init and uninit alike) to track them.
  const ma_allocation_callbacks* GetAllocationCallbacks() const { return m_pData != nullptr ? &m_pData->m_AllocationCallbacks : nullptr; }

private:
  struct ListenerSlot
  {
    plVec3 m_vPosition = plVec3::MakeZero();
    plVec3 m_vForward = plVec3(1, 0, 0);
    plVec3 m_vUp = plVec3(0, 0, 1);
    plVec3 m_vVelocity = plVec3::MakeZero();
    bool m_bActive = false; ///< received a transform since it was last released
  };

  static void DeviceNotificationCallback(const ma_device_notification* pNotification);

  /// Replaces the device's data callback (device thread): forwards to m_OriginalDataCallback and measures it.
  static void TimedDataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 uiFrameCount);

  /// Folds one mix that took mixTime and produced fAudioSeconds of audio into the DSP load.
  void RecordMixTime(plTime mixTime, double fAudioSeconds);

  void OnOutputDeviceCVarChanged(const plCVarEvent& e);

  plResult InitEngine();
  void UninitEngine();
  void ApplyListenerSlot(plUInt8 uiSlot);
  void ApplyListenerEnabledStates();

  bool m_bInitialized = false;
  bool m_bContextInitialized = false;
  bool m_bListenerOverrideMode = false;
  bool m_bEnginePaused = false;
  bool m_bHeadless = false; ///< engine runs without a playback device
  plUInt8 m_uiNumListeners = 1;
  ListenerSlot m_Listeners[MaxListeners];

  // written by the device notification callback (device thread) and the cvar handler (main thread)
  std::atomic<bool> m_bDeviceLost = false;
  std::atomic<bool> m_bDeviceRerouted = false;
  std::atomic<bool> m_bDeviceStopExpected = false;
  std::atomic<bool> m_bDeviceChangeRequested = false;
  plTime m_LastReinitCheck;

  // written by the device thread (or UpdateSound without a device), read anywhere
  std::atomic<float> m_fDspLoad = 0.0f;
  std::atomic<float> m_fLastMixMicroseconds = 0.0f;

  /// the engine's device data callback, called by TimedDataCallback; set before the device starts
  ma_device_data_proc m_OriginalDataCallback = nullptr;

  struct Data
  {
    Data();

    /// declared first so it is destroyed last: the context and engine free into it until they are uninitialized
    plAllocatorWithPolicy<plAllocPolicyProxy, plAllocatorTrackingMode::AllocationStats> m_Allocator;
    ma_allocation_callbacks m_AllocationCallbacks;

    plMutex m_Mutex;

    ma_context m_Context;
    ma_engine m_Engine;
    plMaClockNode m_ClockNode;

    plHybridArray<SoundGroup, 4> m_SoundGroups;

    /// frames mixed by UpdateSound without a device: fraction carried to the next update, and the discarded output
    double m_fHeadlessFrameRemainder = 0.0;
    plDynamicArray<float> m_HeadlessScratch;

    /// copy of the PlasmaAudio.OutputDevice cvar, taken on the thread that sets the cvar
    plMutex m_OutputDeviceMutex;
    plString m_sRequestedOutputDevice;
  };

  plUniquePtr<Data> m_pData;
};
