#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetStatusIndicator.moc.h>
#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAssetWindow.moc.h>
#include <EditorPluginPlasmaAudio/BankAsset/AudioBankDashboardView.h>
#include <GuiFoundation/ActionViews/MenuBarActionMapView.moc.h>
#include <GuiFoundation/ActionViews/ToolBarActionMapView.moc.h>
#include <GuiFoundation/DockPanels/DocumentPanel.moc.h>
#include <GuiFoundation/EngineUi/EngineUiContentWidget.h>

#include <QTimer>

namespace
{
  constexpr int s_iPollMs = 250;
} // namespace

plAudioBankAssetDocumentWindow::plAudioBankAssetDocumentWindow(plDocument* pDocument)
  : plQtDocumentWindow(pDocument)
{
  plAudioBankAssetDocument* pBankDocument = static_cast<plAudioBankAssetDocument*>(pDocument);

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

  // Central: the dashboard
  {
    plQtDocumentPanel* pPanel = new plQtDocumentPanel(this, pDocument);
    pPanel->setObjectName("AudioBankDashboardDockWidget");
    pPanel->setWindowTitle("Audio Banks");

    plQtEngineUiContentWidget* pDashboardWidget = nullptr;
    m_pDashboard = plQtEngineUiContentWidget::Create<plEditorUiAudioBankDashboard>(pPanel, "AudioBankDashboard", &pDashboardWidget, pBankDocument);

    pPanel->setWidget(pDashboardWidget, ads::CDockWidget::ForceNoScrollArea);
    m_pDockManager->setCentralWidget(pPanel);
  }

  // Right: the bank's status and properties
  {
    plQtDocumentPanel* pPanel = new plQtDocumentPanel(this, pDocument);
    pPanel->setObjectName("AudioBankAssetDockWidget");
    pPanel->setWindowTitle("Bank Properties");

    QWidget* pWidget = new QWidget();
    pWidget->setObjectName("Group");
    pWidget->setLayout(new QVBoxLayout());
    pWidget->setContentsMargins(0, 0, 0, 0);
    pWidget->layout()->setContentsMargins(0, 0, 0, 0);
    pWidget->layout()->addWidget(new plQtAssetStatusIndicator(pBankDocument));
    pWidget->layout()->addWidget(new plQtPropertyGridWidget(pWidget, pDocument));

    pPanel->setWidget(pWidget, ads::CDockWidget::ForceNoScrollArea);
    m_pDockManager->addDockWidgetTab(ads::RightDockWidgetArea, pPanel);
  }

  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);

  // the loaded banks and their progress come from the engine process, which answers every request
  m_pPollTimer = new QTimer(this);
  m_pPollTimer->setInterval(s_iPollMs);
  connect(m_pPollTimer, &QTimer::timeout, this, [this, pBankDocument]()
    {
      pBankDocument->SendBankCommand(plAudioBankCommand::Poll);
      m_pDashboard->OnTimer(); });
  m_pPollTimer->start();

  FinishWindowCreation();
}
