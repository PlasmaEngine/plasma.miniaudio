#pragma once

#include <EditorPluginPlasmaAudio/SoundAsset/MiniAudioSoundAsset.h>
#include <Foundation/Basics.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

/// \brief The legacy sound asset's window: its properties.
class plMiniAudioSoundAssetDocumentWindow : public plDocumentWindow
{
public:
  plMiniAudioSoundAssetDocumentWindow(plDocument* pDocument);

private:
  plMiniAudioSoundAssetDocument* m_pAssetDoc;
};
