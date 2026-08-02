#pragma once

#include <AudioSystem/Mixer/AudioMixerData.h>
#include <EditorPluginMiniAudio/EditorPluginMiniAudioDLL.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Strings/String.h>
#include <MiniAudio/miniaudio.h>

#include <atomic>

/// \brief Pass-through metering node used for the preview engine's master and bus meters.
struct plAudioPreviewMeterNode
{
  ma_node_base m_Base; // must be first
  plUInt32 m_uiChannels = 0;
  std::atomic<float> m_fPeak = 0.0f;
  std::atomic<float> m_fRms = 0.0f;
};

/// \brief Editor-side audio preview: one private miniaudio engine shared by the audio tools.
///
/// When a mixer description is installed (the Mixer Console pushes the one it is editing), the
/// service mirrors its bus hierarchy as sound groups with a meter on each bus, and auditioned
/// sounds are routed into the bus their event targets. That makes the Mixer Console's meters
/// respond to auditioning an event, and makes bus volumes audible in the preview.
///
/// Without a mixer, playback still works and is routed straight to the output.
///
/// This is editor-only preview: insert effects, sends and snapshots are not applied - play the
/// scene to hear the full mix.
class PL_EDITORPLUGINMINIAUDIO_DLL plAudioPreviewService
{
public:
  static plAudioPreviewService& GetSingleton();

  ~plAudioPreviewService();

  /// \brief Opens the audio device on first use. Returns false when no device is available.
  bool EnsureInit();

  /// \brief Releases voices, buses and the device.
  void Shutdown();

  // --- mixer ---

  /// \brief Installs (or replaces) the previewed mixer. Stops any playing preview first.
  void SetMixer(const plAudioMixerDesc& desc);

  /// \brief Removes the previewed mixer; playback falls back to routing straight to the output.
  void ClearMixer();

  PL_NODISCARD bool HasMixer() const { return !m_Buses.IsEmpty(); }
  PL_NODISCARD plUInt32 GetBusCount() const { return m_Buses.GetCount(); }

  /// \brief Index of the bus with that name, or 0 (master) when unknown/empty.
  PL_NODISCARD plUInt32 ResolveBus(plStringView sBusName) const;

  /// \brief Current level of a bus. Returns zeros for an unknown index or when no mixer is set.
  void GetBusMeter(plUInt32 uiBusIndex, float& out_fPeak, float& out_fRms) const;

  /// \brief Level of the whole preview output.
  PL_NODISCARD float GetMasterPeak() const;

  // --- playback ---

  /// \brief Starts one preview voice. \a sBusName selects the mixer bus (empty = master).
  plResult PlayFile(const char* szAbsPath, float fVolume, float fPitch, double fDelaySeconds, bool bLoop, plStringView sBusName);

  void StopAll();

  PL_NODISCARD bool IsAnythingPlaying() const;

private:
  plAudioPreviewService() = default;

  struct Bus
  {
    PL_DISALLOW_COPY_AND_ASSIGN(Bus);
    Bus() = default;

    plString m_sName;
    plUInt8 m_uiParent = 0xFF;
    bool m_bIsAux = false;
    ma_sound_group m_Group = {};
    plAudioPreviewMeterNode m_Meter = {};
    bool m_bGroupValid = false;
    bool m_bMeterValid = false;
  };

  void DestroyBuses();

  bool m_bInitialized = false;
  bool m_bMasterMeterValid = false;

  ma_engine m_Engine = {};
  plAudioPreviewMeterNode m_MasterMeter;  // used when no mixer is installed
  plDeque<Bus> m_Buses;                   // deque: stable addresses for miniaudio nodes
  plDeque<ma_sound> m_Sounds;
};
