#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/SoundAsset/MiniAudioSoundAssetWindow.h>

plMiniAudioSoundAssetDocumentWindow::plMiniAudioSoundAssetDocumentWindow(plDocument* pDocument)
  : plDocumentWindow(pDocument)
{
  SetMenuBarActions("MiniAudioSoundAssetMenuBar");
  SetToolBarActions("MiniAudioSoundAssetToolBar");

  AddPropertiesPanel({"Properties", "Sound Properties", plEditorUiDockRole::Right});

  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);

  m_pAssetDoc = static_cast<plMiniAudioSoundAssetDocument*>(pDocument);
}
