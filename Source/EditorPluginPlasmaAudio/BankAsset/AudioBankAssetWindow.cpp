#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAssetWindow.h>
#include <EditorPluginPlasmaAudio/BankAsset/AudioBankDashboardView.h>

namespace
{
  constexpr plInt64 s_iPollMs = 250;
} // namespace

plAudioBankAssetDocumentWindow::plAudioBankAssetDocumentWindow(plDocument* pDocument)
  : plDocumentWindow(pDocument)
{
  plAudioBankAssetDocument* pBankDocument = static_cast<plAudioBankAssetDocument*>(pDocument);

  SetMenuBarActions("AudioBankAssetMenuBar");
  SetToolBarActions("AudioBankAssetToolBar");

  // Audio Banks: the dashboard
  m_pDashboard = AddPanel<plEditorUiAudioBankDashboard>({"AudioBankDashboardDockWidget", "Audio Banks"}, pBankDocument);

  // Bank Properties: the bank's status and properties
  AddPropertiesPanel({"AudioBankAssetDockWidget", "Bank Properties", plEditorUiDockRole::Right});

  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);

  // the loaded banks and their progress come from the engine process, which answers every request
  m_Timers.Start(plTime::MakeFromMilliseconds(s_iPollMs), [this, pBankDocument]()
    {
      pBankDocument->SendBankCommand(plAudioBankCommand::Poll);
      m_pDashboard->OnTimer(); },
    true);
}

plAudioBankAssetDocumentWindow::~plAudioBankAssetDocumentWindow()
{
  m_Timers.StopAll();
}

void plAudioBankAssetDocumentWindow::ArrangePanels(plEditorUiDockManager& ref_panels)
{
  ref_panels.DockPanel("AudioBankAssetDockWidget", "AudioBankDashboardDockWidget", plEditorUiDockSide::Right, 0.22f);
}
