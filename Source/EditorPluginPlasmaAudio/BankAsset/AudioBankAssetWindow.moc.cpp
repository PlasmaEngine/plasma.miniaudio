#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetStatusIndicator.moc.h>
#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAssetWindow.moc.h>
#include <GuiFoundation/ActionViews/MenuBarActionMapView.moc.h>
#include <GuiFoundation/ActionViews/ToolBarActionMapView.moc.h>
#include <GuiFoundation/DockPanels/DocumentPanel.moc.h>

plAudioBankAssetDocumentWindow::plAudioBankAssetDocumentWindow(plDocument* pDocument)
  : plQtDocumentWindow(pDocument)
{
  // Menu Bar
  {
    plQtMenuBarActionMapView* pMenuBar = static_cast<plQtMenuBarActionMapView*>(menuBar());
    plActionContext context;
    context.m_sMapping = "AudioBankAssetMenuBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pMenuBar->SetActionContext(context);
  }

  // Tool Bar
  {
    plQtToolBarActionMapView* pToolBar = new plQtToolBarActionMapView("Toolbar", this);
    plActionContext context;
    context.m_sMapping = "AudioBankAssetToolBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pToolBar->SetActionContext(context);
    pToolBar->setObjectName("AudioBankAssetWindowToolBar");
    addToolBar(pToolBar);
  }

  // Central: bank contents
  {
    plQtDocumentPanel* pPanel = new plQtDocumentPanel(this, pDocument);
    pPanel->setObjectName("AudioBankAssetDockWidget");
    pPanel->setWindowTitle("Bank Contents");

    plQtPropertyGridWidget* pPropertyGrid = new plQtPropertyGridWidget(pPanel, pDocument);

    QWidget* pWidget = new QWidget();
    pWidget->setObjectName("Group");
    pWidget->setLayout(new QVBoxLayout());
    pWidget->setContentsMargins(0, 0, 0, 0);
    pWidget->layout()->setContentsMargins(0, 0, 0, 0);
    pWidget->layout()->addWidget(new plQtAssetStatusIndicator((plAssetDocument*)GetDocument()));
    pWidget->layout()->addWidget(pPropertyGrid);

    pPanel->setWidget(pWidget, ads::CDockWidget::ForceNoScrollArea);
    m_pDockManager->setCentralWidget(pPanel);
  }

  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);

  FinishWindowCreation();
}
