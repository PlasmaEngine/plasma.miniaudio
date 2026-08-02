#pragma once

#include <AudioSystem/Mixer/AudioMixerData.h>
#include <EditorPluginMiniAudio/EditorPluginMiniAudioDLL.h>
#include <Foundation/Strings/String.h>

/// \brief Feeds the shared "AudioSystem.*" dynamic string enums with the names this backend knows,
/// so the audio components offer dropdowns instead of free-text fields.
///
/// Values are only ever *added*: these enums are also user-editable (the editor backs unknown
/// dynamic enums with a project file), and other backends - FMOD in particular - contribute their
/// own control names. Nothing here removes what someone else registered.
class PL_EDITORPLUGINMINIAUDIO_DLL plAudioEnumRegistry
{
public:
  /// \brief Registers bus names ("AudioSystem.Bus" / "AudioSystem.OutputBus"), aux-bus environment
  /// names ("AudioSystem.Environment") and snapshot names ("AudioSystem.Snapshot").
  static void RegisterMixerNames(const plAudioMixerDesc& desc);

  /// \brief Registers a mixer bus name ("AudioSystem.Bus"). Includes aux buses — this is the list
  /// snapshot overrides pick from, and a snapshot may legitimately target a reverb return.
  static void RegisterBusName(plStringView sName);

  /// \brief Registers a bus an event may output to ("AudioSystem.OutputBus"). Aux buses must never
  /// be registered here: they are fed by sends and environment routing, so an event outputting into
  /// one would be permanently 100% wet with no dry signal.
  static void RegisterOutputBusName(plStringView sName);

  /// \brief Registers a reverb-zone name ("AudioSystem.Environment").
  static void RegisterEnvironmentName(plStringView sName);

  /// \brief Registers a snapshot name ("AudioSystem.Snapshot").
  static void RegisterSnapshotName(plStringView sName);

  /// \brief Registers an event parameter name, offered as an RTPC ("AudioSystem.Rtpc").
  static void RegisterRtpcName(plStringView sName);

  /// \brief Registers a switch state name ("AudioSystem.SwitchState").
  static void RegisterSwitchStateName(plStringView sName);
};
