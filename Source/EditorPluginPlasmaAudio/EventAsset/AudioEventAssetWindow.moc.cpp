#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/Assets/EngineAssetPicker.h>
#include <EditorFramework/Assets/AssetStatusIndicator.moc.h>
#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationView.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineViews.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAssetWindow.moc.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventDeckView.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventTimelineView.h>
#include <GuiFoundation/EngineUi/EngineUiControlStrip.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/IO/OSFile.h>
#include <EditorPluginPlasmaAudio/Preferences/MiniAudioPreferences.h>
#include <EditorPluginPlasmaAudio/Preview/AudioEnumRegistry.h>
#include <EnginePluginPlasmaAudio/Audition/AudioAuditionMessages.h>
#include <Foundation/IO/MemoryStream.h>
#include <GuiFoundation/ActionViews/MenuBarActionMapView.moc.h>
#include <GuiFoundation/ActionViews/ToolBarActionMapView.moc.h>
#include <GuiFoundation/DockPanels/DocumentPanel.moc.h>
#include <GuiFoundation/UIServices/DynamicStringEnum.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

#include <QFileDialog>
#include <QGridLayout>
#include <QSplitter>
#include <QTimer>
#include <QtMath>

namespace
{
  void GatherChildrenOfProperty(const plDocumentObject* pParent, const char* szProperty, plHybridArray<const plDocumentObject*, 8>& out_children)
  {
    out_children.Clear();

    if (pParent == nullptr)
      return;

    for (const plDocumentObject* pChild : pParent->GetChildren())
    {
      if (pChild->GetParentProperty().IsEqual(szProperty))
      {
        out_children.PushBack(pChild);
      }
    }
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// plAudioEventAssetDocumentWindow
//////////////////////////////////////////////////////////////////////////

plAudioEventAssetDocumentWindow::plAudioEventAssetDocumentWindow(plDocument* pDocument)
  : plQtDocumentWindow(pDocument)
{
  m_pAssetDoc = static_cast<plAudioEventAssetDocument*>(pDocument);

  // Menu Bar
  {
    plQtMenuBarActionMapView* pMenuBar = static_cast<plQtMenuBarActionMapView*>(menuBar());
    plActionContext context;
    context.m_sMapping = "AudioEventAssetMenuBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pMenuBar->SetActionContext(context);
  }

  // Tool Bar
  {
    plQtToolBarActionMapView* pToolBar = new plQtToolBarActionMapView("Toolbar", this);
    plActionContext context;
    context.m_sMapping = "AudioEventAssetToolBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pToolBar->SetActionContext(context);
    pToolBar->setObjectName("AudioEventAssetWindowToolBar");
    addToolBar(pToolBar);
  }

  // Central
  {
    QWidget* pCentralWidget = new QWidget(this);

    QVBoxLayout* pCentralLayout = new QVBoxLayout(pCentralWidget);
    pCentralLayout->setContentsMargins(0, 0, 0, 0);
    pCentralLayout->setSpacing(0);

    CreateEngineCentralWidget(pCentralWidget, pCentralLayout);

    plQtDocumentPanel* pCentral = new plQtDocumentPanel(this, pDocument);
    pCentral->setObjectName("AudioEventTimelineDockWidget");
    pCentral->setWindowTitle("Audio Event");
    pCentral->setWidget(pCentralWidget, ads::CDockWidget::ForceNoScrollArea);
    m_pDockManager->setCentralWidget(pCentral);
  }

  // Right dock: event macros (root properties, pinned) + asset status
  {
    plQtDocumentPanel* pPropertyPanel = new plQtDocumentPanel(this, pDocument);
    pPropertyPanel->setObjectName("AudioEventAssetDockWidget");
    pPropertyPanel->setWindowTitle("Event Macros");
    pPropertyPanel->show();

    plQtPropertyGridWidget* pPropertyGrid = new plQtPropertyGridWidget(pPropertyPanel, pDocument, false);

    plDeque<const plDocumentObject*> selection;
    selection.PushBack(GetPropertiesObject());
    pPropertyGrid->SetSelection(selection);

    QWidget* pWidget = new QWidget();
    pWidget->setObjectName("Group");
    pWidget->setLayout(new QVBoxLayout());
    pWidget->setContentsMargins(0, 0, 0, 0);
    pWidget->layout()->setContentsMargins(0, 0, 0, 0);
    pWidget->layout()->addWidget(new plQtAssetStatusIndicator((plAssetDocument*)GetDocument()));
    pWidget->layout()->addWidget(pPropertyGrid);

    // the attenuation curve under the macros; its listener marker is the Distance the audition plays at
    m_pAttenuationView = plQtEngineUiContentWidget::Create<plEditorUiAudioAttenuation>(pWidget, "AudioEventAttenuation", &m_pAttenuationWidget, pDocument);
    m_pAttenuationWidget->SetFixedLogicalHeight(plEditorUiAudioAttenuation::GetMinimumHeight() + 20.0f);
    m_pAttenuationView->SetListenerDistance(m_pTimelineView->GetAuditionValue("Distance", 0.0f));
    m_pAttenuationView->m_OnListenerDistanceChanged = [this](float fDistance)
    { m_pTimelineView->SetAuditionValue("Distance", fDistance); };
    pWidget->layout()->addWidget(m_pAttenuationWidget);

    // MESSAGES TO GAME: what the audition's markers, sustain points and key-offs would send (the engine shell shows it
    // between AUTOMATION and DETAILS, see plAudioShellPanels)
    plQtEngineUiContentWidget* pMessagesWidget = nullptr;
    m_pMessagesView = plQtEngineUiContentWidget::Create<plEditorUiAudioMessageList>(pWidget, "AudioEventMessages", &pMessagesWidget, "Messages to game", "Markers the audition passes show here, as the entity's game object gets them.");
    pMessagesWidget->SetFixedLogicalHeight(plEditorUiAudioMessageList::GetMinimumHeight() + 4.0f * 20.0f);
    pWidget->layout()->addWidget(pMessagesWidget);

    pPropertyPanel->setWidget(pWidget, ads::CDockWidget::ForceNoScrollArea);

    m_pDockManager->addDockWidgetTab(ads::RightDockWidgetArea, pPropertyPanel);
  }

  // the first instrument, so Details shows what the deck edits (Event Macros shows the event); an empty event selects
  // its properties
  const plDocumentObject* pInitial = GetPropertiesObject();
  for (const plDocumentObject* pChild : pInitial->GetChildren())
  {
    if (pChild->GetParentProperty().IsEqual("Instruments"))
    {
      pInitial = pChild;
      break;
    }
  }
  pDocument->GetSelectionManager()->SetSelection(pInitial);

  GetDocument()->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plAudioEventAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plAudioEventAssetDocumentWindow::StructureEventHandler, this));
  GetDocument()->GetSelectionManager()->m_Events.AddEventHandler(plMakeDelegate(&plAudioEventAssetDocumentWindow::SelectionEventHandler, this));

  // the audition plays in the engine process; while it plays, its status is polled for the playhead and the meter
  m_AuditionSubscription = m_pAssetDoc->m_AuditionEvents.AddEventHandler(plMakeDelegate(&plAudioEventAssetDocumentWindow::OnAuditionStatus, this));

  m_pMeterTimer = new QTimer(this);
  m_pMeterTimer->setInterval(40);
  connect(m_pMeterTimer, &QTimer::timeout, this, [this]()
    {
      if (m_bAuditioning)
        SendAuditionCommand(plAudioAuditionCommand::Poll);
    });
  m_pMeterTimer->start();

  FinishWindowCreation();

  QueueRefresh();
}

void plAudioEventAssetDocumentWindow::CreateEngineCentralWidget(QWidget* pCentralWidget, QVBoxLayout* pCentralLayout)
{
  using plEditorUi::Icon;

  // toolbar row: audition, edit buttons, the OUT meter
  {
    QWidget* pRow = new QWidget(pCentralWidget);
    QHBoxLayout* pRowLayout = new QHBoxLayout(pRow);
    pRowLayout->setContentsMargins(0, 0, 0, 0);
    pRowLayout->setSpacing(0);

    m_pToolStrip = new plQtEngineControlStrip(pRow);
    m_pToolStrip->setObjectName("AudioEventToolStrip");
    m_uiPlay = m_pToolStrip->AddIconButton(Icon::Play, "Plays the event from the timeline cursor through the engine process's event manager and mixer, as the game does (respects M mute and S solo). Space in the timeline plays and stops.");
    m_uiStop = m_pToolStrip->AddIconButton(Icon::Stop, "Stops the audition with the event's stop fade");
    m_uiKeyOff = m_pToolStrip->AddButton("Key off", "Releases the sustain point the audition waits at, as the game does with plMsgAudioKeyOff (or the trigger component's KeyOff)", 64.0f);
    m_pToolStrip->SetEnabled(m_uiKeyOff, false);
    m_pToolStrip->AddChip("RUNTIME AUDITION", plEditorUi::Accent(),
      "The audition runs in the engine process: clip build settings, fades, loop region, retrigger, automation, parameter ranges, voice limits and the mixer's buses all apply. Not yet: 3D position (P4) and switch states.");
    m_pToolStrip->AddSeparator();
    m_pToolStrip->AddLabel("Seed", -1.0f);
    m_uiSeedField = m_pToolStrip->AddNumberField("#", "", "1", "The seed of the random playlist picks, volumes and pitches: the same seed plays the same takes", 70.0f);
    m_uiNewSeed = m_pToolStrip->AddIconButton(Icon::Repeat, "A new seed");
    m_uiFit = m_pToolStrip->AddIconButton(Icon::FitWidth, "Fits the timeline to the event (F in the timeline; Ctrl+wheel zooms)");
    m_pToolStrip->AddSeparator();
    m_uiAddInstrument = m_pToolStrip->AddIconTextButton(Icon::Add, "Instrument", "Adds an instrument track (fill its playlist with '+ Add Files...' in the deck; double-click a track header to rename)");
    m_uiRemoveSelected = m_pToolStrip->AddIconTextButton(Icon::Remove, "Remove Selected", "Removes the selected instrument / parameter / automation");
    m_pToolStrip->AddSpacer();
    m_pToolStrip->AddLabel("OUT", -1.0f);

    m_pToolStrip->m_OnChanged = [this](plUInt32 uiControl, plInt32 iValue)
    {
      PL_IGNORE_UNUSED(iValue);

      if (uiControl == m_uiPlay)
        OnPlayEvent();
      else if (uiControl == m_uiStop)
        OnStopEvent();
      else if (uiControl == m_uiKeyOff)
        OnKeyOff();
      else if (uiControl == m_uiSeedField)
      {
        plInt64 iSeed = m_uiSeed;
        plConversionUtils::StringToInt64(m_pToolStrip->GetText(uiControl), iSeed).IgnoreResult();
        SetSeed(static_cast<plUInt32>(plMath::Clamp<plInt64>(iSeed, 0, 0xFFFFFFFF)));
      }
      else if (uiControl == m_uiNewSeed)
        SetSeed(static_cast<plUInt32>(static_cast<plUInt64>(plTime::Now().GetMicroseconds()) % 100000));
      else if (uiControl == m_uiFit && m_pTimelineView != nullptr)
        m_pTimelineView->FitView();
      else if (uiControl == m_uiAddInstrument)
        OnAddInstrument();
      else if (uiControl == m_uiRemoveSelected)
        OnRemoveSelected();
    };

    pRowLayout->addWidget(m_pToolStrip, 1);

    plQtEngineUiContentWidget* pMeterWidget = nullptr;
    m_pMeterView = plQtEngineUiContentWidget::Create<plEditorUiAudioMeter>(pRow, "AudioEventMeter", &pMeterWidget);
    const plVec2 vMeter = plEditorUiAudioMeter::GetSize();
    pMeterWidget->setFixedWidth(qCeil(vMeter.x * pMeterWidget->GetUiScale()));
    pMeterWidget->SetFixedLogicalHeight(vMeter.y);
    pRowLayout->addWidget(pMeterWidget);

    pCentralLayout->addWidget(pRow);
  }

  QSplitter* pSplitter = new QSplitter(Qt::Vertical, pCentralWidget);
  pSplitter->setHandleWidth(1);
  pSplitter->setChildrenCollapsible(false);

  // parameter tabs and the timeline
  {
    plQtEngineUiContentWidget* pTimelineWidget = nullptr;
    m_pTimelineView = plQtEngineUiContentWidget::Create<plEditorUiAudioEventTimeline>(pSplitter, "AudioEventTimeline", &pTimelineWidget, GetDocument());
    m_pTimelineView->m_OnAddParameter = [this]()
    { OnAddParameter(); };
    m_pTimelineView->m_OnTogglePlay = [this]()
    { OnTogglePlay(); };
    m_pTimelineView->m_OnOpenEvent = [this](const plString& sEventGuid)
    { OpenNestedEvent(sEventGuid); };
    m_pTimelineView->m_OnAuditionValueChanged = [this](const plString& sName, float fValue)
    {
      if (sName.IsEqual_NoCase("Distance") && m_pAttenuationView != nullptr)
        m_pAttenuationView->SetListenerDistance(fValue);

      OnAuditionParameterChanged(sName, fValue);
    };
    pSplitter->addWidget(pTimelineWidget);
  }

  // deck: INSTRUMENT and PLAYBACK above, AUTOMATION and DETAILS below
  {
    QWidget* pDeck = new QWidget(pSplitter);
    QGridLayout* pDeckLayout = new QGridLayout(pDeck);
    pDeckLayout->setContentsMargins(0, 0, 0, 0);
    pDeckLayout->setSpacing(0);
    pDeckLayout->setColumnStretch(0, 1);
    pDeckLayout->setColumnStretch(1, 1);

    m_pDeckView = plQtEngineUiContentWidget::Create<plEditorUiAudioEventDeck>(pDeck, "AudioEventDeck", &m_pDeckWidget, GetDocument());
    m_pDeckWidget->SetFixedLogicalHeight(m_pDeckView->GetContentHeight());
    m_pDeckView->m_OnInstrumentPicked = [this](const plUuid& instrumentGuid)
    {
      m_ActiveInstrumentGuid = instrumentGuid;
      if (const plDocumentObject* pObject = GetDocument()->GetObjectManager()->GetObject(instrumentGuid))
        GetDocument()->GetSelectionManager()->SetSelection(pObject);
    };
    m_pDeckView->m_OnAddFiles = [this]()
    { OnAddFilesToInstrument(); };
    m_pDeckView->m_OnAddClip = [this]()
    { OnAddClipToInstrument(); };
    m_pDeckView->m_OnAddEvent = [this]()
    { OnAddEventToInstrument(); };
    m_pDeckView->m_OnAddSilence = [this]()
    { OnAddPlaylistEntry(); };
    m_pDeckView->m_OnPickEntryClip = [this](const plUuid& entryGuid)
    { PickEntryClip(entryGuid); };
    m_pDeckView->m_OnAuditionEntry = [this](const plUuid& entryGuid)
    { StartAudition(entryGuid, 0.0); };
    pDeckLayout->addWidget(m_pDeckWidget, 0, 0, 1, 2);

    plQtEngineUiContentWidget* pAutomationWidget = nullptr;
    m_pAutomationView = plQtEngineUiContentWidget::Create<plEditorUiAudioEventAutomation>(pDeck, "AudioEventAutomation", &pAutomationWidget, GetDocument());
    m_pAutomationView->m_OnAddAutomation = [this](plUInt8 uiTarget, const plString& sParameter)
    { OnAddAutomationFor(uiTarget, sParameter); };
    pAutomationWidget->setMinimumHeight(qCeil(plEditorUiAudioEventAutomation::GetMinimumHeight() * pAutomationWidget->GetUiScale()));
    pDeckLayout->addWidget(pAutomationWidget, 1, 0);

    // MESSAGES TO GAME: what the audition's markers, sustain points and key-offs would send

    // DETAILS · selection: the property grid of the selected object
    QWidget* pDetails = new QWidget(pDeck);
    QVBoxLayout* pDetailsLayout = new QVBoxLayout(pDetails);
    pDetailsLayout->setContentsMargins(0, 0, 0, 0);
    pDetailsLayout->setSpacing(0);
    plQtEngineUiContentWidget* pDetailsHeader = nullptr;
    plQtEngineUiContentWidget::Create<plEditorUiAudioSectionHeader>(pDetails, "AudioEventDetailsHeader", &pDetailsHeader, "Details \xC2\xB7 selection", plAudioEngineUi::AudioColor());
    pDetailsHeader->SetFixedLogicalHeight(plEditorUiAudioSectionHeader::GetHeight());
    pDetailsLayout->addWidget(pDetailsHeader);
    pDetailsLayout->addWidget(new plQtPropertyGridWidget(pDetails, GetDocument()), 1);
    pDeckLayout->addWidget(pDetails, 1, 1);
    pDeckLayout->setRowStretch(1, 1);

    pSplitter->addWidget(pDeck);
  }

  pSplitter->setStretchFactor(0, 3);
  pSplitter->setStretchFactor(1, 2);
  pCentralLayout->addWidget(pSplitter, 1);

  plQtEngineUiContentWidget* pStatusWidget = nullptr;
  m_pStatusView = plQtEngineUiContentWidget::Create<plEditorUiAudioStatusRow>(pCentralWidget, "AudioEventStatus", &pStatusWidget);
  m_pStatusView->SetRight("backend: MiniAudio");
  pStatusWidget->SetFixedLogicalHeight(plEditorUiAudioStatusRow::GetHeight());
  pCentralLayout->addWidget(pStatusWidget);
}

plAudioEventAssetDocumentWindow::~plAudioEventAssetDocumentWindow()
{
  OnStopEvent();
  m_pAssetDoc->m_AuditionEvents.RemoveEventHandler(m_AuditionSubscription);

  GetDocument()->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plAudioEventAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plAudioEventAssetDocumentWindow::StructureEventHandler, this));
  GetDocument()->GetSelectionManager()->m_Events.RemoveEventHandler(plMakeDelegate(&plAudioEventAssetDocumentWindow::SelectionEventHandler, this));
}

const plDocumentObject* plAudioEventAssetDocumentWindow::GetPropertiesObject() const
{
  const auto& children = GetDocument()->GetObjectManager()->GetRootObject()->GetChildren();
  return children.IsEmpty() ? nullptr : children[0];
}

const plDocumentObject* plAudioEventAssetDocumentWindow::GetSelectedObjectOfProperty(const char* szParentProperty) const
{
  const plDocumentObject* pSelected = GetDocument()->GetSelectionManager()->GetCurrentObject();

  while (pSelected != nullptr && pSelected != GetDocument()->GetObjectManager()->GetRootObject())
  {
    if (pSelected->GetParentProperty().IsEqual(szParentProperty))
      return pSelected;

    pSelected = pSelected->GetParent();
  }

  return nullptr;
}

const plDocumentObject* plAudioEventAssetDocumentWindow::GetActiveInstrument() const
{
  if (const plDocumentObject* pSelected = GetSelectedObjectOfProperty("Instruments"))
    return pSelected;

  if (m_ActiveInstrumentGuid.IsValid())
  {
    if (const plDocumentObject* pObject = GetDocument()->GetObjectManager()->GetObject(m_ActiveInstrumentGuid))
    {
      if (pObject->GetParentProperty().IsEqual("Instruments"))
        return pObject;
    }
  }

  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherChildrenOfProperty(GetPropertiesObject(), "Instruments", instruments);
  return instruments.IsEmpty() ? nullptr : instruments[0];
}

const plDocumentObject* plAudioEventAssetDocumentWindow::GetActiveAutomation() const
{
  if (const plDocumentObject* pSelected = GetSelectedObjectOfProperty("Automations"))
    return pSelected;

  plHybridArray<const plDocumentObject*, 8> automations;
  GatherChildrenOfProperty(GetPropertiesObject(), "Automations", automations);
  return automations.IsEmpty() ? nullptr : automations[0];
}

void plAudioEventAssetDocumentWindow::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  // a changed sound file must be re-measured for the timeline clip length (clip durations come from the curator, uncached)
  if (e.m_sProperty == "File" && m_pTimelineView != nullptr)
    m_pTimelineView->ClearDurationCache();

  QueueRefresh();
}

bool plAudioEventAssetDocumentWindow::HasSoloTracks() const
{
  return m_pTimelineView != nullptr && m_pTimelineView->HasSolo();
}

bool plAudioEventAssetDocumentWindow::IsSoloTrack(const plUuid& instrumentGuid) const
{
  return m_pTimelineView != nullptr && m_pTimelineView->IsSolo(instrumentGuid);
}

void plAudioEventAssetDocumentWindow::PickEntryClip(const plUuid& entryGuid)
{
  const plDocumentObject* pEntry = GetDocument()->GetObjectManager()->GetObject(entryGuid);
  if (pEntry == nullptr)
    return;

  plString sPicked;
  if (!PickClip(pEntry->GetTypeAccessor().GetValue("Clip").ConvertTo<plString>(), sPicked))
    return;

  // looked up again: the asset browser runs its own event loop
  pEntry = GetDocument()->GetObjectManager()->GetObject(entryGuid);
  if (pEntry == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Set Playlist Clip");
  accessor.SetValueByName(pEntry, "Clip", sPicked).IgnoreResult();
  accessor.SetValueByName(pEntry, "File", plString()).IgnoreResult();
  accessor.FinishTransaction();
}

void plAudioEventAssetDocumentWindow::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  PL_IGNORE_UNUSED(e);
  QueueRefresh();
}

void plAudioEventAssetDocumentWindow::SelectionEventHandler(const plSelectionManagerEvent& e)
{
  PL_IGNORE_UNUSED(e);
  QueueRefresh();
}

void plAudioEventAssetDocumentWindow::QueueRefresh()
{
  if (m_bRefreshQueued)
    return;

  m_bRefreshQueued = true;

  QTimer::singleShot(0, this, [this]()
    {
      m_bRefreshQueued = false;

      // the names this event defines become dropdown choices on the audio components
      {
        plHybridArray<const plDocumentObject*, 8> parameters, instruments;
        GatherChildrenOfProperty(GetPropertiesObject(), "Parameters", parameters);
        GatherChildrenOfProperty(GetPropertiesObject(), "Instruments", instruments);

        for (const plDocumentObject* pParameter : parameters)
        {
          plAudioEnumRegistry::RegisterRtpcName(pParameter->GetTypeAccessor().GetValue("Name").ConvertTo<plString>());
        }

        for (const plDocumentObject* pInstrument : instruments)
        {
          plAudioEnumRegistry::RegisterSwitchStateName(pInstrument->GetTypeAccessor().GetValue("SwitchState").ConvertTo<plString>());
        }
      }

      {
        const plDocumentObject* pInstrument = GetActiveInstrument();
        const plDocumentObject* pAutomation = GetActiveAutomation();

        m_pTimelineView->Refresh();
        m_pDeckView->SetInstrument(pInstrument != nullptr ? pInstrument->GetGuid() : plUuid());
        m_pDeckWidget->SetFixedLogicalHeight(m_pDeckView->GetContentHeight());
        m_pAutomationView->SetAutomation(pAutomation != nullptr ? pAutomation->GetGuid() : plUuid());

        // the attenuation panel only matters for a spatialized event
        const bool bSpatialize = GetPropertiesObject()->GetTypeAccessor().GetValue("Spatialize").ConvertTo<bool>();
        m_pAttenuationWidget->setVisible(bSpatialize);
      }

      plHybridArray<const plDocumentObject*, 8> instruments, parameters, automations, markers, sustains;
      GatherChildrenOfProperty(GetPropertiesObject(), "Instruments", instruments);
      GatherChildrenOfProperty(GetPropertiesObject(), "Parameters", parameters);
      GatherChildrenOfProperty(GetPropertiesObject(), "Automations", automations);
      GatherChildrenOfProperty(GetPropertiesObject(), "Markers", markers);
      GatherChildrenOfProperty(GetPropertiesObject(), "SustainPoints", sustains);

      plUInt32 uiSamples = 0;
      for (const plDocumentObject* pInstrument : instruments)
      {
        plHybridArray<const plDocumentObject*, 8> playlist;
        GatherChildrenOfProperty(pInstrument, "Playlist", playlist);
        uiSamples += playlist.GetCount();
      }

      if (m_pStatusView != nullptr)
      {
        plStringBuilder sInstruments, sEntries, sParameters, sAutomations, sMarkers, sSustains;
        sInstruments.SetFormat("{}", instruments.GetCount());
        sEntries.SetFormat("{}", uiSamples);
        sParameters.SetFormat("{}", parameters.GetCount());
        sAutomations.SetFormat("{}", automations.GetCount());
        sMarkers.SetFormat("{}", markers.GetCount());
        sSustains.SetFormat("{}", sustains.GetCount());

        const plEditorUiAudioStatusRow::Part parts[] = {{sInstruments, "instruments"}, {sEntries, "playlist entries"}, {sParameters, "sheets"}, {sAutomations, "automations"}, {sMarkers, "markers"}, {sSustains, "sustain points"}};
        m_pStatusView->SetParts(parts);
      } });
}

bool plAudioEventAssetDocumentWindow::PickSoundFiles(plHybridArray<plString, 8>& out_files)
{
  out_files.Clear();

  const QStringList files = QFileDialog::getOpenFileNames(this, "Select Sound Files", QString(), "Sound Files (*.wav *.mp3)");

  for (const QString& sFile : files)
  {
    plStringBuilder sPath = sFile.toUtf8().data();
    sPath.MakeCleanPath();

    if (!plQtEditorApp::GetSingleton()->MakePathDataDirectoryRelative(sPath))
    {
      plLog::Warning("Sound file is outside all data directories and may not transform: '{0}'", sPath);
    }

    out_files.PushBack(sPath.GetView());
  }

  return !out_files.IsEmpty();
}

void plAudioEventAssetDocumentWindow::OnAddInstrument()
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Instrument");

  plUuid instrumentGuid;
  if (accessor.AddObjectByName(pProps, "Instruments", -1, plGetStaticRTTI<plAudioEventInstrument>(), instrumentGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  // start with one empty playlist entry so the instrument is immediately editable
  const plDocumentObject* pInstrument = GetDocument()->GetObjectManager()->GetObject(instrumentGuid);
  plUuid entryGuid;
  accessor.AddObjectByName(pInstrument, "Playlist", -1, plGetStaticRTTI<plAudioEventPlaylistEntry>(), entryGuid).IgnoreResult();

  accessor.FinishTransaction();

  m_ActiveInstrumentGuid = instrumentGuid;
  GetDocument()->GetSelectionManager()->SetSelection(pInstrument);
}

void plAudioEventAssetDocumentWindow::OnAddFilesToInstrument()
{
  const plDocumentObject* pInstrument = GetActiveInstrument();
  if (pInstrument == nullptr)
  {
    ShowTemporaryStatusBarMsg("Add an instrument first.");
    return;
  }

  plHybridArray<plString, 8> files;
  if (!PickSoundFiles(files))
    return;

  // the clip of each file, found or created before the playlist is edited
  const plUuid instrumentGuid = pInstrument->GetGuid();
  plHybridArray<plString, 8> clips;

  for (const plString& sFile : files)
  {
    plString sClipGuid;
    const plStatus res = plAudioClipAssetUtils::FindOrCreateClip(sFile, sClipGuid);
    if (res.Failed())
    {
      plLog::Error("Could not add '{0}': {1}", sFile, res.m_sMessage);
      continue;
    }

    clips.PushBack(sClipGuid);
  }

  // looked up again: creating and saving the clip documents can show dialogs, during which the playlist may change
  pInstrument = GetDocument()->GetObjectManager()->GetObject(instrumentGuid);
  if (pInstrument == nullptr || clips.IsEmpty())
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Playlist Files");

  for (const plString& sClipGuid : clips)
  {
    plUuid entryGuid;
    if (accessor.AddObjectByName(pInstrument, "Playlist", -1, plGetStaticRTTI<plAudioEventPlaylistEntry>(), entryGuid).Succeeded())
    {
      accessor.SetValueByName(GetDocument()->GetObjectManager()->GetObject(entryGuid), "Clip", sClipGuid).IgnoreResult();
    }
  }

  // unnamed instrument: derive a name from the first file, stripping the extension and
  // trailing digits/separators ("rifle_shot_01.wav" -> "rifle_shot")
  const plString sCurrentName = pInstrument->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
  if (sCurrentName.IsEmpty())
  {
    plStringBuilder sName = plPathUtils::GetFileName(files[0].GetView());
    while (!sName.IsEmpty())
    {
      const char cLast = sName.GetData()[sName.GetElementCount() - 1];
      if ((cLast >= '0' && cLast <= '9') || cLast == '_' || cLast == '-' || cLast == ' ')
      {
        sName.Shrink(0, 1);
      }
      else
      {
        break;
      }
    }

    if (!sName.IsEmpty())
    {
      accessor.SetValueByName(pInstrument, "Name", sName.GetView()).IgnoreResult();
    }
  }

  accessor.FinishTransaction();
}

bool plAudioEventAssetDocumentWindow::PickClip(plStringView sCurrentClip, plString& out_sClipGuid)
{
  plUuid current;
  if (!sCurrentClip.IsEmpty() && plConversionUtils::IsStringUuid(sCurrentClip))
    current = plConversionUtils::ConvertStringToUuid(sCurrentClip);

  plEngineAssetPicker::Result picked;
  if (!plEngineAssetPicker::PickAsset(this, current, "CompatibleAsset_Audio_Clip", picked, "Select Audio Clip") || !picked.m_Guid.IsValid())
    return false;

  plStringBuilder sGuid;
  out_sClipGuid = plConversionUtils::ToString(picked.m_Guid, sGuid);
  return true;
}

void plAudioEventAssetDocumentWindow::OnAddClipToInstrument()
{
  const plDocumentObject* pInstrument = GetActiveInstrument();
  if (pInstrument == nullptr)
  {
    ShowTemporaryStatusBarMsg("Add an instrument first.");
    return;
  }

  const plUuid instrumentGuid = pInstrument->GetGuid();

  plString sClipGuid;
  if (!PickClip(plStringView(), sClipGuid))
    return;

  pInstrument = GetDocument()->GetObjectManager()->GetObject(instrumentGuid);
  if (pInstrument == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Playlist Clip");

  plUuid entryGuid;
  if (accessor.AddObjectByName(pInstrument, "Playlist", -1, plGetStaticRTTI<plAudioEventPlaylistEntry>(), entryGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  accessor.SetValueByName(GetDocument()->GetObjectManager()->GetObject(entryGuid), "Clip", sClipGuid).IgnoreResult();
  accessor.FinishTransaction();
}

void plAudioEventAssetDocumentWindow::OnAddEventToInstrument()
{
  const plDocumentObject* pInstrument = GetActiveInstrument();
  if (pInstrument == nullptr)
  {
    ShowTemporaryStatusBarMsg("Add an instrument first.");
    return;
  }

  const plUuid instrumentGuid = pInstrument->GetGuid();

  plEngineAssetPicker::Result picked;
  if (!plEngineAssetPicker::PickAsset(this, plUuid(), "CompatibleAsset_Audio_Event", picked, "Select Audio Event") || !picked.m_Guid.IsValid())
    return;

  if (picked.m_Guid == GetDocument()->GetGuid())
  {
    ShowTemporaryStatusBarMsg("An event cannot play itself - pick another event.");
    return;
  }

  pInstrument = GetDocument()->GetObjectManager()->GetObject(instrumentGuid);
  if (pInstrument == nullptr)
    return;

  plStringBuilder sEventGuid;
  plConversionUtils::ToString(picked.m_Guid, sEventGuid);

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Nested Event");

  plUuid entryGuid;
  if (accessor.AddObjectByName(pInstrument, "Playlist", -1, plGetStaticRTTI<plAudioEventPlaylistEntry>(), entryGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  accessor.SetValueByName(GetDocument()->GetObjectManager()->GetObject(entryGuid), "Event", sEventGuid.GetView()).IgnoreResult();
  accessor.FinishTransaction();
}

void plAudioEventAssetDocumentWindow::OnAddPlaylistEntry()
{
  const plDocumentObject* pInstrument = GetActiveInstrument();
  if (pInstrument == nullptr)
  {
    ShowTemporaryStatusBarMsg("Add an instrument first.");
    return;
  }

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Playlist Entry");

  plUuid entryGuid;
  if (accessor.AddObjectByName(pInstrument, "Playlist", -1, plGetStaticRTTI<plAudioEventPlaylistEntry>(), entryGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  accessor.FinishTransaction();
}

void plAudioEventAssetDocumentWindow::OnAddParameter()
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Parameter");

  plUuid parameterGuid;
  if (accessor.AddObjectByName(pProps, "Parameters", -1, plGetStaticRTTI<plAudioEventParameter>(), parameterGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  accessor.FinishTransaction();

  GetDocument()->GetSelectionManager()->SetSelection(GetDocument()->GetObjectManager()->GetObject(parameterGuid));
}

void plAudioEventAssetDocumentWindow::OnAddAutomationFor(plUInt8 uiTarget, const plString& sParameter)
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Automation");

  plUuid automationGuid;
  if (accessor.AddObjectByName(pProps, "Automations", -1, plGetStaticRTTI<plAudioEventAutomation>(), automationGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  const plDocumentObject* pAutomation = GetDocument()->GetObjectManager()->GetObject(automationGuid);
  accessor.SetValueByName(pAutomation, "Parameter", sParameter.GetView()).IgnoreResult();
  accessor.SetValueByName(pAutomation, "Target", static_cast<plInt64>(uiTarget)).IgnoreResult();

  // the curve starts at what changes nothing: a multiplier of 1, an open filter, or no send (rising to a full send)
  float fStartY = 1.0f;
  float fEndY = 1.0f;

  if (uiTarget == plAudioEventAutomationTarget::InstrumentVolume)
  {
    // the active instrument, which the deck shows
    if (const plDocumentObject* pInstrument = GetActiveInstrument())
      accessor.SetValueByName(pAutomation, "Instrument", pInstrument->GetTypeAccessor().GetValue("Name").ConvertTo<plString>().GetView()).IgnoreResult();
  }
  else if (uiTarget == plAudioEventAutomationTarget::FilterCutoff)
  {
    fStartY = 22000.0f;
    fEndY = 22000.0f;
  }
  else if (uiTarget == plAudioEventAutomationTarget::SendLevel)
  {
    fStartY = 0.0f;

    // the first aux bus of the loaded mixers
    const auto& auxBuses = plDynamicStringEnum::GetDynamicEnum("AudioSystem.AuxBus").GetAllValidValues();
    if (!auxBuses.IsEmpty())
      accessor.SetValueByName(pAutomation, "Bus", auxBuses[0].GetView()).IgnoreResult();
  }

  plUuid pointGuid;
  if (accessor.AddObjectByName(pAutomation, "Points", -1, plGetStaticRTTI<plAudioEventCurvePoint>(), pointGuid).Succeeded())
  {
    accessor.SetValueByName(GetDocument()->GetObjectManager()->GetObject(pointGuid), "X", 0.0f).IgnoreResult();
    accessor.SetValueByName(GetDocument()->GetObjectManager()->GetObject(pointGuid), "Y", fStartY).IgnoreResult();
  }
  if (accessor.AddObjectByName(pAutomation, "Points", -1, plGetStaticRTTI<plAudioEventCurvePoint>(), pointGuid).Succeeded())
  {
    accessor.SetValueByName(GetDocument()->GetObjectManager()->GetObject(pointGuid), "X", 1.0f).IgnoreResult();
    accessor.SetValueByName(GetDocument()->GetObjectManager()->GetObject(pointGuid), "Y", fEndY).IgnoreResult();
  }

  accessor.FinishTransaction();

  GetDocument()->GetSelectionManager()->SetSelection(pAutomation);
}

void plAudioEventAssetDocumentWindow::OnPlayEvent()
{
  StartAudition(plUuid(), m_pTimelineView != nullptr ? m_pTimelineView->GetCursor() : 0.0);
}

void plAudioEventAssetDocumentWindow::OnTogglePlay()
{
  if (m_bAuditioning)
    OnStopEvent();
  else
    OnPlayEvent();
}

void plAudioEventAssetDocumentWindow::OnStopEvent()
{
  if (!m_bAuditioning)
    return;

  SendAuditionCommand(plAudioAuditionCommand::Stop);

  if (m_pMessagesView != nullptr)
    m_pMessagesView->Add(m_fAuditionPosition, "stop");
}

void plAudioEventAssetDocumentWindow::OnKeyOff()
{
  if (!m_bAuditioning)
    return;

  SendAuditionCommand(plAudioAuditionCommand::KeyOff);

  if (m_pMessagesView != nullptr)
    m_pMessagesView->Add(m_fAuditionPosition, "key off: sustain released", plEditorUiAudioMessageList::Kind::Ok);
}

void plAudioEventAssetDocumentWindow::OpenNestedEvent(const plString& sEventGuid)
{
  // opening a document from inside an input event of the engine-drawn timeline is not allowed: one frame later
  QTimer::singleShot(0, this, [this, sEventGuid]()
    {
      const auto pSubAsset = plAssetCurator::GetSingleton()->FindSubAsset(sEventGuid);
      if (!pSubAsset.isValid() || pSubAsset->m_pAssetInfo == nullptr)
      {
        ShowTemporaryStatusBarMsg("The nested event is not in the asset browser.");
        return;
      }

      plQtEditorApp::GetSingleton()->OpenDocument(pSubAsset->m_pAssetInfo->m_Path.GetAbsolutePath(), plDocumentFlags::RequestWindow | plDocumentFlags::AddToRecentFilesList);
    });
}

void plAudioEventAssetDocumentWindow::StartAudition(const plUuid& entryGuid, double fStartOffset)
{
  plAudioEventDesc desc;
  if (plStatus res = m_pAssetDoc->BuildEventDesc(desc); res.Failed())
  {
    ShowTemporaryStatusBarMsg(res.m_sMessage.GetView());
    return;
  }

  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherChildrenOfProperty(GetPropertiesObject(), "Instruments", instruments);

  if (entryGuid.IsValid())
  {
    // one playlist entry, right away: its instrument alone, with only that entry and without its range or switch
    const plDocumentObject* pEntry = GetDocument()->GetObjectManager()->GetObject(entryGuid);
    if (pEntry == nullptr || pEntry->GetParent() == nullptr)
      return;

    const plUInt32 uiInstrument = instruments.IndexOf(pEntry->GetParent());
    const plUInt32 uiEntry = pEntry->GetPropertyIndex().ConvertTo<plUInt32>();
    if (uiInstrument >= desc.m_Instruments.GetCount() || uiEntry >= desc.m_Instruments[uiInstrument].m_Playlist.GetCount())
      return;

    plAudioEventInstrumentDesc instrument = desc.m_Instruments[uiInstrument];
    const plAudioEventPlaylistEntryDesc entry = instrument.m_Playlist[uiEntry];
    instrument.m_Playlist.Clear();
    instrument.m_Playlist.PushBack(entry);
    instrument.m_StartDelay = plTime::MakeZero();
    instrument.m_bLoop = false;
    instrument.m_bMute = false;
    instrument.m_sSwitchState.Clear();
    instrument.m_uiRangeParameter = plAudioEventNoParameter;

    desc.m_Instruments.Clear();
    desc.m_Instruments.PushBack(instrument);
    desc.m_bLoopRegion = false;
    desc.m_FadeIn = plTime::MakeZero();
    fStartOffset = 0.0;
  }
  else
  {
    // solo is a session state of the window: the other instruments are muted for the audition
    const bool bAnySolo = HasSoloTracks();
    bool bAnyAudible = false;

    for (plUInt32 i = 0; i < desc.m_Instruments.GetCount() && i < instruments.GetCount(); ++i)
    {
      plAudioEventInstrumentDesc& instrument = desc.m_Instruments[i];
      if (bAnySolo && !IsSoloTrack(instruments[i]->GetGuid()))
        instrument.m_bMute = true;

      bAnyAudible |= !instrument.m_bMute && !instrument.m_Playlist.IsEmpty();
    }

    if (!bAnyAudible)
    {
      ShowTemporaryStatusBarMsg(bAnySolo ? "All channels are muted or silenced by solo." : "Nothing to play - the playlists are empty or every channel is muted.");
      return;
    }
  }

  // the parameters start at the values the parameter tabs audition
  if (m_pTimelineView != nullptr)
  {
    for (plAudioEventParameterDesc& parameter : desc.m_Parameters)
    {
      parameter.m_fDefault = plMath::Clamp(m_pTimelineView->GetAuditionValue(parameter.m_sName, parameter.m_fDefault), parameter.m_fMin, parameter.m_fMax);
    }
  }

  plContiguousMemoryStreamStorage storage;
  {
    plMemoryStreamWriter writer(&storage);
    if (plStatus res = m_pAssetDoc->WriteAuditionData(writer, desc); res.Failed())
    {
      ShowTemporaryStatusBarMsg(res.m_sMessage.GetView());
      return;
    }
  }

  plAudioAuditionMsgToEngine msg;
  msg.m_Command = plAudioAuditionCommand::Play;
  msg.m_EventData.PushBackRange(plArrayPtr<const plUInt8>(storage.GetData(), storage.GetStorageSize32()));
  msg.m_fStartOffset = fStartOffset;
  msg.m_bSeeded = true;
  msg.m_uiSeed = m_uiSeed;

  // a spatialized event plays at the Distance tab's distance
  if (desc.m_bSpatialize && m_pTimelineView != nullptr)
    msg.m_fDistance = m_pTimelineView->GetAuditionValue("Distance", 0.0f);

  if (plPlasmaAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plPlasmaAudioProjectPreferences>())
  {
    msg.m_sMixer = pPreferences->GetAuditionMixer();
  }

  m_bAuditioning = m_pAssetDoc->SendMessageToEngine(&msg);
  m_bMixerNoteShown = false;
  m_bHolding = false;
  m_fAuditionPosition = fStartOffset;

  if (!m_bAuditioning)
  {
    ShowTemporaryStatusBarMsg("The engine process is not running - the audition plays there.");
    return;
  }

  if (m_pMessagesView != nullptr)
  {
    plStringBuilder sStart;
    sStart.SetFormat("start {}", plPathUtils::GetFileName(GetDocument()->GetDocumentPath()));
    m_pMessagesView->Clear();
    m_pMessagesView->Add(fStartOffset, sStart);
  }
}

void plAudioEventAssetDocumentWindow::SetSeed(plUInt32 uiSeed)
{
  m_uiSeed = uiSeed;

  if (m_pToolStrip != nullptr)
  {
    plStringBuilder sSeed;
    sSeed.SetFormat("{}", uiSeed);
    m_pToolStrip->SetText(m_uiSeedField, sSeed);
  }
}

void plAudioEventAssetDocumentWindow::SendAuditionCommand(plAudioAuditionCommand::Enum command)
{
  plAudioAuditionMsgToEngine msg;
  msg.m_Command = command;

  if (!m_pAssetDoc->SendMessageToEngine(&msg))
  {
    m_bAuditioning = false;
    ShowAuditionLevel(0.0f);
  }
}

void plAudioEventAssetDocumentWindow::OnAuditionParameterChanged(const plString& sName, float fValue)
{
  if (!m_bAuditioning)
    return;

  plAudioAuditionMsgToEngine msg;
  msg.m_Command = plAudioAuditionCommand::SetParameters;
  msg.m_ParameterNames.PushBack(sName);
  msg.m_ParameterValues.PushBack(fValue);
  m_pAssetDoc->SendMessageToEngine(&msg);
}

void plAudioEventAssetDocumentWindow::OnAuditionStatus(const plAudioAuditionStatusMsgToEditor& status)
{
  if (!status.m_sError.IsEmpty())
  {
    ShowTemporaryStatusBarMsg(status.m_sError.GetView());
  }

  if (!status.m_bAvailable)
  {
    m_bAuditioning = false;
  }
  else if (status.m_bPlaying && !status.m_bWaiting && !status.m_bHasMixer && !m_bMixerNoteShown)
  {
    m_bMixerNoteShown = true;
    ShowTemporaryStatusBarMsg("No mixer is loaded: the event plays straight to the output. Set 'Audition Mixer' in the PlasmaAudio project preferences.");
  }

  const bool bPlaying = status.m_bAvailable && status.m_bPlaying;

  ShowAuditionLevel(bPlaying ? status.m_fPeak : 0.0f);

  if (bPlaying)
    m_fAuditionPosition = status.m_fPosition;

  // what the game object of the entity would get: the passed markers, and when the playhead starts waiting
  if (m_pMessagesView != nullptr)
  {
    for (const plString& sMarker : status.m_Markers)
    {
      plStringBuilder sText;
      sText.SetFormat("marker \"{}\" -> game object", sMarker);
      m_pMessagesView->Add(m_fAuditionPosition, sText);
    }

    const bool bHolding = bPlaying && status.m_bHolding;
    if (bHolding && !m_bHolding)
      m_pMessagesView->Add(m_fAuditionPosition, "sustain reached: waits for key off", plEditorUiAudioMessageList::Kind::Warning);

    m_bHolding = bHolding;
  }

  // the end comes after the markers the last status carried
  if (!bPlaying && !status.m_bWaiting)
  {
    if (m_bAuditioning && m_pMessagesView != nullptr)
      m_pMessagesView->Add(m_fAuditionPosition, "event finished");

    m_bAuditioning = false;
  }

  if (m_pToolStrip != nullptr)
    m_pToolStrip->SetEnabled(m_uiKeyOff, bPlaying && status.m_uiSustainsLeft > 0);

  if (m_pTimelineView != nullptr)
  {
    m_pTimelineView->SetPlayback(bPlaying && !status.m_bWaiting, status.m_fPosition, status.m_InstrumentSounding);
    m_pTimelineView->SetHolding(m_bHolding);
  }
}

void plAudioEventAssetDocumentWindow::ShowAuditionLevel(float fPeak)
{
  if (m_pMeterView != nullptr)
    m_pMeterView->SetPeak(fPeak);
}

void plAudioEventAssetDocumentWindow::OnRemoveSelected()
{
  const plDocumentObject* pSelected = GetDocument()->GetSelectionManager()->GetCurrentObject();

  if (pSelected == nullptr || pSelected == GetPropertiesObject() || pSelected->GetParent() == GetDocument()->GetObjectManager()->GetRootObject())
  {
    ShowTemporaryStatusBarMsg("Select an instrument, parameter, automation or playlist entry to remove.");
    return;
  }

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Remove Selected");

  if (accessor.RemoveObject(pSelected).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  accessor.FinishTransaction();

  GetDocument()->GetSelectionManager()->SetSelection(GetPropertiesObject());
}
