#pragma once

#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationAsset.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.moc.h>

/// \brief Audio Attenuation editor: the engine-drawn attenuation panel over the property grid.
class plAudioAttenuationAssetDocumentWindow : public plQtDocumentWindow
{
  Q_OBJECT

public:
  plAudioAttenuationAssetDocumentWindow(plDocument* pDocument);

  virtual const char* GetGroupName() const { return "AudioAttenuationAsset"; }
};
