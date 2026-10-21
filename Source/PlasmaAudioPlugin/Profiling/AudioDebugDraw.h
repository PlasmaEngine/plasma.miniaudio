#pragma once

#include <PlasmaAudioPlugin/Profiling/AudioDebugSnapshot.h>

class plView;
struct plRenderWorldExtractionEvent;

/// \brief The Audio view mode: draws the playing emitters, their attenuation ranges, the rays from the listener and the
/// environment zones over every view whose render mode is plViewRenderMode::Audio, and over the main view while the
/// Audio.Debug.Draw cvar is on. Audio.Debug.Layers picks the layers.
///
/// The audio thread publishes a snapshot (Publish) while IsWanted() says a view drew recently; views draw from the latest
/// snapshot before they are extracted.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioDebugDraw
{
public:
  enum Layer : plUInt32
  {
    Emitters = PL_BIT(0),
    Ranges = PL_BIT(1),
    Occlusion = PL_BIT(2),
    Environments = PL_BIT(3),
    Labels = PL_BIT(4),
    All = Emitters | Ranges | Occlusion | Environments | Labels,
  };

  static void Startup();
  static void Shutdown();

  /// \brief Whether a view drew the overlay within the last second. Any thread.
  static bool IsWanted();

  /// \brief Hands the audio thread's latest snapshot to the views.
  static void Publish(plAudioDebugSnapshot&& snapshot);

private:
  static void OnExtractionEvent(const plRenderWorldExtractionEvent& e);
  static void DrawView(const plView* pView, const plAudioDebugSnapshot& snapshot, plUInt32 uiLayers);
};
