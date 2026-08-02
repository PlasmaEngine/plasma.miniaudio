#pragma once

#include <EditorPluginMiniAudio/SoundAsset/MiniAudioSoundAsset.h>
#include <Foundation/Basics.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.moc.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

class QLabel;
class QScrollArea;
class QtImageWidget;

class plMiniAudioSoundAssetDocumentWindow : public plQtDocumentWindow
{
  Q_OBJECT

public:
  plMiniAudioSoundAssetDocumentWindow(plDocument* pDocument);

  virtual const char* GetGroupName() const { return "MiniAudioSoundAsset"; }

private:
  plMiniAudioSoundAssetDocument* m_pAssetDoc;
};
