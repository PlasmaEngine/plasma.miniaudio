#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationAssetWindow.moc.h>
#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationView.h>
#include <GuiFoundation/ActionViews/MenuBarActionMapView.moc.h>
#include <GuiFoundation/ActionViews/ToolBarActionMapView.moc.h>
#include <GuiFoundation/DockPanels/DocumentPanel.moc.h>
#include <GuiFoundation/EngineUi/EngineUiContentWidget.h>

#include <QtMath>

plAudioAttenuationAssetDocumentWindow::plAudioAttenuationAssetDocumentWindow(plDocument* pDocument)
  : plQtDocumentWindow(pDocument)
{
  // Menu Bar
  {
    plQtMenuBarActionMapView* pMenuBar = static_cast<plQtMenuBarActionMapView*>(menuBar());
    plActionContext context;
    context.m_sMapping = "AudioAttenuationAssetMenuBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pMenuBar->SetActionContext(context);
  }

  // Tool Bar
  {
    plQtToolBarActionMapView* pToolBar = new plQtToolBarActionMapView("Toolbar", this);
    plActionContext context;
    context.m_sMapping = "AudioAttenuationAssetToolBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pToolBar->SetActionContext(context);
    pToolBar->setObjectName("AudioAttenuationAssetWindowToolBar");
    addToolBar(pToolBar);
  }

  // Central: the curve over the property grid
  {
    plQtDocumentPanel* pPanel = new plQtDocumentPanel(this, pDocument);
    pPanel->setObjectName("AudioAttenuationAssetDockWidget");
    pPanel->setWindowTitle("Attenuation");

    QWidget* pWidget = new QWidget();
    pWidget->setObjectName("Group");
    QVBoxLayout* pLayout = new QVBoxLayout(pWidget);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(0);

    plQtEngineUiContentWidget* pCurveWidget = nullptr;
    plEditorUiAudioAttenuation* pCurve = plQtEngineUiContentWidget::Create<plEditorUiAudioAttenuation>(pWidget, "AudioAttenuationCurve", &pCurveWidget, pDocument);
    pCurveWidget->setMinimumHeight(qCeil(plEditorUiAudioAttenuation::GetMinimumHeight() * 1.4f * pCurveWidget->GetUiScale()));
    pCurve->SetListenerDistance(10.0f);
    pLayout->addWidget(pCurveWidget, 3);

    pLayout->addWidget(new plQtPropertyGridWidget(pWidget, pDocument), 2);

    pPanel->setWidget(pWidget, ads::CDockWidget::ForceNoScrollArea);
    m_pDockManager->setCentralWidget(pPanel);
  }

  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);

  FinishWindowCreation();
}
