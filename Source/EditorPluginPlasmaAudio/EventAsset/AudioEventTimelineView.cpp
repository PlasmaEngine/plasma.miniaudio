#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/EditorApp/EditorApp.moc.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAsset.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventTimelineView.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <GameUiPlugin/Components/UiCanvasComponent.h>
#include <MiniAudio/miniaudio.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

namespace
{
  constexpr float s_fTabRow = plEditorUiTabs::Height + 4.0f;
  constexpr float s_fRuler = 24.0f;
  constexpr float s_fRow = 66.0f;
  constexpr float s_fHead = 156.0f;
  constexpr float s_fLane = 28.0f;
  constexpr float s_fTimeInset = 10.0f;
  constexpr double s_fPixelsPerSecond = 120.0;
  constexpr float s_fHandleGrab = 5.0f;
  constexpr float s_fWheelStep = 40.0f;

  /// Length drawn for instruments whose entries could not be measured.
  constexpr double s_fPlaceholderLength = 0.6;

  const plHashedString s_sCanvasEntry = plMakeHashedString("Timeline.Canvas");
  const plHashedString s_sRenameText = plMakeHashedString("Timeline.RenameText");
  constexpr const char* s_szTabs = "Timeline.Tabs";
  constexpr const char* s_szAddParameter = "Timeline.AddParameter";
  constexpr const char* s_szRename = "Timeline.Rename";
  constexpr const char* s_szRenameSubmit = "Timeline.RenameSubmit";

  void GatherChildrenOfProperty(const plDocumentObject* pParent, const char* szProperty, plHybridArray<const plDocumentObject*, 8>& out_children)
  {
    out_children.Clear();

    if (pParent == nullptr)
      return;

    for (const plDocumentObject* pChild : pParent->GetChildren())
    {
      if (pChild->GetParentProperty().IsEqual(szProperty))
        out_children.PushBack(pChild);
    }
  }

  /// Track colours from the theme's system colours, in this order.
  plColorGammaUB TrackColor(plUInt32 uiIndex)
  {
    switch (uiIndex % 6)
    {
      case 0:
        return plEditorUi::Axis(2);
      case 1:
        return plAudioEngineUi::CategoryColor("Gameplay");
      case 2:
        return plAudioEngineUi::CategoryColor("Animation");
      case 3:
        return plAudioEngineUi::CategoryColor("Scripting");
      case 4:
        return plAudioEngineUi::CategoryColor("XR");
      default:
        return plAudioEngineUi::CategoryColor("Effects");
    }
  }

  void AddQuad(plDynamicArray<plVec2>& ref_points, float x, float y, float w, float h)
  {
    ref_points.PushBack(plVec2(x, y));
    ref_points.PushBack(plVec2(x + w, y));
    ref_points.PushBack(plVec2(x + w, y + h));
    ref_points.PushBack(plVec2(x, y));
    ref_points.PushBack(plVec2(x + w, y + h));
    ref_points.PushBack(plVec2(x, y + h));
  }
} // namespace

plEditorUiAudioEventTimeline::plEditorUiAudioEventTimeline(plDocument* pDocument)
  : plEditorUiContent("AudioEventTimeline")
  , m_pDocument(pDocument)
{
  if (IsReady())
  {
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioEventTimeline::OnPointer, this));
    m_KeySubscription = GetSurface()->m_KeyEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioEventTimeline::OnKey, this));

    m_pTabs = PL_DEFAULT_NEW(plEditorUiTabs, *GetSurface(), s_szTabs);
    m_TabSubscription = m_pTabs->m_CurrentChanged.AddEventHandler(plMakeDelegate(&plEditorUiAudioEventTimeline::OnTabChanged, this));
  }
}

plEditorUiAudioEventTimeline::~plEditorUiAudioEventTimeline()
{
  if (IsReady())
  {
    m_pTabs->m_CurrentChanged.RemoveEventHandler(m_TabSubscription);
    GetSurface()->m_KeyEvents.RemoveEventHandler(m_KeySubscription);
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
  }
}

void plEditorUiAudioEventTimeline::Refresh()
{
  UpdateTabs();
  m_bDrawDirty = true;
}

void plEditorUiAudioEventTimeline::ClearDurationCache()
{
  m_DurationCache.Clear();
  m_bDrawDirty = true;
}

//////////////////////////////////////////////////////////////////////////
// Document data

const plDocumentObject* plEditorUiAudioEventTimeline::GetPropertiesObject() const
{
  const auto& children = m_pDocument->GetObjectManager()->GetRootObject()->GetChildren();
  return children.IsEmpty() ? nullptr : children[0];
}

void plEditorUiAudioEventTimeline::GatherInstruments(plHybridArray<const plDocumentObject*, 8>& out_instruments) const
{
  GatherChildrenOfProperty(GetPropertiesObject(), "Instruments", out_instruments);
}

double plEditorUiAudioEventTimeline::GetSampleDuration(plStringView sFile) const
{
  if (sFile.IsEmpty())
    return 0.0;

  const plString sKey = sFile;

  if (auto it = m_DurationCache.Find(sKey); it.IsValid())
    return it.Value();

  double fDuration = 0.0;

  plStringBuilder sAbsPath = sFile;
  if (plQtEditorApp::GetSingleton()->MakeDataDirectoryRelativePathAbsolute(sAbsPath))
  {
    ma_decoder decoder;
    if (ma_decoder_init_file(sAbsPath.GetData(), nullptr, &decoder) == MA_SUCCESS)
    {
      ma_uint64 uiFrames = 0;
      if (ma_decoder_get_length_in_pcm_frames(&decoder, &uiFrames) == MA_SUCCESS && decoder.outputSampleRate > 0)
        fDuration = static_cast<double>(uiFrames) / static_cast<double>(decoder.outputSampleRate);

      ma_decoder_uninit(&decoder);
    }
  }

  m_DurationCache[sKey] = fDuration;
  return fDuration;
}

double plEditorUiAudioEventTimeline::GetEntryDuration(const plDocumentObject* pEntry) const
{
  const plString sClip = pEntry->GetTypeAccessor().GetValue("Clip").ConvertTo<plString>();
  if (!sClip.IsEmpty())
  {
    plAudioClipAssetMetaInfo info;
    return plAudioClipAssetUtils::GetClipInfo(sClip, info) ? info.m_fDuration : 0.0;
  }

  return GetSampleDuration(pEntry->GetTypeAccessor().GetValue("File").ConvertTo<plString>());
}

double plEditorUiAudioEventTimeline::GetLongestEntry(const plDocumentObject* pInstrument) const
{
  plHybridArray<const plDocumentObject*, 8> playlist;
  GatherChildrenOfProperty(pInstrument, "Playlist", playlist);

  double fLongest = 0.0;
  for (const plDocumentObject* pEntry : playlist)
    fLongest = plMath::Max(fLongest, GetEntryDuration(pEntry));

  return fLongest;
}

double plEditorUiAudioEventTimeline::GetInstrumentDelay(const plDocumentObject* pInstrument) const
{
  if (pInstrument == nullptr)
    return 0.0;

  // a clip being dragged shows its new position before it is committed
  if (m_bDraggingClip && pInstrument->GetGuid() == m_DragClipGuid)
    return m_fDragClipDelay;

  return pInstrument->GetTypeAccessor().GetValue("StartDelay").ConvertTo<double>();
}

double plEditorUiAudioEventTimeline::GetMarkerValue(const char* szProperty, DragHandle handle) const
{
  if (m_ActiveHandle == handle && m_bHandlePreviewValid)
    return m_fHandlePreview;

  const plDocumentObject* pProps = GetPropertiesObject();
  return pProps != nullptr ? pProps->GetTypeAccessor().GetValue(szProperty).ConvertTo<double>() : 0.0;
}

double plEditorUiAudioEventTimeline::GetTimeExtent() const
{
  // the event's end: the latest point at which any instrument stops sounding; the stop fade anchors to it
  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherInstruments(instruments);

  double fEnd = 0.0;
  for (const plDocumentObject* pInstrument : instruments)
  {
    const double fLength = GetLongestEntry(pInstrument);
    fEnd = plMath::Max(fEnd, GetInstrumentDelay(pInstrument) + plMath::Max(fLength, s_fPlaceholderLength));
  }

  // an empty event still needs a usable ruler
  return fEnd > 0.0 ? fEnd : 2.0;
}

//////////////////////////////////////////////////////////////////////////
// Geometry

float plEditorUiAudioEventTimeline::GetCanvasTop() const
{
  return s_fTabRow;
}

float plEditorUiAudioEventTimeline::TimeToX(double fSeconds) const
{
  return s_fHead + s_fTimeInset + static_cast<float>(fSeconds * s_fPixelsPerSecond) - m_fScrollX;
}

double plEditorUiAudioEventTimeline::XToTime(float x) const
{
  return plMath::Max(0.0, static_cast<double>(x + m_fScrollX - s_fHead - s_fTimeInset) / s_fPixelsPerSecond);
}

float plEditorUiAudioEventTimeline::GetRowTop(plUInt32 uiRow) const
{
  return GetCanvasTop() + s_fRuler + uiRow * s_fRow - m_fScrollY;
}

plVec2 plEditorUiAudioEventTimeline::GetContentSize() const
{
  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherInstruments(instruments);

  // padding is pixels, not seconds, so the content end stays at its true time position
  const float fWidth = s_fHead + s_fTimeInset + static_cast<float>(GetTimeExtent() * s_fPixelsPerSecond) + 90.0f;
  const float fHeight = s_fRuler + plMath::Max<plUInt32>(1, instruments.GetCount()) * s_fRow + 2.0f * s_fLane + 8.0f;
  return plVec2(fWidth, fHeight);
}

void plEditorUiAudioEventTimeline::ClampScroll()
{
  if (!IsReady())
    return;

  const plVec2 size = GetSurface()->GetLogicalSize();
  const plVec2 content = GetContentSize();

  m_fScrollX = plMath::Clamp(m_fScrollX, 0.0f, plMath::Max(content.x - size.x, 0.0f));
  m_fScrollY = plMath::Clamp(m_fScrollY, 0.0f, plMath::Max(content.y - (size.y - GetCanvasTop()), 0.0f));
}

//////////////////////////////////////////////////////////////////////////
// Document and tabs

void plEditorUiAudioEventTimeline::UpdateTabs()
{
  plHybridArray<const plDocumentObject*, 8> parameters;
  GatherChildrenOfProperty(GetPropertiesObject(), "Parameters", parameters);

  const plDocumentObject* pSelected = m_pDocument->GetSelectionManager()->GetCurrentObject();

  plHybridArray<plString, 8> labels;
  plHybridArray<plUuid, 8> guids;
  plUInt32 uiCurrent = 0;

  labels.PushBack("Timeline");
  guids.PushBack(plUuid());

  for (const plDocumentObject* pParameter : parameters)
  {
    plStringBuilder sName = pParameter->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      sName = "<unnamed>";
    else if (sName.IsEqual_NoCase("Distance"))
      sName.Append(" (built-in)");

    if (pSelected != nullptr && pSelected->GetGuid() == pParameter->GetGuid())
      uiCurrent = labels.GetCount();

    labels.PushBack(sName);
    guids.PushBack(pParameter->GetGuid());
  }

  const bool bLabelsChanged = labels != m_TabLabels;

  m_TabLabels = labels;
  m_TabParameters = guids;

  if (bLabelsChanged)
  {
    m_uiCurrentTab = uiCurrent;
    m_bRebuild = true;
  }
  else if (uiCurrent != m_uiCurrentTab)
  {
    m_uiCurrentTab = uiCurrent;
    if (m_pTabs != nullptr && !m_bRebuild)
      m_pTabs->SetCurrent(uiCurrent);
  }
}

void plEditorUiAudioEventTimeline::OnUiUpdate()
{
  if (!IsReady())
    return;

  if (GetSurface()->GetLogicalSize() != m_vBuiltSize)
    m_bRebuild = true;

  // the rename field commits when it loses focus
  if (m_RenameGuid.IsValid() && !m_bRebuild)
  {
    const bool bFocused = GetSurface()->GetCanvas().GetFocusedElementName() == s_szRename;
    if (bFocused)
      m_bRenameHadFocus = true;
    else if (m_bRenameHadFocus)
      CommitRename();
  }

  if (m_bRebuild)
    BuildDocument();

  if (m_bDrawDirty)
    Draw();
}

void plEditorUiAudioEventTimeline::BuildDocument()
{
  using namespace plEditorUi;

  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();
  ClampScroll();

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = Surface();

  DrawList(pRoot, "Timeline.Canvas", s_sCanvasEntry.GetView());

  // dock-style tabs for the timeline and the parameters, then the dashed "+ Parameter" slot
  plHybridArray<plEditorUiTab, 8> tabs;
  float fTabsWidth = 0.0f;
  for (const plString& sLabel : m_TabLabels)
  {
    tabs.ExpandAndGetRef().m_sLabel = sLabel;

    plStringBuilder sUpper(sLabel);
    sUpper.ToUpper();
    fTabsWidth += 20.0f + MeasureText(sUpper, FontSize - 2);
  }

  fTabsWidth = plMath::Min(fTabsWidth, plMath::Max(m_vBuiltSize.x - 140.0f, 60.0f));

  plUiPanel* pTabHost = Panel(pRoot);
  plAudioEngineUi::Place(pTabHost, plRectFloat(6.0f, 2.0f, fTabsWidth, plEditorUiTabs::Height));
  pTabHost->m_Color = plColorGammaUB(0, 0, 0, 0);

  m_pTabs->SetTabs(tabs);
  m_pTabs->SetCurrent(plMath::Min(m_uiCurrentTab, tabs.GetCount() - 1));
  m_pTabs->Build(pTabHost, fTabsWidth);

  m_fAddTabX = 6.0f + fTabsWidth + 6.0f;
  plStringBuilder sAddLabel("+ PARAMETER");
  plAudioEngineUi::HitButton(pRoot, plRectFloat(m_fAddTabX, 4.0f, MeasureText(sAddLabel, FontSize - 2) + 20.0f, 22.0f), s_szAddParameter, "Adds an event parameter (an RTPC the game sets)", &GetSurface()->GetBlackboard());

  // the inline rename field over the track header
  if (m_RenameGuid.IsValid())
  {
    plHybridArray<const plDocumentObject*, 8> instruments;
    GatherInstruments(instruments);

    for (plUInt32 i = 0; i < instruments.GetCount(); ++i)
    {
      if (instruments[i]->GetGuid() != m_RenameGuid)
        continue;

      plUiTextInput* pInput = Input(pRoot, s_szRename, s_sRenameText.GetView(), "", s_szRenameSubmit);
      plAudioEngineUi::Place(pInput, plRectFloat(8.0f, GetRowTop(i) + 6.0f, s_fHead - 60.0f, 22.0f));
      pInput->m_BorderColor = Accent();
      pInput->m_vCornerRadius = plVec4(3);
    }
  }

  SetDocument(Finish(pRoot));
  m_bDrawDirty = true;

  if (m_RenameGuid.IsValid())
    GetSurface()->GetCanvas().FocusElement(s_szRename, m_vBuiltSize);
}

void plEditorUiAudioEventTimeline::OnTabChanged(plUInt32 uiTab)
{
  m_uiCurrentTab = uiTab;

  if (uiTab >= m_TabParameters.GetCount())
    return;

  const plDocumentObject* pObject = m_TabParameters[uiTab].IsValid() ? m_pDocument->GetObjectManager()->GetObject(m_TabParameters[uiTab]) : GetPropertiesObject();
  if (pObject != nullptr)
    m_pDocument->GetSelectionManager()->SetSelection(pObject);
}

void plEditorUiAudioEventTimeline::OnUiEvent(const plMsgUiEvent& event)
{
  const plStringView sId = event.m_sIdentifier.GetView();

  if (sId == s_szAddParameter && m_OnAddParameter.IsValid())
    m_OnAddParameter();
  else if (sId == s_szRenameSubmit)
    CommitRename();
}

//////////////////////////////////////////////////////////////////////////
// Drawing

void plEditorUiAudioEventTimeline::Draw()
{
  using namespace plEditorUi;

  m_bDrawDirty = false;
  m_Writer.Clear();
  m_Hits.Clear();

  const plVec2 size = GetSurface()->GetLogicalSize();
  const plRectFloat canvas(0.0f, GetCanvasTop(), size.x, plMath::Max(size.y - GetCanvasTop(), 1.0f));

  // the tab row: the painted "+ Parameter" slot and the hairline under the row
  {
    plStringBuilder sAddLabel("+ PARAMETER");
    const plRectFloat addRect(m_fAddTabX, 4.0f, MeasureText(sAddLabel, FontSize - 2) + 20.0f, 22.0f);
    plAudioEngineUi::DashedRect(m_Writer, addRect, Border());
    m_Writer.Text(addRect, sAddLabel, Dim(), FontSize - 2, plUiTextHAlign::Center);
    m_Writer.Rect(plRectFloat(0.0f, canvas.y - 1.0f, size.x, 1.0f), Line());
  }

  m_Writer.Rect(canvas, Background());

  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherInstruments(instruments);

  const plDocumentObject* pProps = GetPropertiesObject();
  const double fExtent = GetTimeExtent();
  const bool bLoopRegion = pProps != nullptr && pProps->GetTypeAccessor().GetValue("LoopRegion").ConvertTo<bool>();
  const float fRulerBottom = canvas.y + s_fRuler;
  const plRectFloat content(s_fHead, fRulerBottom, plMath::Max(size.x - s_fHead, 1.0f), plMath::Max(size.y - fRulerBottom, 1.0f));

  plString sOutputBus = pProps != nullptr ? pProps->GetTypeAccessor().GetValue("OutputBus").ConvertTo<plString>() : plString();
  if (sOutputBus.IsEmpty())
    sOutputBus = "Master";

  const plUuid selected = m_pDocument->GetSelectionManager()->GetCurrentObject() != nullptr ? m_pDocument->GetSelectionManager()->GetCurrentObject()->GetGuid() : plUuid();

  // header column behind the rows
  m_Writer.Rect(plRectFloat(0.0f, fRulerBottom, s_fHead, content.height), Surface());

  m_Writer.PushClip(content);
  {
    // loop region: a neutral band with edges
    if (bLoopRegion)
    {
      const double fLoopStart = GetMarkerValue("LoopStart", DragHandle::LoopStart);
      const double fLoopEnd = GetMarkerValue("LoopEnd", DragHandle::LoopEnd);
      const float x0 = TimeToX(plMath::Min(fLoopStart, fLoopEnd));
      const float x1 = TimeToX(plMath::Max(fLoopStart, fLoopEnd));
      m_Writer.Rect(plRectFloat(x0, content.y, x1 - x0, content.height), plAudioEngineUi::Mix(Background(), Foreground(), 0.05f));
      m_Writer.Rect(plRectFloat(x0, content.y, 1.0f, content.height), Muted());
      m_Writer.Rect(plRectFloat(x1 - 1.0f, content.y, 1.0f, content.height), Muted());
    }

    // end of the event: where the last instrument stops sounding
    const float fEndX = TimeToX(fExtent);
    m_Writer.Line(plVec2(fEndX + 0.5f, content.y), plVec2(fEndX + 0.5f, content.y + content.height), Muted(), 1.0f, 3.0f, 3.0f);

    plStringBuilder sEnd;
    sEnd.SetFormat("end {}s", plArgF(fExtent, 2));
    m_Writer.Text(plRectFloat(fEndX + 4.0f, content.y + 2.0f, 90.0f, 14.0f), sEnd, Muted(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
  }
  m_Writer.PopClip();

  if (instruments.IsEmpty())
  {
    // the empty state centred in the lanes
    const float cy = content.y + content.height * 0.5f;
    plAudioEngineUi::Icon(m_Writer, plRectFloat(content.x + content.width * 0.5f - 11.0f, cy - 46.0f, 22.0f, 22.0f), IconPath(Icon::Event), Dim());
    m_Writer.Text(plRectFloat(content.x, cy - 18.0f, content.width, 18.0f), "No instruments yet.", Muted(), FontSize, plUiTextHAlign::Center);
    m_Writer.Text(plRectFloat(content.x, cy, content.width, 18.0f), "Use '+ Instrument' to add a track, then fill its playlist with '+ Add Files...'.", Dim(), FontSize - 1, plUiTextHAlign::Center);
  }
  else
  {
    const bool bAnySolo = !m_SoloTracks.IsEmpty();
    for (plUInt32 i = 0; i < instruments.GetCount(); ++i)
      DrawTrack(instruments[i], i, bAnySolo, sOutputBus, selected);

    DrawLanes(instruments.GetCount(), selected);
  }

  DrawRuler(canvas, fExtent, bLoopRegion);

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sCanvasEntry);
}

void plEditorUiAudioEventTimeline::DrawRuler(const plRectFloat& canvas, double fExtent, bool bLoopRegion)
{
  using namespace plEditorUi;

  PL_IGNORE_UNUSED(fExtent);

  const plRectFloat ruler(0.0f, canvas.y, canvas.width, s_fRuler);
  m_Writer.Rect(ruler, Surface());
  m_Writer.Rect(plRectFloat(0.0f, ruler.y + ruler.height - 1.0f, ruler.width, 1.0f), Line());

  m_Writer.PushClip(plRectFloat(s_fHead, ruler.y, plMath::Max(ruler.width - s_fHead, 1.0f), ruler.height));

  // minor ticks every 0.1 s, labels every 0.5 s
  const double fFirst = plMath::Floor(XToTime(s_fHead) * 10.0) / 10.0;
  for (plInt32 i = 0;; ++i)
  {
    const double t = fFirst + i * 0.1;
    const float x = plMath::Round(TimeToX(t)) + 0.5f;
    if (x > ruler.width)
      break;

    const plInt32 iTenths = static_cast<plInt32>(plMath::Round(t * 10.0));
    const bool bMajor = (iTenths % 5) == 0;
    const float fTick = bMajor ? 10.0f : 5.0f;
    m_Writer.Rect(plRectFloat(x - 0.5f, ruler.y + ruler.height - fTick, 1.0f, fTick), Border());

    if (bMajor)
    {
      plStringBuilder sLabel;
      sLabel.SetFormat("{}s", plArgF(iTenths / 10.0, 1));
      m_Writer.Text(plRectFloat(x + 3.0f, ruler.y + 2.0f, 50.0f, 12.0f), sLabel, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    }
  }

  // loop region grab handles
  if (bLoopRegion)
  {
    const double fLoopStart = GetMarkerValue("LoopStart", DragHandle::LoopStart);
    const double fLoopEnd = GetMarkerValue("LoopEnd", DragHandle::LoopEnd);
    const float x0 = TimeToX(plMath::Min(fLoopStart, fLoopEnd));
    const float x1 = TimeToX(plMath::Max(fLoopStart, fLoopEnd));

    m_Writer.Rect(plRectFloat(x0, ruler.y, 5.0f, 12.0f), Muted());
    m_Writer.Rect(plRectFloat(x1 - 5.0f, ruler.y, 5.0f, 12.0f), Muted());
    m_Writer.Text(plRectFloat(x0 + 8.0f, ruler.y, 90.0f, 12.0f), "LOOP REGION", Dim(), MinFontSize);
  }

  m_Writer.PopClip();
}

void plEditorUiAudioEventTimeline::DrawTrack(const plDocumentObject* pInstrument, plUInt32 uiIndex, bool bAnySolo, const plString& sOutputBus, const plUuid& selected)
{
  using namespace plEditorUi;

  const plVec2 size = GetSurface()->GetLogicalSize();
  const float fRulerBottom = GetCanvasTop() + s_fRuler;
  const float y = GetRowTop(uiIndex);

  if (y + s_fRow < fRulerBottom || y > size.y)
    return;

  const auto& accessor = pInstrument->GetTypeAccessor();
  const plUuid guid = pInstrument->GetGuid();
  const bool bSelected = guid == selected;
  const bool bMuted = accessor.GetValue("Mute").ConvertTo<bool>();
  const bool bSolo = IsSolo(guid);
  const bool bSilencedBySolo = bAnySolo && !bSolo;
  const bool bAudible = !bMuted && !bSilencedBySolo;
  const bool bLoop = accessor.GetValue("Loop").ConvertTo<bool>();
  const plColorGammaUB trackColor = TrackColor(uiIndex);

  plHybridArray<const plDocumentObject*, 8> playlist;
  GatherChildrenOfProperty(pInstrument, "Playlist", playlist);

  // longest measured entry: 0 when nothing could be decoded (missing files, silence only)
  const double fRealLength = GetLongestEntry(pInstrument);
  const bool bLengthKnown = fRealLength > 0.0;

  // header: track colour edge, name, routing and state, M and S
  m_Writer.PushClip(plRectFloat(0.0f, fRulerBottom, s_fHead, plMath::Max(size.y - fRulerBottom, 1.0f)));
  {
    m_Writer.Rect(plRectFloat(0.0f, y, s_fHead, s_fRow), bSelected ? Selection() : Surface());
    m_Writer.Rect(plRectFloat(0.0f, y, 3.0f, s_fRow), trackColor);

    plStringBuilder sName = accessor.GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      sName.SetFormat("Instrument {0}", uiIndex + 1);

    if (guid != m_RenameGuid)
      m_Writer.Text(plRectFloat(12.0f, y + 8.0f, s_fHead - 66.0f, 20.0f), sName, bAudible ? Foreground() : Muted(), FontSize);

    plStringBuilder sSub;
    sSub.SetFormat("-> {}", sOutputBus);
    if (bMuted)
      sSub.Append(" \xC2\xB7 muted");
    else if (bSilencedBySolo)
      sSub.Append(" \xC2\xB7 silenced (solo)");

    if (bLengthKnown)
      sSub.AppendFormat(" \xC2\xB7 {}s", plArgF(fRealLength, 2));
    else
      sSub.Append(" \xC2\xB7 length ?");

    if (bLoop)
      sSub.Append(" \xE2\x88\x9E");

    m_Writer.Text(plRectFloat(12.0f, y + 30.0f, s_fHead - 18.0f, 16.0f), sSub, Dim(), FontSize - 2);

    // M (saved) in the danger colour, S (session) in the warning colour
    auto toggle = [&](float x, plStringView sLetter, bool bOn, plColorGammaUB onColor, HitKind kind)
    {
      const plRectFloat rect(x, y + 8.0f, 20.0f, 18.0f);
      m_Writer.Rect(rect, bOn ? plAudioEngineUi::Mix(Field(), onColor, 0.2f) : Field(), 3.0f);
      m_Writer.RectOutline(rect, bOn ? onColor : Border(), 1.0f, 3.0f);
      m_Writer.Text(rect, sLetter, bOn ? onColor : Dim(), FontSize - 2, plUiTextHAlign::Center, plUiTextVAlign::Center, true);

      Hit& hit = m_Hits.ExpandAndGetRef();
      hit.m_Kind = kind;
      hit.m_Guid = guid;
      hit.m_Rect = plRectFloat(rect.x - 2.0f, rect.y - 3.0f, rect.width + 4.0f, rect.height + 6.0f);
    };

    toggle(s_fHead - 50.0f, "M", bMuted, Danger(), HitKind::Mute);
    toggle(s_fHead - 26.0f, "S", bSolo, Warning(), HitKind::Solo);
  }
  m_Writer.PopClip();

  Hit& header = m_Hits.ExpandAndGetRef();
  header.m_Kind = HitKind::Header;
  header.m_Guid = guid;
  header.m_Rect = plRectFloat(0.0f, y, s_fHead, s_fRow);

  // the clip block in the lane
  const plRectFloat content(s_fHead, fRulerBottom, plMath::Max(size.x - s_fHead, 1.0f), plMath::Max(size.y - fRulerBottom, 1.0f));
  m_Writer.PushClip(content);
  {
    m_Writer.Rect(plRectFloat(0.0f, y + s_fRow - 1.0f, size.x, 1.0f), Line());

    const double fDelay = GetInstrumentDelay(pInstrument);
    const double fDrawLength = bLengthKnown ? fRealLength : s_fPlaceholderLength;
    const float fClipX = TimeToX(fDelay);
    const float fClipWidth = plMath::Max(24.0f, static_cast<float>(fDrawLength * s_fPixelsPerSecond));
    const plRectFloat clip(fClipX, y + 6.0f, fClipWidth, s_fRow - 12.0f);
    const float fStrength = bAudible ? 1.0f : 0.35f;
    const plColorGammaUB background = Background();

    m_Writer.Rect(clip, plAudioEngineUi::Mix(background, trackColor, 0.18f * fStrength), 3.0f);

    if (bAudible)
      m_Writer.RectOutline(clip, plAudioEngineUi::Mix(background, trackColor, 0.8f), 1.0f, 3.0f);
    else
      plAudioEngineUi::DashedRect(m_Writer, clip, Border());

    if (bSelected)
      m_Writer.RectOutline(clip, Accent(), 1.5f, 3.0f);

    m_Writer.PushClip(clip);
    {
      // a drawn waveform band, not decoded audio, between the name row and the info row
      plDynamicArray<plVec2> bars;
      for (float x = 0.0f; x < clip.width - 4.0f; x += 3.0f)
      {
        const float a = plMath::Abs(plMath::Sin(plAngle::MakeFromRadian(x * 0.21f + uiIndex)) * plMath::Sin(plAngle::MakeFromRadian(x * 0.05f))) * 9.0f * (1.0f - x / clip.width * 0.7f);
        AddQuad(bars, clip.x + 2.0f + x, y + 31.0f - a, 2.0f, a * 2.0f + 1.0f);
      }
      if (!bars.IsEmpty())
        m_Writer.Triangles(bars, plAudioEngineUi::Mix(background, trackColor, fStrength));

      m_Writer.Text(plRectFloat(clip.x + 6.0f, clip.y + 2.0f, clip.width - 10.0f, 16.0f), playlist.GetCount() > 1 ? "Multi Sound" : "Single Sound", bAudible ? Foreground() : Dim(), FontSize - 1);

      const plInt64 iMode = accessor.GetValue("PlaylistMode").ConvertTo<plInt64>();
      const char* szMode = (iMode == plAudioEventPlaylistMode::Sequential) ? "sequential" : (iMode == plAudioEventPlaylistMode::Shuffle ? "shuffle" : "random");
      const double fPitchRand = accessor.GetValue("PitchRandomization").ConvertTo<double>();

      plStringBuilder sInfo;
      sInfo.SetFormat("{} {} \xC2\xB7 {}", playlist.GetCount(), playlist.GetCount() == 1 ? "entry" : "entries", szMode);
      if (fPitchRand > 0.0)
        sInfo.AppendFormat(" \xC2\xB7 \xC2\xB1{} st", plArgF(fPitchRand, 1));
      if (bLengthKnown)
        sInfo.AppendFormat(" \xC2\xB7 {}s", plArgF(fRealLength, 2));
      if (bLoop)
        sInfo.Append(" \xC2\xB7 \xE2\x88\x9E");

      m_Writer.Text(plRectFloat(clip.x + 6.0f, clip.y + clip.height - 16.0f, clip.width - 10.0f, 14.0f), sInfo, bAudible ? Muted() : Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    }
    m_Writer.PopClip();

    // the clip's end as a time position; a placeholder length is marked so it is not mistaken for a measured one
    if (bLengthKnown)
      m_Writer.Rect(plRectFloat(clip.x + clip.width - 1.0f, clip.y + 2.0f, 1.0f, clip.height - 4.0f), plAudioEngineUi::WithAlpha(Foreground(), 70));
    else
      m_Writer.Line(plVec2(clip.x + clip.width - 0.5f, clip.y + 2.0f), plVec2(clip.x + clip.width - 0.5f, clip.y + clip.height - 2.0f), Warning(), 1.0f, 2.0f, 2.0f);

    // while dragging: where the clip starts now
    if (m_bDraggingClip && guid == m_DragClipGuid)
    {
      plStringBuilder sPos;
      sPos.SetFormat("{}s", plArgF(fDelay, 2));
      const plRectFloat badge(clip.x, clip.y - 15.0f, MeasureCodeText(sPos, MinFontSize) + 10.0f, 14.0f);
      m_Writer.Rect(badge, Accent(), 3.0f);
      m_Writer.Text(badge, sPos, Background(), MinFontSize, plUiTextHAlign::Center, plUiTextVAlign::Center, true);
    }

    Hit& hit = m_Hits.ExpandAndGetRef();
    hit.m_Kind = HitKind::Clip;
    hit.m_Guid = guid;
    hit.m_Rect = plRectFloat(clip.x, y, clip.width, s_fRow);
  }
  m_Writer.PopClip();
}

void plEditorUiAudioEventTimeline::DrawLanes(plUInt32 uiInstruments, const plUuid& selected)
{
  using namespace plEditorUi;

  const plVec2 size = GetSurface()->GetLogicalSize();
  const float fRulerBottom = GetCanvasTop() + s_fRuler;
  const float y = GetRowTop(uiInstruments);
  const plDocumentObject* pProps = GetPropertiesObject();

  // headers
  m_Writer.PushClip(plRectFloat(0.0f, fRulerBottom, s_fHead, plMath::Max(size.y - fRulerBottom, 1.0f)));
  m_Writer.Rect(plRectFloat(0.0f, y, s_fHead, 2.0f * s_fLane), Surface());
  m_Writer.Text(plRectFloat(12.0f, y, s_fHead - 18.0f, s_fLane), "Logic", Dim(), FontSize - 1);
  m_Writer.Text(plRectFloat(12.0f, y + s_fLane, s_fHead - 18.0f, s_fLane), "Automation", Dim(), FontSize - 1);
  m_Writer.PopClip();

  const plRectFloat content(s_fHead, fRulerBottom, plMath::Max(size.x - s_fHead, 1.0f), plMath::Max(size.y - fRulerBottom, 1.0f));
  m_Writer.PushClip(content);

  m_Writer.Rect(plRectFloat(0.0f, y + s_fLane - 1.0f, size.x, 1.0f), Line());
  m_Writer.Rect(plRectFloat(0.0f, y + 2.0f * s_fLane - 1.0f, size.x, 1.0f), Line());

  // fade in from 0, stop fade back from the end; the handles are draggable
  if (pProps != nullptr)
  {
    const double fFadeIn = GetMarkerValue("FadeIn", DragHandle::FadeIn);
    const double fFadeOut = GetMarkerValue("StopFadeOut", DragHandle::FadeOut);
    const double fEnd = GetTimeExtent();

    const plColorGammaUB fadeInColor = Axis(1);
    const plColorGammaUB fadeOutColor = plAudioEngineUi::CategoryColor("Gameplay");
    const plColorGammaUB background = Background();

    const float x0 = TimeToX(0.0);
    const float fInX = TimeToX(fFadeIn);
    const float fEndX = TimeToX(fEnd);
    const float fOutX = TimeToX(plMath::Max(fEnd - fFadeOut, 0.0));

    m_Writer.Rect(plRectFloat(x0, y + 6.0f, plMath::Max(fInX - x0, 1.0f), s_fLane - 12.0f), plAudioEngineUi::Mix(background, fadeInColor, 0.25f), 2.0f);
    m_Writer.Rect(plRectFloat(fInX - 1.5f, y + 4.0f, 3.0f, s_fLane - 8.0f), fadeInColor);

    m_Writer.Rect(plRectFloat(fOutX, y + 6.0f, plMath::Max(fEndX - fOutX, 1.0f), s_fLane - 12.0f), plAudioEngineUi::Mix(background, fadeOutColor, 0.25f), 2.0f);
    m_Writer.Rect(plRectFloat(fOutX - 1.5f, y + 4.0f, 3.0f, s_fLane - 8.0f), fadeOutColor);

    // labels: shortened when they would collide, the stop fade label moves past the end line when there is no room
    plStringBuilder sIn, sOut;
    sIn.SetFormat("fade in {}s", plArgF(fFadeIn, 2));
    sOut.SetFormat("stop fade {}s", plArgF(fFadeOut, 2));

    float fInLeft = fInX + 6.0f;
    float fOutRight = fOutX - 6.0f;
    auto fits = [&]()
    { return fInLeft + MeasureCodeText(sIn, MinFontSize) < fOutRight - MeasureCodeText(sOut, MinFontSize); };

    if (!fits())
    {
      sIn.SetFormat("in {}s", plArgF(fFadeIn, 2));
      sOut.SetFormat("out {}s", plArgF(fFadeOut, 2));
    }

    const bool bOutAfterEnd = !fits();
    const float fInWidth = MeasureCodeText(sIn, MinFontSize) + 4.0f;
    const float fOutWidth = MeasureCodeText(sOut, MinFontSize) + 4.0f;

    // each label is a chip tinted like its fade
    auto chip = [&](float fLeft, float fWidth, plStringView sText, plColorGammaUB color)
    {
      const plRectFloat rect(fLeft - 4.0f, y + 6.0f, fWidth + 6.0f, s_fLane - 12.0f);
      m_Writer.Rect(rect, plAudioEngineUi::Mix(background, color, 0.35f), 3.0f);
      m_Writer.Text(plRectFloat(fLeft, y, fWidth, s_fLane), sText, Foreground(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    };

    chip(fInLeft, fInWidth, sIn, fadeInColor);
    chip(bOutAfterEnd ? fEndX + 6.0f : fOutRight - fOutWidth, fOutWidth, sOut, fadeOutColor);
  }

  // one chip per automation
  plHybridArray<const plDocumentObject*, 8> automations;
  GatherChildrenOfProperty(pProps, "Automations", automations);

  const float fChipY = y + s_fLane + 5.0f;
  if (automations.IsEmpty())
  {
    m_Writer.Text(plRectFloat(TimeToX(0.0), y + s_fLane, 500.0f, s_fLane), "no automation curves - add one in the deck's AUTOMATION column", Dim(), FontSize - 2);
  }
  else
  {
    float x = TimeToX(0.0);
    for (const plDocumentObject* pAutomation : automations)
    {
      const auto& accessor = pAutomation->GetTypeAccessor();

      plString sParam = accessor.GetValue("Parameter").ConvertTo<plString>();
      if (sParam.IsEmpty())
        sParam = "?";

      const plInt64 iTarget = accessor.GetValue("Target").ConvertTo<plInt64>();

      plStringBuilder sLabel;
      sLabel.SetFormat("{} by {}", iTarget == plAudioEventAutomationTarget::EventPitch ? "Pitch" : "Volume", sParam);

      const plRectFloat chip(x, fChipY, MeasureText(sLabel, FontSize - 1) + 16.0f, 18.0f);
      const bool bChipSelected = pAutomation->GetGuid() == selected;
      m_Writer.Rect(chip, plAudioEngineUi::Mix(Surface(), Raised(), 0.5f), 3.0f);
      m_Writer.RectOutline(chip, bChipSelected ? Accent() : Border(), 1.0f, 3.0f);
      m_Writer.Text(chip, sLabel, Foreground(), FontSize - 1, plUiTextHAlign::Center);

      Hit& hit = m_Hits.ExpandAndGetRef();
      hit.m_Kind = HitKind::Chip;
      hit.m_Guid = pAutomation->GetGuid();
      hit.m_Rect = plRectFloat(chip.x - 2.0f, chip.y - 4.0f, chip.width + 4.0f, chip.height + 8.0f);

      x += chip.width + 14.0f;
    }
  }

  m_Writer.PopClip();
}

//////////////////////////////////////////////////////////////////////////
// Input

const plEditorUiAudioEventTimeline::Hit* plEditorUiAudioEventTimeline::FindHit(const plVec2& vPos) const
{
  // the ruler and the tab row cover the scrolled rows
  if (vPos.y < GetCanvasTop() + s_fRuler)
    return nullptr;

  // toggles first: they lie inside the header
  for (HitKind kind : {HitKind::Mute, HitKind::Solo, HitKind::Clip, HitKind::Chip, HitKind::Header})
  {
    for (const Hit& hit : m_Hits)
    {
      if (hit.m_Kind == kind && hit.m_Rect.Contains(vPos) && (kind == HitKind::Header || kind == HitKind::Mute || kind == HitKind::Solo || vPos.x >= s_fHead))
        return &hit;
    }
  }

  return nullptr;
}

plEditorUiAudioEventTimeline::DragHandle plEditorUiAudioEventTimeline::PickHandle(const plVec2& vPos) const
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr || vPos.x < s_fHead - 6.0f || vPos.y < GetCanvasTop())
    return DragHandle::None;

  const auto& accessor = pProps->GetTypeAccessor();

  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherInstruments(instruments);

  const float fLaneY = GetRowTop(instruments.GetCount());
  const bool bInLogicRow = vPos.y >= fLaneY && vPos.y < fLaneY + s_fLane;

  // loop region handles: in the ruler and along the region's edges
  if (accessor.GetValue("LoopRegion").ConvertTo<bool>() && !bInLogicRow)
  {
    if (plMath::Abs(vPos.x - TimeToX(accessor.GetValue("LoopStart").ConvertTo<double>())) <= s_fHandleGrab)
      return DragHandle::LoopStart;
    if (plMath::Abs(vPos.x - TimeToX(accessor.GetValue("LoopEnd").ConvertTo<double>())) <= s_fHandleGrab)
      return DragHandle::LoopEnd;
  }

  // fade handles live in the logic row
  if (bInLogicRow)
  {
    if (plMath::Abs(vPos.x - TimeToX(accessor.GetValue("FadeIn").ConvertTo<double>())) <= s_fHandleGrab)
      return DragHandle::FadeIn;

    const double fEnd = GetTimeExtent();
    if (plMath::Abs(vPos.x - TimeToX(plMath::Max(fEnd - accessor.GetValue("StopFadeOut").ConvertTo<double>(), 0.0))) <= s_fHandleGrab)
      return DragHandle::FadeOut;
  }

  return DragHandle::None;
}

void plEditorUiAudioEventTimeline::ApplyHandleDrag(DragHandle handle, float x, bool bFinal)
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr || handle == DragHandle::None)
    return;

  const auto& accessor = pProps->GetTypeAccessor();
  const double fTime = plMath::RoundToMultiple(XToTime(x), 0.01);

  const char* szProperty = nullptr;
  float fValue = 0.0f;

  switch (handle)
  {
    case DragHandle::LoopStart:
      szProperty = "LoopStart";
      fValue = static_cast<float>(plMath::Min(fTime, accessor.GetValue("LoopEnd").ConvertTo<double>() - 0.05));
      break;

    case DragHandle::LoopEnd:
      szProperty = "LoopEnd";
      fValue = static_cast<float>(plMath::Max(fTime, accessor.GetValue("LoopStart").ConvertTo<double>() + 0.05));
      break;

    case DragHandle::FadeIn:
      szProperty = "FadeIn";
      fValue = static_cast<float>(plMath::Max(fTime, 0.0));
      break;

    case DragHandle::FadeOut:
      // the handle is the fade's start, so the stored duration is measured back from the end
      szProperty = "StopFadeOut";
      fValue = static_cast<float>(plMath::Clamp(GetTimeExtent() - fTime, 0.0, 60.0));
      break;

    default:
      return;
  }

  m_bDrawDirty = true;

  // while dragging only preview; one command is written on release
  if (!bFinal)
  {
    m_fHandlePreview = fValue;
    m_bHandlePreviewValid = true;
    return;
  }

  m_bHandlePreviewValid = false;

  if (plMath::Abs(accessor.GetValue(szProperty).ConvertTo<float>() - fValue) < 0.0001f)
    return;

  plObjectCommandAccessor commandAccessor(m_pDocument->GetCommandHistory());
  commandAccessor.StartTransaction("Edit Timeline Marker");
  commandAccessor.SetValueByName(pProps, szProperty, fValue).IgnoreResult();
  commandAccessor.FinishTransaction();
}

void plEditorUiAudioEventTimeline::ToggleMute(const plUuid& instrumentGuid)
{
  const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(instrumentGuid);
  if (pObject == nullptr)
    return;

  const bool bMuted = pObject->GetTypeAccessor().GetValue("Mute").ConvertTo<bool>();

  plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
  accessor.StartTransaction(bMuted ? "Unmute Instrument" : "Mute Instrument");
  accessor.SetValueByName(pObject, "Mute", !bMuted).IgnoreResult();
  accessor.FinishTransaction();
}

void plEditorUiAudioEventTimeline::ToggleSolo(const plUuid& instrumentGuid)
{
  if (m_SoloTracks.Contains(instrumentGuid))
    m_SoloTracks.RemoveAndCopy(instrumentGuid);
  else
    m_SoloTracks.PushBack(instrumentGuid);

  m_bDrawDirty = true;
}

void plEditorUiAudioEventTimeline::OnPointer(const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;
  using Button = plEditorUiPointerEvent::Button;

  // the tabs, the "+ Parameter" slot and the rename field handle themselves
  const bool bOnCanvas = e.m_sElement.IsEmpty() && e.m_vPosition.y >= GetCanvasTop();

  switch (e.m_Type)
  {
    case Type::Wheel:
    {
      if (!bOnCanvas && e.m_vPosition.y < GetCanvasTop())
        return;

      CommitRename();

      const plVec2 size = GetSurface()->GetLogicalSize();
      const plVec2 content = GetContentSize();
      const bool bVertical = !e.HasModifier(plEditorUiPointerEvent::Shift) && content.y > size.y - GetCanvasTop();

      if (bVertical)
        m_fScrollY -= e.m_fWheel * s_fWheelStep;
      else
        m_fScrollX -= e.m_fWheel * s_fWheelStep;

      ClampScroll();
      m_bDrawDirty = true;
      return;
    }

    case Type::Move:
    {
      if (!bOnCanvas)
      {
        SetCursor(plEditorPlatformCursor::Arrow);
        return;
      }

      if (PickHandle(e.m_vPosition) != DragHandle::None)
      {
        SetCursor(plEditorPlatformCursor::SizeWE);
        return;
      }

      const Hit* pHit = FindHit(e.m_vPosition);
      if (pHit != nullptr && pHit->m_Kind == HitKind::Clip)
        SetCursor(plEditorPlatformCursor::SizeAll);
      else
        SetCursor(plEditorPlatformCursor::Arrow);
      return;
    }

    case Type::Press:
    {
      if (e.m_Button != Button::Left || !bOnCanvas)
        return;

      CommitRename();

      // loop region and fade handles win over selection
      if (const DragHandle handle = PickHandle(e.m_vPosition); handle != DragHandle::None)
      {
        m_ActiveHandle = handle;
        ApplyHandleDrag(handle, e.m_vPosition.x, false);
        return;
      }

      const Hit* pHit = FindHit(e.m_vPosition);
      if (pHit == nullptr)
      {
        if (const plDocumentObject* pProps = GetPropertiesObject())
          m_pDocument->GetSelectionManager()->SetSelection(pProps);
        return;
      }

      switch (pHit->m_Kind)
      {
        case HitKind::Mute:
          ToggleMute(pHit->m_Guid);
          return;

        case HitKind::Solo:
          ToggleSolo(pHit->m_Guid);
          return;

        default:
          break;
      }

      const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(pHit->m_Guid);
      if (pObject == nullptr)
        return;

      m_pDocument->GetSelectionManager()->SetSelection(pObject);

      // instrument clips drag along the timeline to change their start delay
      if (pHit->m_Kind == HitKind::Clip)
      {
        m_bDraggingClip = true;
        m_DragClipGuid = pHit->m_Guid;
        m_fDragClipDelay = pObject->GetTypeAccessor().GetValue("StartDelay").ConvertTo<double>();
        m_fDragGrabOffset = XToTime(e.m_vPosition.x) - m_fDragClipDelay;
      }

      m_bDrawDirty = true;
      return;
    }

    case Type::DoubleClick:
    {
      if (e.m_Button != Button::Left || !bOnCanvas)
        return;

      const Hit* pHit = FindHit(e.m_vPosition);
      if (pHit != nullptr && pHit->m_Kind == HitKind::Header)
        BeginRename(pHit->m_Guid);
      return;
    }

    case Type::DragStart:
    case Type::DragMove:
    {
      if (m_ActiveHandle != DragHandle::None)
      {
        ApplyHandleDrag(m_ActiveHandle, e.m_vPosition.x, false);
      }
      else if (m_bDraggingClip)
      {
        // 10 ms snap; the timeline end and the stop fade follow the clip
        m_fDragClipDelay = plMath::Max(0.0, plMath::RoundToMultiple(XToTime(e.m_vPosition.x) - m_fDragGrabOffset, 0.01));
        m_bDrawDirty = true;
      }
      return;
    }

    case Type::Release:
    case Type::Drop:
    {
      if (m_ActiveHandle != DragHandle::None)
      {
        ApplyHandleDrag(m_ActiveHandle, e.m_vPosition.x, true);
        m_ActiveHandle = DragHandle::None;
        return;
      }

      if (m_bDraggingClip)
      {
        const plUuid guid = m_DragClipGuid;
        const double fDelay = m_fDragClipDelay;

        m_bDraggingClip = false;
        m_DragClipGuid = plUuid();
        m_bDrawDirty = true;

        if (const plDocumentObject* pInstrument = m_pDocument->GetObjectManager()->GetObject(guid))
        {
          if (plMath::Abs(pInstrument->GetTypeAccessor().GetValue("StartDelay").ConvertTo<double>() - fDelay) > 0.0001)
          {
            plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
            accessor.StartTransaction("Move Instrument");
            accessor.SetValueByName(pInstrument, "StartDelay", static_cast<float>(fDelay)).IgnoreResult();
            accessor.FinishTransaction();
          }
        }
      }
      return;
    }

    case Type::DragCancel:
    {
      m_ActiveHandle = DragHandle::None;
      m_bHandlePreviewValid = false;
      m_bDraggingClip = false;
      m_DragClipGuid = plUuid();
      m_bDrawDirty = true;
      return;
    }

    default:
      return;
  }
}

void plEditorUiAudioEventTimeline::OnKey(const plEditorUiKeyEvent& e)
{
  if (e.m_bHandled || !m_RenameGuid.IsValid())
    return;

  if (e.m_Key == plEditorUiKeyEvent::Key::Escape)
  {
    CancelRename();
    e.m_bHandled = true;
  }
}

void plEditorUiAudioEventTimeline::BeginRename(const plUuid& instrumentGuid)
{
  const plDocumentObject* pInstrument = m_pDocument->GetObjectManager()->GetObject(instrumentGuid);
  if (pInstrument == nullptr)
    return;

  m_RenameGuid = instrumentGuid;
  m_bRenameHadFocus = false;
  GetSurface()->GetBlackboard().SetEntryValue(s_sRenameText, pInstrument->GetTypeAccessor().GetValue("Name").ConvertTo<plString>());
  m_bRebuild = true;
}

void plEditorUiAudioEventTimeline::CommitRename()
{
  if (!m_RenameGuid.IsValid())
    return;

  const plUuid guid = m_RenameGuid;
  plStringBuilder sNewName = GetSurface()->GetBlackboard().GetEntryValue(s_sRenameText).ConvertTo<plString>();
  sNewName.Trim(" \t");

  m_RenameGuid = plUuid();
  m_bRebuild = true;

  const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(guid);
  if (pObject == nullptr)
    return;

  if (sNewName.IsEqual(pObject->GetTypeAccessor().GetValue("Name").ConvertTo<plString>()))
    return;

  plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
  accessor.StartTransaction("Rename Instrument");
  accessor.SetValueByName(pObject, "Name", sNewName.GetView()).IgnoreResult();
  accessor.FinishTransaction();
}

void plEditorUiAudioEventTimeline::CancelRename()
{
  m_RenameGuid = plUuid();
  m_bRebuild = true;
}
