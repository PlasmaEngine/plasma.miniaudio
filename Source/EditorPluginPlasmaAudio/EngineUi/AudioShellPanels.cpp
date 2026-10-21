#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/EngineShell/AssetDocumentContent.h>
#include <EditorFramework/EngineShell/DocumentFrame.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipWaveformView.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineViews.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioShellPanels.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAsset.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventDeckView.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventTimelineView.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerAsset.h>
#include <EditorUi/DockManager.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <GuiFoundation/EngineUi/EngineUiContentWidget.h>
#include <GuiFoundation/PropertyGrid/EnginePropertyGridWidget.h>

namespace
{
  plAudioShellPanels s_Provider;

  /// The event timeline shows at least the tabs, the ruler, two tracks and the lanes.
  constexpr float MinTimelineHeight = 200.0f;

  /// The logical height a one-row view asks for (a control strip).
  float GetRowHeight(const plQtEngineUiContentWidget* pWidget)
  {
    if (pWidget == nullptr)
      return 0.0f;

    return plMath::Ceil(static_cast<float>(pWidget->sizeHint().height()) / plMath::Max(pWidget->GetUiScale(), 0.01f));
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// plEditorUiAudioShellStack

plEditorUiAudioShellStack::plEditorUiAudioShellStack(bool bHorizontal)
  : plEditorUiContent("AudioShellStack")
  , m_bHorizontal(bHorizontal)
{
}

void plEditorUiAudioShellStack::AddPart(plEditorUiContent* pPart, float fExtent, float fWeight)
{
  if (pPart == nullptr)
    return;

  m_Parts.PushBack({pPart, fExtent, fWeight});
  RequestRedraw();
}

plEditorUiContent* plEditorUiAudioShellStack::AddOwnedPart(plUniquePtr<plEditorUiContent>&& pPart, float fExtent, float fWeight)
{
  plEditorUiContent* pChild = AddChild(std::move(pPart));
  AddPart(pChild, fExtent, fWeight);
  return pChild;
}

plEditorUiContent* plEditorUiAudioShellStack::AddLentPart(plEditorUiDocumentFrame& ref_frame, QWidget* pWindow, const char* szName, float fExtent, float fWeight)
{
  plQtEngineUiContentWidget* pWidget = plAudioShellPanels::FindView(pWindow, szName);
  if (pWidget == nullptr)
    return nullptr;

  // a row keeps the height its view asks for
  if (fExtent < 0.0f)
    fExtent = GetRowHeight(pWidget);

  plEditorUiContent* pContent = ref_frame.AddLentChild(*this, *pWidget);
  AddPart(pContent, fExtent, fWeight);
  return pContent;
}

void plEditorUiAudioShellStack::SetExtentSource(const plEditorUiContent* pPart, plDelegate<float(float)> extent)
{
  for (Part& part : m_Parts)
  {
    if (part.m_pContent == pPart)
      part.m_Extent = extent;
  }
}

bool plEditorUiAudioShellStack::IsChild(const plEditorUiContent* pContent) const
{
  for (plUInt32 i = 0; i < GetChildCount(); ++i)
  {
    if (GetChild(i) == pContent)
      return true;
  }
  return false;
}

void plEditorUiAudioShellStack::OnUiUpdate()
{
  if (!IsReady())
    return;

  if (!m_bBuilt)
  {
    m_bBuilt = true;

    // with separators, the background shows through the gaps between the parts
    plUiPanel* pRoot = plEditorUi::Panel();
    pRoot->m_Color = m_bSeparators ? plEditorUi::Line() : plEditorUi::Surface();
    SetDocument(plEditorUi::Finish(pRoot));
  }

  PlaceParts();
}

void plEditorUiAudioShellStack::PlaceParts()
{
  const plVec2 vSize = GetSurface()->GetLogicalSize();
  const float fLength = m_bHorizontal ? vSize.x : vSize.y;
  const float fAcross = m_bHorizontal ? vSize.y : vSize.x;
  const float fGap = m_bSeparators ? 1.0f : 0.0f;

  // a lent view that went back is no longer a child
  plHybridArray<Part, 6> parts;
  for (const Part& part : m_Parts)
  {
    if (!IsChild(part.m_pContent))
      continue;

    Part& placed = parts.ExpandAndGetRef();
    placed = part;
    if (part.m_Extent.IsValid())
      placed.m_fExtent = part.m_Extent(fLength);
  }

  float fFixed = parts.IsEmpty() ? 0.0f : fGap * (parts.GetCount() - 1);
  float fWeights = 0.0f;
  for (const Part& part : parts)
  {
    if (part.m_fExtent > 0.0f)
      fFixed += part.m_fExtent;
    else
      fWeights += part.m_fWeight;
  }

  const float fShared = plMath::Max(fLength - fFixed, 0.0f);
  float fAt = 0.0f;
  for (const Part& part : parts)
  {
    float fExtent = part.m_fExtent > 0.0f ? part.m_fExtent : (fWeights > 0.0f ? fShared * part.m_fWeight / fWeights : 0.0f);
    fExtent = plMath::Min(fExtent, plMath::Max(fLength - fAt, 0.0f));

    const plRectFloat rect = m_bHorizontal ? plRectFloat(fAt, 0.0f, fExtent, fAcross) : plRectFloat(0.0f, fAt, fAcross, fExtent);
    SetChildRect(part.m_pContent, rect);
    fAt += fExtent + fGap;
  }
}

//////////////////////////////////////////////////////////////////////////
// plAudioShellPanels

bool plAudioShellPanels::Matches(const plDocument* pDocument) const
{
  // the Qt views of the windows cannot be lent
  if (!plAudioEngineUi::UseEngineViews())
    return false;

  return plDynamicCast<const plAudioClipAssetDocument*>(pDocument) != nullptr || plDynamicCast<const plAudioEventAssetDocument*>(pDocument) != nullptr ||
         plDynamicCast<const plAudioMixerAssetDocument*>(pDocument) != nullptr;
}

void plAudioShellPanels::CreatePanels(plEditorUiDocumentFrame& ref_frame) const
{
  plDocument* pDocument = ref_frame.GetDocument();
  if (plDynamicCast<plAudioClipAssetDocument*>(pDocument) != nullptr)
    CreateClipPanels(ref_frame);
  else if (plDynamicCast<plAudioEventAssetDocument*>(pDocument) != nullptr)
    CreateEventPanels(ref_frame);
  else if (plDynamicCast<plAudioMixerAssetDocument*>(pDocument) != nullptr)
    CreateMixerPanels(ref_frame);
}

void plAudioShellPanels::Register()
{
  plEditorUiDocumentContentProvider::Register(&s_Provider);
}

void plAudioShellPanels::Unregister()
{
  plEditorUiDocumentContentProvider::Unregister(&s_Provider);
}

plQtEngineUiContentWidget* plAudioShellPanels::FindView(QWidget* pWindow, const char* szName)
{
  if (pWindow == nullptr)
    return nullptr;

  // engine hosts have no meta object of their own, so findChild cannot match them
  QList<QWidget*> widgets;
  plQtEngineUiWidget::CollectWindowWidgets(pWindow, widgets);
  for (QWidget* pWidget : widgets)
  {
    auto* pView = dynamic_cast<plQtEngineUiContentWidget*>(pWidget);
    if (pView != nullptr && pView->GetContent() != nullptr && pView->objectName() == QLatin1String(szName))
      return pView;
  }
  return nullptr;
}

void plAudioShellPanels::CreateClipPanels(plEditorUiDocumentFrame& ref_frame)
{
  QWidget* pWindow = ref_frame.GetContextWindow();
  plAssetDocument* pDocument = static_cast<plAssetDocument*>(ref_frame.GetDocument());

  // Audio Clip: the transport and info row over the tabs, ruler, waveform and stat row
  auto* pClip = static_cast<plEditorUiAudioShellStack*>(ref_frame.AddContent({"AudioClipWaveformDockWidget", "Audio Clip"}, PL_DEFAULT_NEW(plEditorUiAudioShellStack)));
  pClip->AddLentPart(ref_frame, pWindow, "AudioClipToolStrip", -1.0f);
  pClip->AddLentPart(ref_frame, pWindow, "AudioClipWaveform", 0.0f);

  // Clip Properties: the platform the grid edits, the asset's status and grid, and the assets that use the clip
  auto* pProperties = static_cast<plEditorUiAudioShellStack*>(ref_frame.AddContent({"AudioClipAssetDockWidget", "Clip Properties"}, PL_DEFAULT_NEW(plEditorUiAudioShellStack)));
  pProperties->AddLentPart(ref_frame, pWindow, "AudioClipPlatform", -1.0f);
  pProperties->AddOwnedPart(PL_DEFAULT_NEW(plEditorUiAssetProperties, pDocument, pWindow), 0.0f);
  pProperties->AddLentPart(ref_frame, pWindow, "AudioClipUsedBy", plEditorUiAudioClipUsedBy::Height);

  ref_frame.GetPanels().DockPanel("AudioClipAssetDockWidget", "AudioClipWaveformDockWidget", plEditorUiDockSide::Right, 0.26f);
}

void plAudioShellPanels::CreateEventPanels(plEditorUiDocumentFrame& ref_frame)
{
  QWidget* pWindow = ref_frame.GetContextWindow();
  plAssetDocument* pDocument = static_cast<plAssetDocument*>(ref_frame.GetDocument());

  // Audio Event: the toolbar row with the OUT meter, the parameter tabs and timeline, the deck and the status row
  auto* pMain = static_cast<plEditorUiAudioShellStack*>(ref_frame.AddContent({"AudioEventTimelineDockWidget", "Audio Event"}, PL_DEFAULT_NEW(plEditorUiAudioShellStack)));

  const float fToolRow = GetRowHeight(FindView(pWindow, "AudioEventToolStrip"));
  auto* pToolRow = static_cast<plEditorUiAudioShellStack*>(pMain->AddOwnedPart(PL_DEFAULT_NEW(plEditorUiAudioShellStack, true), fToolRow));
  pToolRow->AddLentPart(ref_frame, pWindow, "AudioEventToolStrip", 0.0f);
  pToolRow->AddLentPart(ref_frame, pWindow, "AudioEventMeter", plEditorUiAudioMeter::GetSize().x);

  // the tracks without empty space under them, scrolling past half the panel
  if (auto* pTimeline = static_cast<plEditorUiAudioEventTimeline*>(pMain->AddLentPart(ref_frame, pWindow, "AudioEventTimeline", 1.0f)))
  {
    pMain->SetExtentSource(pTimeline, [pTimeline](float fLength)
      { return plMath::Clamp(pTimeline->GetPreferredHeight(), plMath::Min(MinTimelineHeight, fLength * 0.5f), fLength * 0.5f); });
  }

  // INSTRUMENT and PLAYBACK, as high as the playlist needs
  if (auto* pDeck = static_cast<plEditorUiAudioEventDeck*>(pMain->AddLentPart(ref_frame, pWindow, "AudioEventDeck", 1.0f)))
    pMain->SetExtentSource(pDeck, [pDeck](float) { return pDeck->GetContentHeight(); });

  // AUTOMATION beside DETAILS · selection take the rest
  auto* pLower = static_cast<plEditorUiAudioShellStack*>(pMain->AddOwnedPart(PL_DEFAULT_NEW(plEditorUiAudioShellStack, true), 0.0f));
  pLower->SetSeparators(true);
  pLower->AddLentPart(ref_frame, pWindow, "AudioEventAutomation", 0.0f);

  auto* pDetails = static_cast<plEditorUiAudioShellStack*>(pLower->AddOwnedPart(PL_DEFAULT_NEW(plEditorUiAudioShellStack), 0.0f));
  pDetails->AddLentPart(ref_frame, pWindow, "AudioEventDetailsHeader", plEditorUiAudioSectionHeader::GetHeight());

  plUniquePtr<plEditorUiPropertyGrid> pSelectionGrid = PL_DEFAULT_NEW(plEditorUiPropertyGrid);
  pSelectionGrid->SetContextWindow(pWindow);
  pSelectionGrid->FollowSelection(pDocument);
  pDetails->AddOwnedPart(std::move(pSelectionGrid), 0.0f);

  pMain->AddLentPart(ref_frame, pWindow, "AudioEventStatus", plEditorUiAudioStatusRow::GetHeight());

  // Event Macros: the asset's status and the event's own properties, whatever is selected
  auto* pMacros = static_cast<plEditorUiAssetProperties*>(ref_frame.AddContent({"AudioEventAssetDockWidget", "Event Macros"}, PL_DEFAULT_NEW(plEditorUiAssetProperties, pDocument, pWindow)));
  plDeque<const plDocumentObject*> properties;
  properties.PushBack(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);
  pMacros->GetGrid().FollowSelection(nullptr);
  pMacros->GetGrid().SetSourceDocument(pDocument);
  pMacros->GetGrid().SetSelection(properties);

  ref_frame.GetPanels().DockPanel("AudioEventAssetDockWidget", "AudioEventTimelineDockWidget", plEditorUiDockSide::Right, 0.24f);
}

void plAudioShellPanels::CreateMixerPanels(plEditorUiDocumentFrame& ref_frame)
{
  QWidget* pWindow = ref_frame.GetContextWindow();
  plAssetDocument* pDocument = static_cast<plAssetDocument*>(ref_frame.GetDocument());

  // Mixer Console: the add buttons and the live meter toggle, the strips and the status row
  auto* pConsole = static_cast<plEditorUiAudioShellStack*>(ref_frame.AddContent({"AudioMixerConsoleDockWidget", "Mixer Console"}, PL_DEFAULT_NEW(plEditorUiAudioShellStack)));
  pConsole->AddLentPart(ref_frame, pWindow, "AudioMixerToolStrip", -1.0f);
  pConsole->AddLentPart(ref_frame, pWindow, "AudioMixerStrips", 0.0f);
  pConsole->AddLentPart(ref_frame, pWindow, "AudioMixerStatus", plEditorUiAudioStatusRow::GetHeight());

  if (plQtEngineUiContentWidget* pRouting = FindView(pWindow, "AudioMixerRouting"))
    ref_frame.AddLentContent({"AudioMixerRoutingDockWidget", "Routing"}, *pRouting);

  if (plQtEngineUiContentWidget* pSnapshots = FindView(pWindow, "AudioMixerSnapshots"))
    ref_frame.AddLentContent({"AudioMixerSnapshotsDockWidget", "Snapshots"}, *pSnapshots);

  // the selected bus, VCA or snapshot
  ref_frame.AddContent({"AudioMixerAssetDockWidget", "Properties"}, PL_DEFAULT_NEW(plEditorUiAssetProperties, pDocument, pWindow));

  // Routing on the left, Snapshots first on the right with Properties as its neighbour tab
  plEditorUiDockManager& panels = ref_frame.GetPanels();
  panels.DockPanel("AudioMixerRoutingDockWidget", "AudioMixerConsoleDockWidget", plEditorUiDockSide::Left, 0.2f);
  panels.DockPanel("AudioMixerSnapshotsDockWidget", "AudioMixerConsoleDockWidget", plEditorUiDockSide::Right, 0.26f);
  panels.DockPanel("AudioMixerAssetDockWidget", "AudioMixerSnapshotsDockWidget", plEditorUiDockSide::Tab);
  panels.SetCurrentPanel("AudioMixerSnapshotsDockWidget");
}
