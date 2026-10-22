#pragma once

#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationAsset.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.h>

class plEditorUiDockManager;

/// \brief Audio Attenuation editor: the engine-drawn attenuation panel over the property grid.
///
/// The panel: "AudioAttenuationAssetDockWidget" (the curve over the asset's status and grid).
class plAudioAttenuationAssetDocumentWindow : public plDocumentWindow
{
public:
  plAudioAttenuationAssetDocumentWindow(plDocument* pDocument);

  /// The default layout of the panels: the one panel fills the document.
  static void ArrangePanels(plEditorUiDockManager& ref_panels);
};
