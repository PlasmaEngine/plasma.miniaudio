#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <Core/Configuration/PlatformProfile.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/Assets/AssetStatusIndicator.moc.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAssetWindow.moc.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipWaveformView.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/Preferences/MiniAudioPreferences.h>
#include <EditorUi/Menu.h>
#include <GuiFoundation/EngineUi/EngineUiControlStrip.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Threading/DelegateTask.h>
#include <GuiFoundation/ActionViews/MenuBarActionMapView.moc.h>
#include <GuiFoundation/ActionViews/ToolBarActionMapView.moc.h>
#include <GuiFoundation/DockPanels/DocumentPanel.moc.h>
#include <PlasmaAudioPlugin/Codecs/ImaAdpcm.h>
#include <PlasmaAudioPlugin/Codecs/OpusCodec.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

#include <QTimer>

namespace
{
  constexpr plUInt32 kPreviewCacheSize = 8;

  /// Audition ring buffer of a streamed clip: four disk chunks.
  constexpr plUInt64 kStreamRingBytes = 4 * plAudioClipConstants::StreamChunkBytes;

  QString FormatBytes(plUInt64 uiBytes)
  {
    if (uiBytes >= 1024 * 1024)
      return QString::asprintf("%.2f MB", uiBytes / (1024.0 * 1024.0));
    if (uiBytes >= 1024)
      return QString::asprintf("%.1f KB", uiBytes / 1024.0);
    return QString::asprintf("%u B", static_cast<plUInt32>(uiBytes));
  }

  const char* CodecName(plAudioClipCodec::Enum eCodec)
  {
    switch (eCodec)
    {
      case plAudioClipCodec::Pcm16:
        return "PCM";
      case plAudioClipCodec::ImaAdpcm:
        return "ADPCM";
      case plAudioClipCodec::Opus:
        return "Opus";
    }
    return "?";
  }

  const char* LoadModeName(plAudioClipLoadMode::Enum eMode)
  {
    switch (eMode)
    {
      case plAudioClipLoadMode::DecompressOnLoad:
        return "Decompress on load";
      case plAudioClipLoadMode::CompressedInMemory:
        return "Compressed in memory";
      case plAudioClipLoadMode::Streamed:
        return "Streamed";
    }
    return "?";
  }

  plUInt64 HashSettings(const plAudioClipBuildSettings& s, plStringView sPath, plInt64 iModificationTime)
  {
    plUInt64 uiHash = plHashingUtils::xxHash64String(sPath);
    auto Add = [&](const auto& value)
    { uiHash = plHashingUtils::xxHash64(&value, sizeof(value), uiHash); };

    Add(iModificationTime);
    Add(static_cast<plUInt8>(s.m_eCodec));
    Add(static_cast<plUInt8>(s.m_eLoadMode));
    Add(s.m_uiOpusBitrateKbps);
    Add(s.m_uiTargetSampleRate);
    Add(static_cast<plUInt8>(s.m_bForceMono ? 1 : 0));
    Add(s.m_fNormaliseLufs);
    Add(s.m_fPeakFallbackDb);
    Add(s.m_fTrimSilenceDb);
    Add(s.m_iLoopStart);
    Add(s.m_iLoopEnd);
    return uiHash;
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// plAudioClipPreviewJob
//////////////////////////////////////////////////////////////////////////

void plAudioClipPreviewJob::Run()
{
  plAudioClipPreview& res = *m_pResult;

  plAudioClipSourcePcm source;
  if (plAudioClipBuilder::DecodeSourceFile(m_sAbsSourceFile, source).Failed())
  {
    res.m_sError = "Could not decode the source file.";
    m_bDone = true;
    return;
  }

  // the builder trims leading silence; measure it on the source so loop markers can be mapped back to source time
  if (plMath::IsFinite(m_Settings.m_fTrimSilenceDb))
  {
    const float fThreshold = plMath::Pow(10.0f, m_Settings.m_fTrimSilenceDb / 20.0f);
    const plUInt32 uiChannels = source.m_uiChannels;
    const plUInt64 uiFrames = source.GetFrames();
    const bool bMono = m_Settings.m_bForceMono && uiChannels > 1;

    plUInt64 uiFirst = 0;
    for (; uiFirst < uiFrames; ++uiFirst)
    {
      const float* pFrame = source.m_Samples.GetData() + uiFirst * uiChannels;
      bool bLoud = false;

      if (bMono)
      {
        float fSum = 0.0f;
        for (plUInt32 c = 0; c < uiChannels; ++c)
          fSum += pFrame[c];
        bLoud = plMath::Abs(fSum / uiChannels) > fThreshold;
      }
      else
      {
        for (plUInt32 c = 0; c < uiChannels && !bLoud; ++c)
          bLoud = plMath::Abs(pFrame[c]) > fThreshold;
      }

      if (bLoud)
        break;
    }

    // never trimmed into the loop
    const plInt64 iLoopStart = m_Settings.m_iLoopStart >= 0 ? m_Settings.m_iLoopStart : (source.m_bHasLoop ? static_cast<plInt64>(source.m_uiLoopStart) : -1);
    if (iLoopStart >= 0)
      uiFirst = plMath::Min(uiFirst, static_cast<plUInt64>(iLoopStart));

    if (uiFirst < uiFrames)
      res.m_fTrimOffset = static_cast<double>(uiFirst) / source.m_uiSampleRate;
  }

  plDefaultMemoryStreamStorage storage;
  {
    plMemoryStreamWriter writer(&storage);
    if (plAudioClipBuilder::Build(source, m_Settings, writer).Failed())
    {
      res.m_sError = "Building the clip failed, see the log for details.";
      m_bDone = true;
      return;
    }
  }

  res.m_uiFileBytes = storage.GetStorageSize64();

  plMemoryStreamReader reader(&storage);
  if (res.m_Header.Read(reader).Failed() || res.m_Header.m_Languages.IsEmpty() || res.m_Header.m_uiChannels == 0)
  {
    res.m_sError = "The built clip could not be read back.";
    m_bDone = true;
    return;
  }

  const plAudioClipHeader& header = res.m_Header;
  const plAudioClipLanguageEntry& lang = header.m_Languages[0];
  reader.SkipBytes(static_cast<plUInt64>(lang.m_uiPrimingBytes) + lang.m_uiLoopPrimingBytes);

  plDynamicArray<plUInt8> payload;
  payload.SetCountUninitialized(static_cast<plUInt32>(lang.m_uiPayloadBytes));
  if (reader.ReadBytes(payload.GetData(), payload.GetCount()) != payload.GetCount())
  {
    res.m_sError = "The built clip is truncated.";
    m_bDone = true;
    return;
  }

  // decode the payload again, so auditioning plays what the runtime plays
  const plUInt8 uiChannels = header.m_uiChannels;
  const plUInt64 uiFrames = header.m_uiFrames;
  res.m_Pcm.SetCount(static_cast<plUInt32>(uiFrames * uiChannels));

  if (header.m_eCodec == plAudioClipCodec::Pcm16)
  {
    plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(res.m_Pcm.GetData()), payload.GetData(), plMath::Min<size_t>(payload.GetCount(), res.m_Pcm.GetCount() * sizeof(plInt16)));
  }
  else if (header.m_eCodec == plAudioClipCodec::ImaAdpcm)
  {
    const plUInt32 uiBlockBytes = plImaAdpcmCodec::GetBlockBytes(uiChannels);
    const plUInt64 uiBlocks = plImaAdpcmCodec::GetBlockCount(uiFrames);

    plDynamicArray<plInt16> block;
    block.SetCountUninitialized(plImaAdpcmCodec::FramesPerBlock * uiChannels);

    for (plUInt64 b = 0; b < uiBlocks && (b + 1) * uiBlockBytes <= payload.GetCount(); ++b)
    {
      plImaAdpcmCodec::DecodeBlock(payload.GetData() + b * uiBlockBytes, uiChannels, block.GetData());

      const plUInt64 uiFirst = b * plImaAdpcmCodec::FramesPerBlock;
      const plUInt64 uiCount = plMath::Min<plUInt64>(plImaAdpcmCodec::FramesPerBlock, uiFrames - uiFirst);
      plMemoryUtils::Copy(res.m_Pcm.GetData() + uiFirst * uiChannels, block.GetData(), static_cast<size_t>(uiCount * uiChannels));
    }
  }
  else if (header.m_eCodec == plAudioClipCodec::Opus)
  {
    if (plOpusCodec::Decode(payload, uiChannels, header.m_uiPreSkip, uiFrames, res.m_Pcm).Failed())
    {
      res.m_sError = "The Opus payload of the built clip could not be decoded.";
      m_bDone = true;
      return;
    }
  }

  res.m_bSuccess = true;
  m_bDone = true;
}

//////////////////////////////////////////////////////////////////////////
// plAudioClipAssetDocumentWindow
//////////////////////////////////////////////////////////////////////////

plAudioClipAssetDocumentWindow::plAudioClipAssetDocumentWindow(plDocument* pDocument)
  : plQtDocumentWindow(pDocument)
{
  m_pAssetDoc = static_cast<plAudioClipAssetDocument*>(pDocument);

  // Menu Bar
  {
    plQtMenuBarActionMapView* pMenuBar = static_cast<plQtMenuBarActionMapView*>(menuBar());
    plActionContext context;
    context.m_sMapping = "AudioClipAssetMenuBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pMenuBar->SetActionContext(context);
  }

  // Tool Bar
  {
    plQtToolBarActionMapView* pToolBar = new plQtToolBarActionMapView("Toolbar", this);
    plActionContext context;
    context.m_sMapping = "AudioClipAssetToolBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pToolBar->SetActionContext(context);
    pToolBar->setObjectName("AudioClipAssetWindowToolBar");
    addToolBar(pToolBar);
  }

  // Central: buttons, platform tabs, waveform, estimates
  {
    QWidget* pCentralWidget = new QWidget(this);

    QVBoxLayout* pCentralLayout = new QVBoxLayout(pCentralWidget);
    pCentralLayout->setContentsMargins(0, 0, 0, 0);
    pCentralLayout->setSpacing(0);

    CreateEngineCentralWidget(pCentralWidget, pCentralLayout);

    plQtDocumentPanel* pCentral = new plQtDocumentPanel(this, pDocument);
    pCentral->setObjectName("AudioClipWaveformDockWidget");
    pCentral->setWindowTitle("Audio Clip");
    pCentral->setWidget(pCentralWidget, ads::CDockWidget::ForceNoScrollArea);
    m_pDockManager->setCentralWidget(pCentral);
  }

  // Right dock: asset status, properties of the selected tab, users of the clip
  {
    plQtDocumentPanel* pPropertyPanel = new plQtDocumentPanel(this, pDocument);
    pPropertyPanel->setObjectName("AudioClipAssetDockWidget");
    pPropertyPanel->setWindowTitle("Clip Properties");
    pPropertyPanel->show();

    QWidget* pWidget = new QWidget();
    pWidget->setObjectName("Group");
    QVBoxLayout* pLayout = new QVBoxLayout(pWidget);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(0);

    pLayout->addWidget(new plQtAssetStatusIndicator((plAssetDocument*)GetDocument()));

    m_pPlatformStrip = new plQtEngineControlStrip(pWidget);
    m_pPlatformStrip->setObjectName("AudioClipPlatform");
    m_uiPlatform = m_pPlatformStrip->AddCodeLabel("PLATFORM \xC2\xB7 Default", 0.0f);
    m_pPlatformStrip->SetColor(m_uiPlatform, plEditorUi::Muted());
    pLayout->addWidget(m_pPlatformStrip);

    m_pPropertyGrid = new plQtPropertyGridWidget(pWidget, pDocument, false);
    pLayout->addWidget(m_pPropertyGrid, 1);

    plQtEngineUiContentWidget* pUsedByWidget = nullptr;
    m_pUsedByView = plQtEngineUiContentWidget::Create<plEditorUiAudioClipUsedBy>(pWidget, "AudioClipUsedBy", &pUsedByWidget);
    pUsedByWidget->SetFixedLogicalHeight(plEditorUiAudioClipUsedBy::Height);
    pLayout->addWidget(pUsedByWidget);

    pPropertyPanel->setWidget(pWidget, ads::CDockWidget::ForceNoScrollArea);
    m_pDockManager->addDockWidgetTab(ads::RightDockWidgetArea, pPropertyPanel);
  }

  pDocument->GetSelectionManager()->SetSelection(GetPropertiesObject());

  GetDocument()->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plAudioClipAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plAudioClipAssetDocumentWindow::StructureEventHandler, this));
  plAssetCurator::GetSingleton()->m_Events.AddEventHandler(plMakeDelegate(&plAudioClipAssetDocumentWindow::AssetCuratorEventHandler, this));
  m_AuditionSubscription = m_pAssetDoc->m_AuditionEvents.AddEventHandler(plMakeDelegate(&plAudioClipAssetDocumentWindow::OnAuditionStatus, this));

  m_pTimer = new QTimer(this);
  m_pTimer->setInterval(40);
  connect(m_pTimer, &QTimer::timeout, this, &plAudioClipAssetDocumentWindow::OnTimer);
  m_pTimer->start();

  RebuildTabs();
  RequestPreview(true);

  FinishWindowCreation();
}

void plAudioClipAssetDocumentWindow::CreateEngineCentralWidget(QWidget* pCentralWidget, QVBoxLayout* pCentralLayout)
{
  using plEditorUi::Icon;

  // toolbar row: transport, info line, loop label
  {
    m_pToolStrip = new plQtEngineControlStrip(pCentralWidget);
    m_pToolStrip->setObjectName("AudioClipToolStrip");

    m_uiPlay = m_pToolStrip->AddIconButton(Icon::Play, "Plays the clip as built for the selected platform tab (codec, rate and normalisation applied)");
    m_uiStop = m_pToolStrip->AddIconButton(Icon::Stop, "Stops playback");
    m_uiLoop = m_pToolStrip->AddIconButton(Icon::Repeat, "Repeats the loop region while playing. Drag across the waveform to set the loop, drag the markers to adjust it.", true);
    m_pToolStrip->SetChecked(m_uiLoop, true);
    m_pToolStrip->AddSeparator();
    m_uiInfo = m_pToolStrip->AddCodeLabel("", 0.0f);
    m_pToolStrip->SetColor(m_uiInfo, plEditorUi::Muted());
    m_uiLoopLabel = m_pToolStrip->AddCodeLabel("no loop", plMath::Ceil(plEditorUi::MeasureCodeText("loop 000.00 s - 000.00 s", plEditorUi::FontSize - 1)) + 4.0f);
    m_pToolStrip->SetColor(m_uiLoopLabel, plEditorUi::Muted());

    m_pToolStrip->m_OnChanged = [this](plUInt32 uiControl, plInt32 iValue)
    {
      PL_IGNORE_UNUSED(iValue);

      if (uiControl == m_uiPlay)
        OnPlay();
      else if (uiControl == m_uiStop)
        OnStop();
    };

    pCentralLayout->addWidget(m_pToolStrip);
  }

  // platform tabs, ruler, waveform and the stat row
  {
    plQtEngineUiContentWidget* pWaveWidget = nullptr;
    m_pWaveView = plQtEngineUiContentWidget::Create<plEditorUiAudioClipWaveform>(pCentralWidget, "AudioClipWaveform", &pWaveWidget);

    m_pWaveView->m_OnTabPicked = [this](plUInt32 uiTab)
    { OnTabChanged(static_cast<int>(uiTab)); };

    m_pWaveView->m_OnAddTab = [this](const plVec2& vPosition)
    { ShowAddOverrideMenu(vPosition); };

    m_pWaveView->m_OnTabMenu = [this](plUInt32 uiTab, const plVec2& vPosition)
    { ShowTabMenu(uiTab, vPosition); };

    m_pWaveView->m_OnLoopChanged = [this](plUInt64 uiStart, plUInt64 uiEnd)
    { SetLoopPoints(uiStart, uiEnd); };

    pCentralLayout->addWidget(pWaveWidget, 1);
  }
}

plAudioClipAssetDocumentWindow::~plAudioClipAssetDocumentWindow()
{
  m_pAssetDoc->m_AuditionEvents.RemoveEventHandler(m_AuditionSubscription);

  if (m_pRunningJob != nullptr)
  {
    plTaskSystem::WaitForGroup(m_RunningTask);
    m_pRunningJob.Clear();
  }

  plAssetCurator::GetSingleton()->m_Events.RemoveEventHandler(plMakeDelegate(&plAudioClipAssetDocumentWindow::AssetCuratorEventHandler, this));
  GetDocument()->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plAudioClipAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plAudioClipAssetDocumentWindow::StructureEventHandler, this));
}

const plDocumentObject* plAudioClipAssetDocumentWindow::GetPropertiesObject() const
{
  const auto& children = GetDocument()->GetObjectManager()->GetRootObject()->GetChildren();
  return children.IsEmpty() ? nullptr : children[0];
}

const plDocumentObject* plAudioClipAssetDocumentWindow::GetActiveOverrideObject() const
{
  if (!m_ActiveOverrideGuid.IsValid())
    return nullptr;

  const plDocumentObject* pObject = GetDocument()->GetObjectManager()->GetObject(m_ActiveOverrideGuid);
  if (pObject == nullptr || !pObject->GetParentProperty().IsEqual("PlatformOverrides"))
    return nullptr;

  return pObject;
}

void plAudioClipAssetDocumentWindow::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  if (e.m_sProperty == "Profile")
    m_bTabsDirty = true;

  RequestPreview(false);
}

void plAudioClipAssetDocumentWindow::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  PL_IGNORE_UNUSED(e);
  m_bTabsDirty = true;
  RequestPreview(false);
}

void plAudioClipAssetDocumentWindow::AssetCuratorEventHandler(const plAssetCuratorEvent& e)
{
  switch (e.m_Type)
  {
    case plAssetCuratorEvent::Type::AssetAdded:
    case plAssetCuratorEvent::Type::AssetRemoved:
    case plAssetCuratorEvent::Type::AssetMoved:
    case plAssetCuratorEvent::Type::AssetUpdated:
    case plAssetCuratorEvent::Type::AssetListReset:
      m_bUsedByDirty = true;
      break;

    default:
      break;
  }
}

void plAudioClipAssetDocumentWindow::RebuildTabs()
{
  m_bTabsDirty = false;
  m_bUpdatingTabs = true;

  m_TabOverrides.Clear();

  plHybridArray<plString, 8> labels;
  labels.PushBack("Default");
  m_TabOverrides.PushBack(plUuid());

  plUInt32 uiSelect = 0;

  if (const plDocumentObject* pProps = GetPropertiesObject())
  {
    for (const plDocumentObject* pChild : pProps->GetChildren())
    {
      if (!pChild->GetParentProperty().IsEqual("PlatformOverrides"))
        continue;

      const plString sProfile = pChild->GetTypeAccessor().GetValue("Profile").ConvertTo<plString>();
      labels.PushBack(sProfile.IsEmpty() ? plString("(no profile)") : sProfile);
      m_TabOverrides.PushBack(pChild->GetGuid());

      if (pChild->GetGuid() == m_ActiveOverrideGuid)
        uiSelect = labels.GetCount() - 1;
    }
  }

  if (uiSelect == 0)
    m_ActiveOverrideGuid = plUuid();

  m_pWaveView->SetTabs(labels, uiSelect);

  m_bUpdatingTabs = false;

  UpdatePropertyGridSelection();
}

void plAudioClipAssetDocumentWindow::OnTabChanged(int iIndex)
{
  if (m_bUpdatingTabs || iIndex < 0 || iIndex >= static_cast<int>(m_TabOverrides.GetCount()))
    return;

  m_ActiveOverrideGuid = m_TabOverrides[iIndex];
  UpdatePropertyGridSelection();
  RequestPreview(true);
}

void plAudioClipAssetDocumentWindow::UpdatePropertyGridSelection()
{
  const plDocumentObject* pOverride = GetActiveOverrideObject();
  const plDocumentObject* pObject = pOverride != nullptr ? pOverride : GetPropertiesObject();

  plDeque<const plDocumentObject*> selection;
  if (pObject != nullptr)
    selection.PushBack(pObject);
  m_pPropertyGrid->SetSelection(selection);

  if (pOverride == nullptr)
  {
    SetPlatformText(QString::fromUtf8("PLATFORM \xC2\xB7 Default"));
    return;
  }

  const plString sProfile = pOverride->GetTypeAccessor().GetValue("Profile").ConvertTo<plString>();

  bool bKnown = false;
  for (plUInt32 i = 0; i < plAssetCurator::GetSingleton()->GetNumAssetProfiles(); ++i)
  {
    if (plAssetCurator::GetSingleton()->GetAssetProfile(i)->GetConfigName().IsEqual_NoCase(sProfile))
      bKnown = true;
  }

  QString sText = QString::fromUtf8("PLATFORM \xC2\xB7 ") + QString::fromUtf8(sProfile.GetData());
  if (!bKnown)
    sText += " (no such asset profile)";
  SetPlatformText(sText);
}

void plAudioClipAssetDocumentWindow::GetOverrideProfiles(plDynamicArray<plString>& out_profiles, plDynamicArray<bool>& out_exists) const
{
  const plAudioClipAssetProperties* pProps = m_pAssetDoc->GetProperties();

  for (plUInt32 i = 0; i < plAssetCurator::GetSingleton()->GetNumAssetProfiles(); ++i)
  {
    const plString sProfile = plAssetCurator::GetSingleton()->GetAssetProfile(i)->GetConfigName();

    bool bExists = false;
    for (const plAudioClipPlatformOverride& o : pProps->m_PlatformOverrides)
    {
      if (o.m_sProfile.IsEqual_NoCase(sProfile))
        bExists = true;
    }

    out_profiles.PushBack(sProfile);
    out_exists.PushBack(bExists);
  }
}

void plAudioClipAssetDocumentWindow::AddPlatformOverride(const plString& sProfile)
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Platform Override");

  plUuid guid;
  if (accessor.AddObjectByName(pProps, "PlatformOverrides", -1, plGetStaticRTTI<plAudioClipPlatformOverride>(), guid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  const plDocumentObject* pOverride = GetDocument()->GetObjectManager()->GetObject(guid);
  accessor.SetValueByName(pOverride, "Profile", sProfile).IgnoreResult();

  // start from the default settings
  for (const char* szProperty : {"LoadMode", "Codec", "OpusKbps", "SampleRate"})
  {
    accessor.SetValueByName(pOverride, szProperty, pProps->GetTypeAccessor().GetValue(szProperty)).IgnoreResult();
  }

  accessor.FinishTransaction();

  m_ActiveOverrideGuid = guid;
  RebuildTabs();
  RequestPreview(true);
}

void plAudioClipAssetDocumentWindow::RemovePlatformOverride(const plUuid& overrideGuid)
{
  const plDocumentObject* pOverride = GetDocument()->GetObjectManager()->GetObject(overrideGuid);
  if (pOverride == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Remove Platform Override");
  accessor.RemoveObject(pOverride).IgnoreResult();
  accessor.FinishTransaction();

  RebuildTabs();
  RequestPreview(true);
}

void plAudioClipAssetDocumentWindow::ShowAddOverrideMenu(const plVec2& vPosition)
{
  plHybridArray<plString, 8> profiles;
  plHybridArray<bool, 8> exists;
  GetOverrideProfiles(profiles, exists);

  plEditorUiMenu menu;
  for (plUInt32 i = 0; i < profiles.GetCount(); ++i)
  {
    menu.AddAction(profiles[i], i).m_bEnabled = !exists[i];
  }

  if (profiles.IsEmpty())
  {
    menu.AddAction("No asset profiles", 0).m_bEnabled = false;
  }

  plEditorUiMenuView::Show(*m_pWaveView->GetSurface(), vPosition, menu, [this, profiles](plUInt32 uiId)
    {
      if (uiId < profiles.GetCount())
        AddPlatformOverride(profiles[uiId]);
    });
}

void plAudioClipAssetDocumentWindow::ShowTabMenu(plUInt32 uiTab, const plVec2& vPosition)
{
  if (uiTab == 0 || uiTab >= m_TabOverrides.GetCount())
    return;

  const plUuid guid = m_TabOverrides[uiTab];

  plEditorUiMenu menu;
  plEditorUiMenuItem& remove = menu.AddAction("Remove Override", 1);
  remove.m_sIcon = plEditorUi::IconPath(plEditorUi::Icon::Remove);
  remove.m_bDanger = true;

  plEditorUiMenuView::Show(*m_pWaveView->GetSurface(), vPosition, menu, [this, guid](plUInt32 uiId)
    {
      if (uiId == 1)
        RemovePlatformOverride(guid);
    });
}

void plAudioClipAssetDocumentWindow::SetLoopPoints(plUInt64 uiStartFrame, plUInt64 uiEndFrame)
{
  const plSharedPtr<plAudioClipPreview>& pPreview = m_pShownPreview;
  if (pPreview == nullptr || !pPreview->m_bSuccess || pPreview->m_Header.m_uiSampleRate == 0 || uiEndFrame <= uiStartFrame)
    return;

  // loop points are stored in source seconds
  const plAudioClipHeader& header = pPreview->m_Header;
  const double fStart = pPreview->m_fTrimOffset + static_cast<double>(uiStartFrame) / header.m_uiSampleRate;
  const double fEnd = pPreview->m_fTrimOffset + static_cast<double>(uiEndFrame) / header.m_uiSampleRate;

  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Set Loop Points");
  accessor.SetValueByName(pProps, "OverrideLoop", true).IgnoreResult();
  accessor.SetValueByName(pProps, "LoopStart", static_cast<float>(fStart)).IgnoreResult();
  accessor.SetValueByName(pProps, "LoopEnd", static_cast<float>(fEnd)).IgnoreResult();
  accessor.FinishTransaction();
}

void plAudioClipAssetDocumentWindow::SetInfoText(const QString& sText)
{
  m_pToolStrip->SetText(m_uiInfo, sText.toUtf8().data());
}

void plAudioClipAssetDocumentWindow::SetLoopText(const QString& sText)
{
  m_pToolStrip->SetText(m_uiLoopLabel, sText.toUtf8().data());
}

void plAudioClipAssetDocumentWindow::SetPlatformText(const QString& sText)
{
  m_pPlatformStrip->SetText(m_uiPlatform, sText.toUtf8().data());
}

bool plAudioClipAssetDocumentWindow::IsLoopEnabled() const
{
  return m_pToolStrip->IsChecked(m_uiLoop);
}

void plAudioClipAssetDocumentWindow::RequestPreview(bool bImmediate)
{
  m_bPreviewRequested = true;
  m_PreviewDue = plTime::Now() + (bImmediate ? plTime::MakeZero() : plTime::MakeFromMilliseconds(300));
}

void plAudioClipAssetDocumentWindow::OnTimer()
{
  if (m_bTabsDirty)
    RebuildTabs();

  if (m_bUsedByDirty)
    RefreshUsedBy();

  if (m_pRunningJob != nullptr && m_pRunningJob->m_bDone)
  {
    plSharedPtr<plAudioClipPreview> pResult = m_pRunningJob->m_pResult;
    m_pRunningJob.Clear();

    if (m_PreviewCache.GetCount() >= kPreviewCacheSize)
      m_PreviewCache.RemoveAtAndCopy(0);
    m_PreviewCache.PushBack(pResult);

    if (pResult->m_uiHash == m_uiWantedHash)
      ShowPreview(pResult);
  }

  StartPreviewIfNeeded();

  // the audition answers every request with its position
  if (m_bAuditioning)
  {
    plAudioAuditionMsgToEngine msg;
    msg.m_Command = plAudioAuditionCommand::Poll;
    m_bAuditioning = m_pAssetDoc->SendMessageToEngine(&msg);
  }

  m_pWaveView->SetPlayCursor(GetAuditionCursor());
}

void plAudioClipAssetDocumentWindow::ShowWaveformMessage(const QString& sMessage)
{
  m_pWaveView->SetMessage(sMessage.toUtf8().data());
}

void plAudioClipAssetDocumentWindow::StartPreviewIfNeeded()
{
  if (!m_bPreviewRequested || plTime::Now() < m_PreviewDue || m_pRunningJob != nullptr)
    return;

  m_bPreviewRequested = false;

  const plAudioClipAssetProperties* pProps = m_pAssetDoc->GetProperties();

  auto ShowMessage = [this](const QString& sMessage)
  {
    m_uiWantedHash = 0;
    m_pShownPreview.Clear();
    m_pWaveView->SetPreview(nullptr);

    ShowWaveformMessage(sMessage);
    ShowPreview(nullptr);
  };

  if (pProps->m_sSourceFile.IsEmpty())
  {
    ShowMessage("Select a source file (WAV, MP3 or FLAC) in the properties.");
    return;
  }

  plStringBuilder sAbsPath = pProps->m_sSourceFile;
  plFileStats stats;
  if (!plQtEditorApp::GetSingleton()->MakeDataDirectoryRelativePathAbsolute(sAbsPath) || plOSFile::GetFileStats(sAbsPath, stats).Failed())
  {
    ShowMessage(QString("Source file not found: ") + QString::fromUtf8(pProps->m_sSourceFile.GetData()));
    return;
  }

  plAudioClipAssetMetaInfo info;
  if (plAudioClipAssetUtils::ReadSourceInfo(sAbsPath, info).Failed())
  {
    ShowMessage("The source file could not be read.");
    return;
  }

  // the override and the limits of the selected tab's profile
  const plAudioClipPlatformOverride* pOverride = nullptr;
  const plPlatformProfile* pProfile = nullptr;

  if (const plDocumentObject* pOverrideObject = GetActiveOverrideObject())
  {
    const plInt32 iIndex = pOverrideObject->GetPropertyIndex().ConvertTo<plInt32>();
    if (iIndex >= 0 && iIndex < static_cast<plInt32>(pProps->m_PlatformOverrides.GetCount()))
    {
      pOverride = &pProps->m_PlatformOverrides[iIndex];

      for (plUInt32 i = 0; i < plAssetCurator::GetSingleton()->GetNumAssetProfiles(); ++i)
      {
        if (plAssetCurator::GetSingleton()->GetAssetProfile(i)->GetConfigName().IsEqual_NoCase(pOverride->m_sProfile))
          pProfile = plAssetCurator::GetSingleton()->GetAssetProfile(i);
      }
    }
  }

  const plAudioClipBuildSettings settings = plAudioClipAssetUtils::MakeBuildSettings(*pProps, pOverride, pProfile, info.m_uiSampleRate);
  const plUInt64 uiHash = HashSettings(settings, sAbsPath, stats.m_LastModificationTime.GetInt64(plSIUnitOfTime::Microsecond));

  m_uiWantedHash = uiHash;

  for (const auto& pCached : m_PreviewCache)
  {
    if (pCached->m_uiHash == uiHash)
    {
      ShowPreview(pCached);
      return;
    }
  }

  plSharedPtr<plAudioClipPreview> pResult = PL_DEFAULT_NEW(plAudioClipPreview);
  pResult->m_uiHash = uiHash;
  pResult->m_Settings = settings;

  if (m_pShownPreview == nullptr)
    ShowWaveformMessage("Building preview...");

  m_pRunningJob = PL_DEFAULT_NEW(plAudioClipPreviewJob);
  m_pRunningJob->m_sAbsSourceFile = sAbsPath;
  m_pRunningJob->m_Settings = settings;
  m_pRunningJob->m_pResult = pResult;

  plSharedPtr<plAudioClipPreviewJob> pJob = m_pRunningJob;
  plSharedPtr<plDelegateTask<void>> pTask = PL_DEFAULT_NEW(plDelegateTask<void>, "AudioClipPreview", plTaskNesting::Never, [pJob]()
    { pJob->Run(); });
  m_RunningTask = plTaskSystem::StartSingleTask(pTask, plTaskPriority::LongRunning);
}

void plAudioClipAssetDocumentWindow::ShowPreview(const plSharedPtr<plAudioClipPreview>& pPreview)
{
  m_pShownPreview = pPreview;

  if (pPreview != nullptr)
    m_pWaveView->SetPreview(pPreview);

  UpdateLoopLabel();

  const QString sDot = QString::fromUtf8(" \xC2\xB7 ");

  // the stat row: label, value, tooltip
  auto showStats = [this](const QString& sResident, const QString& sDisk, const QString& sDecode, const QString& sLoudness)
  {
    plEditorUiAudioClipWaveform::Stat stats[4];
    stats[0] = {"Resident", sResident.toUtf8().data(), "Memory the clip holds while loaded"};
    stats[1] = {"Disk", sDisk.toUtf8().data(), "Size of the built clip on disk (default language)"};
    stats[2] = {"Decode cost", sDecode.toUtf8().data(), "CPU work per playing voice"};
    stats[3] = {"Loudness", sLoudness.toUtf8().data(), ""};
    m_pWaveView->SetStats(stats);
  };

  if (pPreview == nullptr || !pPreview->m_bSuccess)
  {
    SetInfoText(pPreview != nullptr ? QString::fromUtf8(pPreview->m_sError.GetData()) : QString());
    showStats("-", "-", "-", "-");
    return;
  }

  const plAudioClipHeader& header = pPreview->m_Header;
  const double fDuration = header.m_uiSampleRate > 0 ? static_cast<double>(header.m_uiFrames) / header.m_uiSampleRate : 0.0;
  const float fLufs = header.m_fMeasuredLufs + header.m_fAppliedGainDb;

  QString sChannels = header.m_uiChannels == 1 ? QString("mono") : header.m_uiChannels == 2 ? QString("stereo")
                                                                                             : QString::asprintf("%u ch", header.m_uiChannels);

  QString sInfo = QString::asprintf("%.1f kHz", header.m_uiSampleRate / 1000.0) + sDot + sChannels + sDot + QString::asprintf("%.2f s", fDuration);
  if (plMath::IsFinite(fLufs))
    sInfo += sDot + QString::asprintf("%.1f LUFS", fLufs);
  sInfo += sDot + CodecName(header.m_eCodec) + sDot + LoadModeName(header.m_eLoadMode);
  SetInfoText(sInfo);

  // estimates
  const plAudioClipLanguageEntry& lang = header.m_Languages[0];
  const plUInt64 uiPcmBytes = header.m_uiFrames * header.m_uiChannels * sizeof(plInt16);

  plUInt64 uiResident = 0;
  switch (header.m_eLoadMode)
  {
    case plAudioClipLoadMode::DecompressOnLoad:
      uiResident = uiPcmBytes;
      break;
    case plAudioClipLoadMode::CompressedInMemory:
      uiResident = lang.m_uiPayloadBytes;
      break;
    case plAudioClipLoadMode::Streamed:
      uiResident = kStreamRingBytes + lang.m_uiPrimingBytes + lang.m_uiLoopPrimingBytes;
      break;
  }

  QString sDecode;
  if (header.m_eLoadMode == plAudioClipLoadMode::DecompressOnLoad)
  {
    sDecode = header.m_eCodec == plAudioClipCodec::Pcm16 ? QString("none") : QString("once on load (%1)").arg(CodecName(header.m_eCodec));
  }
  else
  {
    switch (header.m_eCodec)
    {
      case plAudioClipCodec::Pcm16:
        sDecode = "none";
        break;
      case plAudioClipCodec::ImaAdpcm:
        sDecode = "low (ADPCM per voice)";
        break;
      case plAudioClipCodec::Opus:
        sDecode = "high (Opus per voice)";
        break;
    }

    if (header.m_eLoadMode == plAudioClipLoadMode::Streamed)
      sDecode += " + disk stream";
  }

  QString sStreamNote = header.m_eLoadMode == plAudioClipLoadMode::Streamed ? QString(" per voice") : QString();

  QString sLoudness;
  if (!plMath::IsFinite(header.m_fMeasuredLufs))
    sLoudness = QString::asprintf("too short to measure (gain %+.1f dB)", header.m_fAppliedGainDb);
  else if (pPreview->m_Settings.m_fNormaliseLufs != 0.0f)
    sLoudness = QString::asprintf("normalised to %.1f LUFS (gain %+.1f dB)", fLufs, header.m_fAppliedGainDb);
  else
    sLoudness = QString::asprintf("%.1f LUFS (not normalised)", header.m_fMeasuredLufs);

  showStats(FormatBytes(uiResident) + sStreamNote, FormatBytes(pPreview->m_uiFileBytes), sDecode, sLoudness);
}

void plAudioClipAssetDocumentWindow::UpdateLoopLabel()
{
  if (m_pShownPreview == nullptr || !m_pShownPreview->m_bSuccess || !m_pShownPreview->m_Header.m_bHasLoop || m_pShownPreview->m_Header.m_uiSampleRate == 0)
  {
    SetLoopText("no loop");
    return;
  }

  const plAudioClipHeader& header = m_pShownPreview->m_Header;
  SetLoopText(QString::asprintf("loop %.2f s - %.2f s", static_cast<double>(header.m_uiLoopStart) / header.m_uiSampleRate, static_cast<double>(header.m_uiLoopEnd) / header.m_uiSampleRate));
}

void plAudioClipAssetDocumentWindow::RefreshUsedBy()
{
  m_bUsedByDirty = false;

  plSet<plUuid> uses;
  plAssetCurator::GetSingleton()->FindAllUses(GetDocument()->GetGuid(), uses, false);

  plHybridArray<plEditorUiAudioClipUsedBy::Entry, 8> entries;
  for (const plUuid& guid : uses)
  {
    plAssetCurator::plLockedSubAsset pSubAsset = plAssetCurator::GetSingleton()->GetSubAsset(guid);
    if (pSubAsset == nullptr || pSubAsset->m_pAssetInfo == nullptr)
      continue;

    plEditorUiAudioClipUsedBy::Entry& entry = entries.ExpandAndGetRef();
    entry.m_sPath = pSubAsset->m_pAssetInfo->m_Path.GetDataDirParentRelativePath();
    entry.m_sAbsPath = pSubAsset->m_pAssetInfo->m_Path.GetAbsolutePath();
  }

  m_pUsedByView->SetEntries(entries);
}

void plAudioClipAssetDocumentWindow::OnPlay()
{
  // the engine process plays the clip as built; unsaved settings are heard once the clip is transformed
  plAudioAuditionMsgToEngine msg;
  msg.m_Command = plAudioAuditionCommand::Play;

  plMemoryStreamContainerWrapperStorage<plDataBuffer> storage(&msg.m_EventData);
  plMemoryStreamWriter writer(&storage);
  if (m_pAssetDoc->WriteAuditionData(writer, IsLoopEnabled()).Failed())
    return;

  if (plPlasmaAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plPlasmaAudioProjectPreferences>())
    msg.m_sMixer = pPreferences->GetAuditionMixer();

  m_bAuditioning = m_pAssetDoc->SendMessageToEngine(&msg);

  if (!m_bAuditioning)
    ShowTemporaryStatusBarMsg("The engine process is not running; the clip cannot be auditioned.");
  else if (m_pAssetDoc->IsModified())
    ShowTemporaryStatusBarMsg("Auditioning the last built clip: save and transform it to hear the current settings.");
}

void plAudioClipAssetDocumentWindow::OnStop()
{
  plAudioAuditionMsgToEngine msg;
  msg.m_Command = plAudioAuditionCommand::Stop;
  m_pAssetDoc->SendMessageToEngine(&msg);
}

void plAudioClipAssetDocumentWindow::OnAuditionStatus(const plAudioAuditionStatusMsgToEditor& status)
{
  if (!status.m_sError.IsEmpty())
    ShowTemporaryStatusBarMsg(plFmt("Audition: {}", status.m_sError));

  // the polls stop once the audition is over
  if (!status.m_bPlaying)
    m_bAuditioning = false;
}

plInt64 plAudioClipAssetDocumentWindow::GetAuditionCursor() const
{
  const plAudioAuditionStatusMsgToEditor& status = m_pAssetDoc->GetAuditionStatus();
  const plSharedPtr<plAudioClipPreview>& pPreview = m_pShownPreview;

  if (!m_bAuditioning || !status.m_bPlaying || status.m_bWaiting || pPreview == nullptr || !pPreview->m_bSuccess || pPreview->m_Header.m_uiSampleRate == 0)
    return -1;

  // a looping clip plays its intro once and then repeats its loop region
  const plAudioClipHeader& header = pPreview->m_Header;
  plInt64 iFrame = static_cast<plInt64>(status.m_fPosition * header.m_uiSampleRate);

  if (IsLoopEnabled() && header.m_bHasLoop && header.m_uiLoopEnd > header.m_uiLoopStart && iFrame >= static_cast<plInt64>(header.m_uiLoopEnd))
  {
    const plInt64 iLoopLength = static_cast<plInt64>(header.m_uiLoopEnd - header.m_uiLoopStart);
    iFrame = static_cast<plInt64>(header.m_uiLoopStart) + (iFrame - static_cast<plInt64>(header.m_uiLoopStart)) % iLoopLength;
  }

  return iFrame < static_cast<plInt64>(header.m_uiFrames) ? iFrame : -1;
}
