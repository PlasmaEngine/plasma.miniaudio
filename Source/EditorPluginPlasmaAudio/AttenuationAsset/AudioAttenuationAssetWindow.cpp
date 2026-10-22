#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/EngineShell/AssetDocumentContent.h>
#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationAssetWindow.h>
#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationView.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioShellPanels.h>

plAudioAttenuationAssetDocumentWindow::plAudioAttenuationAssetDocumentWindow(plDocument* pDocument)
  : plDocumentWindow(pDocument)
{
  SetMenuBarActions("AudioAttenuationAssetMenuBar");
  SetToolBarActions("AudioAttenuationAssetToolBar");

  // Attenuation: the curve over the asset's status and grid
  {
    plEditorUiAudioShellStack* pMain = AddPanel<plEditorUiAudioShellStack>({"AudioAttenuationAssetDockWidget", "Attenuation"});
    pMain->SetSeparators(true);

    auto* pCurve = static_cast<plEditorUiAudioAttenuation*>(pMain->AddOwnedPart(PL_DEFAULT_NEW(plEditorUiAudioAttenuation, pDocument), 0.0f));
    pCurve->SetListenerDistance(10.0f);

    // the curve takes three fifths, but never less than it needs for its labels; the grid gets the rest
    pMain->SetExtentSource(pCurve, [](float fLength)
      { return plMath::Max(plEditorUiAudioAttenuation::GetMinimumHeight() * 1.4f, fLength * 0.6f); });

    pMain->AddOwnedPart(PL_DEFAULT_NEW(plEditorUiAssetProperties, static_cast<plAssetDocument*>(pDocument), this), 0.0f);
  }

  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);
}

void plAudioAttenuationAssetDocumentWindow::ArrangePanels(plEditorUiDockManager& ref_panels)
{
  PL_IGNORE_UNUSED(ref_panels);
}
