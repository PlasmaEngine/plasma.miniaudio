#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <AudioSystem/Mixer/AudioMixerData.h>
#include <EditorFramework/EditorApp/EditorApp.moc.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAsset.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventTimelineView.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <GameUiPlugin/Components/UiCanvasComponent.h>
#include <MiniAudio/miniaudio.h>
#include <PlasmaAudioPlugin/Clips/AudioClipDesc.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

namespace
{
  constexpr float s_fTabRow = plEditorUiTabs::Height + 4.0f;
  constexpr float s_fRuler = 24.0f;
  constexpr float s_fRow = 66.0f;
  constexpr float s_fHead = 156.0f;
  constexpr float s_fLane = 28.0f;
  constexpr float s_fTimeInset = 10.0f;
  constexpr double s_fMinPixelsPerSecond = 10.0;
  constexpr double s_fMaxPixelsPerSecond = 8000.0;
  constexpr double s_fZoomStep = 1.18;
  constexpr float s_fHandleGrab = 5.0f;
  constexpr float s_fWheelStep = 40.0f;

  /// Buckets of the waveform overview decoded from an embedded sound file, and the longest file decoded for it.
  constexpr plUInt32 s_uiPeakBuckets = 512;
  constexpr double s_fMaxPeakSeconds = 120.0;

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
  m_PeakCache.Clear();
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

double plEditorUiAudioEventTimeline::GetLongestEntry(const plDocumentObject* pInstrument, const plDocumentObject** out_pEntry) const
{
  plHybridArray<const plDocumentObject*, 8> playlist;
  GatherChildrenOfProperty(pInstrument, "Playlist", playlist);

  double fLongest = 0.0;
  const plDocumentObject* pLongest = nullptr;
  for (const plDocumentObject* pEntry : playlist)
  {
    const double fDuration = GetEntryDuration(pEntry);
    if (fDuration > fLongest)
    {
      fLongest = fDuration;
      pLongest = pEntry;
    }
  }

  if (out_pEntry != nullptr)
    *out_pEntry = pLongest;

  return fLongest;
}

const plDynamicArray<float>& plEditorUiAudioEventTimeline::GetEntryPeaks(const plDocumentObject* pEntry) const
{
  const plString sClip = pEntry->GetTypeAccessor().GetValue("Clip").ConvertTo<plString>();
  const plString sFile = pEntry->GetTypeAccessor().GetValue("File").ConvertTo<plString>();
  const plString& sKey = sClip.IsEmpty() ? sFile : sClip;

  if (auto it = m_PeakCache.Find(sKey); it.IsValid())
    return it.Value();

  plDynamicArray<float>& peaks = m_PeakCache[sKey];

  if (!sClip.IsEmpty())
  {
    // the built clip's overview: (min, max) per bucket and channel, the loudest channel per bucket
    plAudioClipHeader header;
    if (plAudioClipAssetUtils::ReadBuiltHeader(sClip, header) && header.m_uiPeakBuckets > 0 && header.m_uiChannels > 0 &&
        header.m_Peaks.GetCount() >= header.m_uiPeakBuckets * header.m_uiChannels * 2)
    {
      peaks.SetCount(header.m_uiPeakBuckets);
      for (plUInt32 uiBucket = 0; uiBucket < header.m_uiPeakBuckets; ++uiBucket)
      {
        float fPeak = 0.0f;
        for (plUInt32 uiChannel = 0; uiChannel < header.m_uiChannels; ++uiChannel)
        {
          const plUInt32 uiIndex = (uiBucket * header.m_uiChannels + uiChannel) * 2;
          fPeak = plMath::Max(fPeak, plMath::Max(plMath::Abs(header.m_Peaks[uiIndex] / 127.0f), plMath::Abs(header.m_Peaks[uiIndex + 1] / 127.0f)));
        }
        peaks[uiBucket] = plMath::Min(fPeak, 1.0f);
      }

      return peaks;
    }
  }

  // an embedded sound file, or the source of a clip that was not built yet, is decoded once
  plStringBuilder sAbsPath = sFile;
  if (!sClip.IsEmpty())
  {
    plAudioClipAssetMetaInfo info;
    sAbsPath = plAudioClipAssetUtils::GetClipInfo(sClip, info) ? info.m_sSourceFile : plString();
  }

  if (sAbsPath.IsEmpty() || !plQtEditorApp::GetSingleton()->MakeDataDirectoryRelativePathAbsolute(sAbsPath))
    return peaks;

  ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
  ma_decoder decoder;
  if (ma_decoder_init_file(sAbsPath.GetData(), &config, &decoder) != MA_SUCCESS)
    return peaks;

  ma_uint64 uiFrames = 0;
  const plUInt32 uiChannels = decoder.outputChannels;
  if (ma_decoder_get_length_in_pcm_frames(&decoder, &uiFrames) == MA_SUCCESS && uiFrames > 0 && uiChannels > 0 &&
      static_cast<double>(uiFrames) / plMath::Max<ma_uint32>(decoder.outputSampleRate, 1u) <= s_fMaxPeakSeconds)
  {
    peaks.SetCount(s_uiPeakBuckets);

    plDynamicArray<float> chunk;
    chunk.SetCountUninitialized(4096 * uiChannels);

    ma_uint64 uiFrame = 0;
    while (uiFrame < uiFrames)
    {
      ma_uint64 uiRead = 0;
      if (ma_decoder_read_pcm_frames(&decoder, chunk.GetData(), 4096, &uiRead) != MA_SUCCESS || uiRead == 0)
        break;

      for (ma_uint64 i = 0; i < uiRead; ++i)
      {
        const plUInt32 uiBucket = static_cast<plUInt32>(plMath::Min<ma_uint64>((uiFrame + i) * s_uiPeakBuckets / uiFrames, s_uiPeakBuckets - 1));
        for (plUInt32 c = 0; c < uiChannels; ++c)
        {
          peaks[uiBucket] = plMath::Max(peaks[uiBucket], plMath::Min(plMath::Abs(chunk[static_cast<plUInt32>(i) * uiChannels + c]), 1.0f));
        }
      }

      uiFrame += uiRead;
    }
  }

  ma_decoder_uninit(&decoder);
  return peaks;
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
  return s_fHead + s_fTimeInset + static_cast<float>(fSeconds * m_fPixelsPerSecond) - m_fScrollX;
}

double plEditorUiAudioEventTimeline::XToTime(float x) const
{
  return plMath::Max(0.0, static_cast<double>(x + m_fScrollX - s_fHead - s_fTimeInset) / m_fPixelsPerSecond);
}

void plEditorUiAudioEventTimeline::FitView()
{
  if (!IsReady())
    return;

  const float fWidth = GetSurface()->GetLogicalSize().x - s_fHead - s_fTimeInset - 40.0f;
  m_fPixelsPerSecond = plMath::Clamp(static_cast<double>(plMath::Max(fWidth, 40.0f)) / GetTimeExtent(), s_fMinPixelsPerSecond, s_fMaxPixelsPerSecond);
  m_fScrollX = 0.0f;
  ClampScroll();
  m_bDrawDirty = true;
}

float plEditorUiAudioEventTimeline::GetAuditionValue(plStringView sParameter, float fDefault) const
{
  auto it = m_AuditionValues.Find(plString(sParameter));
  return it.IsValid() ? it.Value() : fDefault;
}

void plEditorUiAudioEventTimeline::SetPlayback(bool bPlaying, double fPosition, plArrayPtr<const plUInt8> sounding)
{
  if (!bPlaying && !m_bPlaying)
    return;

  m_bPlaying = bPlaying;
  m_fPlayhead = fPosition;
  m_Sounding = sounding;

  if (!bPlaying)
    m_Sounding.Clear();

  m_bDrawDirty = true;
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
  const float fWidth = s_fHead + s_fTimeInset + static_cast<float>(GetTimeExtent() * m_fPixelsPerSecond) + 90.0f;
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

  // selecting a parameter shows its tab and selecting the event the timeline; other selections (an instrument clicked
  // in a parameter tab) keep the tab
  plUInt32 uiCurrent = (pSelected == nullptr || pSelected == GetPropertiesObject()) ? 0 : m_uiCurrentTab;

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

  const plUuid selected = m_pDocument->GetSelectionManager()->GetCurrentObject() != nullptr ? m_pDocument->GetSelectionManager()->GetCurrentObject()->GetGuid() : plUuid();

  // a parameter tab: the x-axis is the parameter
  if (const plDocumentObject* pParameter = GetCurrentParameter())
  {
    const float fRulerBottom = canvas.y + s_fRuler;
    m_Writer.Rect(plRectFloat(0.0f, fRulerBottom, s_fHead, plMath::Max(size.y - fRulerBottom, 1.0f)), Surface());

    const bool bAnySolo = !m_SoloTracks.IsEmpty();
    for (plUInt32 i = 0; i < instruments.GetCount(); ++i)
      DrawParameterTrack(instruments[i], i, pParameter, bAnySolo, selected);

    DrawParameterRuler(canvas, pParameter);

    m_Writer.Publish(GetSurface()->GetBlackboard(), s_sCanvasEntry);
    return;
  }

  const plDocumentObject* pProps = GetPropertiesObject();
  const double fExtent = GetTimeExtent();
  const bool bLoopRegion = pProps != nullptr && pProps->GetTypeAccessor().GetValue("LoopRegion").ConvertTo<bool>();
  const float fRulerBottom = canvas.y + s_fRuler;
  const plRectFloat content(s_fHead, fRulerBottom, plMath::Max(size.x - s_fHead, 1.0f), plMath::Max(size.y - fRulerBottom, 1.0f));

  plString sOutputBus = pProps != nullptr ? pProps->GetTypeAccessor().GetValue("OutputBus").ConvertTo<plString>() : plString();
  if (sOutputBus.IsEmpty())
    sOutputBus = "Master";

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

  // the cursor auditions start from, and the playhead while the engine process plays
  m_Writer.PushClip(plRectFloat(s_fHead, canvas.y, plMath::Max(size.x - s_fHead, 1.0f), canvas.height));
  {
    const float fCursorX = plMath::Round(TimeToX(m_fCursor)) + 0.5f;
    m_Writer.Line(plVec2(fCursorX, canvas.y + s_fRuler), plVec2(fCursorX, canvas.y + canvas.height), Muted(), 1.0f, 2.0f, 3.0f);
    plDynamicArray<plVec2> cursorMark;
    cursorMark.PushBack(plVec2(fCursorX - 5.0f, canvas.y + s_fRuler - 8.0f));
    cursorMark.PushBack(plVec2(fCursorX + 5.0f, canvas.y + s_fRuler - 8.0f));
    cursorMark.PushBack(plVec2(fCursorX, canvas.y + s_fRuler));
    m_Writer.Triangles(cursorMark, Muted());

    if (m_bPlaying)
    {
      const float fPlayX = TimeToX(m_fPlayhead);
      m_Writer.Rect(plRectFloat(fPlayX - 0.75f, canvas.y, 1.5f, canvas.height), Foreground());

      plDynamicArray<plVec2> playMark;
      playMark.PushBack(plVec2(fPlayX - 5.0f, canvas.y));
      playMark.PushBack(plVec2(fPlayX + 6.0f, canvas.y));
      playMark.PushBack(plVec2(fPlayX + 0.5f, canvas.y + 7.0f));
      m_Writer.Triangles(playMark, Foreground());
    }
  }
  m_Writer.PopClip();

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sCanvasEntry);
}

//////////////////////////////////////////////////////////////////////////
// Parameter tabs

const plDocumentObject* plEditorUiAudioEventTimeline::GetCurrentParameter() const
{
  if (m_uiCurrentTab >= m_TabParameters.GetCount() || !m_TabParameters[m_uiCurrentTab].IsValid())
    return nullptr;

  return m_pDocument->GetObjectManager()->GetObject(m_TabParameters[m_uiCurrentTab]);
}

void plEditorUiAudioEventTimeline::GetParameterRange(const plDocumentObject* pParameter, float& out_fMin, float& out_fMax) const
{
  const float fA = pParameter->GetTypeAccessor().GetValue("Min").ConvertTo<float>();
  const float fB = pParameter->GetTypeAccessor().GetValue("Max").ConvertTo<float>();
  out_fMin = plMath::Min(fA, fB);
  out_fMax = plMath::Max(fA, fB);

  if (out_fMax - out_fMin < 0.0001f)
    out_fMax = out_fMin + 1.0f;
}

float plEditorUiAudioEventTimeline::ValueToX(const plDocumentObject* pParameter, float fValue) const
{
  float fMin, fMax;
  GetParameterRange(pParameter, fMin, fMax);

  const float fWidth = GetSurface()->GetLogicalSize().x - s_fHead - 2.0f * s_fTimeInset;
  return s_fHead + s_fTimeInset + (fValue - fMin) / (fMax - fMin) * plMath::Max(fWidth, 1.0f);
}

float plEditorUiAudioEventTimeline::XToValue(const plDocumentObject* pParameter, float x) const
{
  float fMin, fMax;
  GetParameterRange(pParameter, fMin, fMax);

  const float fWidth = plMath::Max(GetSurface()->GetLogicalSize().x - s_fHead - 2.0f * s_fTimeInset, 1.0f);
  return plMath::Clamp(fMin + (x - s_fHead - s_fTimeInset) / fWidth * (fMax - fMin), fMin, fMax);
}

void plEditorUiAudioEventTimeline::SetAuditionValue(const plDocumentObject* pParameter, float fValue)
{
  const plString sName = pParameter->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
  m_AuditionValues[sName] = fValue;
  m_bDrawDirty = true;

  if (m_OnAuditionValueChanged.IsValid())
    m_OnAuditionValueChanged(sName, fValue);
}

void plEditorUiAudioEventTimeline::DrawParameterRuler(const plRectFloat& canvas, const plDocumentObject* pParameter)
{
  using namespace plEditorUi;

  float fMin, fMax;
  GetParameterRange(pParameter, fMin, fMax);

  const plRectFloat ruler(0.0f, canvas.y, canvas.width, s_fRuler);
  m_Writer.Rect(ruler, Surface());
  m_Writer.Rect(plRectFloat(0.0f, ruler.y + ruler.height - 1.0f, ruler.width, 1.0f), Line());

  const plString sName = pParameter->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
  plStringBuilder sCorner;
  sCorner.SetFormat("Instruments by {}", sName);
  m_Writer.Text(plRectFloat(10.0f, ruler.y, s_fHead - 14.0f, ruler.height), sCorner, Muted(), FontSize - 2);

  // twelve steps across the range
  for (plUInt32 i = 0; i <= 12; ++i)
  {
    const float fValue = fMin + (fMax - fMin) * i / 12.0f;
    const float x = plMath::Round(ValueToX(pParameter, fValue)) + 0.5f;
    m_Writer.Rect(plRectFloat(x - 0.5f, ruler.y + ruler.height - 8.0f, 1.0f, 8.0f), Border());

    plStringBuilder sLabel;
    sLabel.SetFormat("{}", plArgF(fValue, (fMax - fMin) >= 12.0f ? 0 : 1));
    m_Writer.Text(plRectFloat(x + 3.0f, ruler.y + 2.0f, 50.0f, 12.0f), sLabel, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
  }

  // the scrub line: the value the audition plays with
  const float fValue = GetAuditionValue(sName, pParameter->GetTypeAccessor().GetValue("Default").ConvertTo<float>());
  const float fLineX = ValueToX(pParameter, fValue);

  m_Writer.PushClip(plRectFloat(s_fHead, canvas.y, plMath::Max(canvas.width - s_fHead, 1.0f), canvas.height));
  {
    m_Writer.Rect(plRectFloat(fLineX - 0.75f, canvas.y, 1.5f, canvas.height), Accent());

    plDynamicArray<plVec2> mark;
    mark.PushBack(plVec2(fLineX - 6.0f, canvas.y));
    mark.PushBack(plVec2(fLineX + 6.0f, canvas.y));
    mark.PushBack(plVec2(fLineX, canvas.y + 9.0f));
    m_Writer.Triangles(mark, Accent());

    plStringBuilder sValue;
    sValue.SetFormat("{} = {}", sName, plArgF(fValue, 2));
    const float fLabelWidth = MeasureCodeText(sValue, MinFontSize) + 10.0f;
    const float fLabelX = plMath::Min(fLineX + 8.0f, canvas.width - fLabelWidth - 4.0f);
    const plRectFloat badge(fLabelX, canvas.y + s_fRuler + 4.0f, fLabelWidth, 16.0f);
    m_Writer.Rect(badge, Accent(), 3.0f);
    m_Writer.Text(badge, sValue, Background(), MinFontSize, plUiTextHAlign::Center, plUiTextVAlign::Center, true);
  }
  m_Writer.PopClip();
}

void plEditorUiAudioEventTimeline::DrawParameterTrack(const plDocumentObject* pInstrument, plUInt32 uiIndex, const plDocumentObject* pParameter, bool bAnySolo, const plUuid& selected)
{
  using namespace plEditorUi;

  const plVec2 size = GetSurface()->GetLogicalSize();
  const float fRulerBottom = GetCanvasTop() + s_fRuler;
  const float y = GetRowTop(uiIndex);

  if (y + s_fRow < fRulerBottom || y > size.y)
    return;

  const auto& accessor = pInstrument->GetTypeAccessor();
  const plUuid guid = pInstrument->GetGuid();
  const bool bAudible = !accessor.GetValue("Mute").ConvertTo<bool>() && !(bAnySolo && !IsSolo(guid));
  const plColorGammaUB trackColor = TrackColor(uiIndex);
  const plColorGammaUB background = Background();

  const plString sParameter = pParameter->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
  const bool bOnParameter = !sParameter.IsEmpty() && accessor.GetValue("RangeParameter").ConvertTo<plString>() == sParameter;

  // the range, with a range edge being dragged shown where it is now
  float fRangeMin = accessor.GetValue("RangeMin").ConvertTo<float>();
  float fRangeMax = accessor.GetValue("RangeMax").ConvertTo<float>();
  if (m_RangeDragGuid == guid && m_ActiveHandle == DragHandle::RangeMin)
    fRangeMin = m_fRangeDragValue;
  if (m_RangeDragGuid == guid && m_ActiveHandle == DragHandle::RangeMax)
    fRangeMax = m_fRangeDragValue;

  const float fLow = plMath::Min(fRangeMin, fRangeMax);
  const float fHigh = plMath::Max(fRangeMin, fRangeMax);
  const float fFade = plMath::Max(accessor.GetValue("RangeFade").ConvertTo<float>(), 0.0f);

  // the volume of the range at the auditioned value
  const float fValue = GetAuditionValue(sParameter, pParameter->GetTypeAccessor().GetValue("Default").ConvertTo<float>());
  float fGain = 1.0f;
  if (bOnParameter && (fValue < fLow || fValue > fHigh))
  {
    const float fOutside = fValue < fLow ? fLow - fValue : fValue - fHigh;
    fGain = fFade > 0.0f ? plMath::Clamp(1.0f - fOutside / fFade, 0.0f, 1.0f) : 0.0f;
  }

  // at the far end of a fade the gain is a rounding error above 0
  const bool bLit = bAudible && fGain > 0.001f;

  plStringBuilder sSub;
  if (bOnParameter)
    sSub.SetFormat("{} - {} \xC2\xB7 fade {}", plArgF(fLow, 2), plArgF(fHigh, 2), plArgF(fFade, 2));
  else
    sSub = "plays at any value";

  const bool bSounding = m_bPlaying && uiIndex < m_Sounding.GetCount() && m_Sounding[uiIndex] != 0;
  DrawTrackHeader(pInstrument, uiIndex, sSub, bAnySolo, guid == selected, bSounding);

  const plRectFloat content(s_fHead, fRulerBottom, plMath::Max(size.x - s_fHead, 1.0f), plMath::Max(size.y - fRulerBottom, 1.0f));
  m_Writer.PushClip(content);
  {
    m_Writer.Rect(plRectFloat(0.0f, y + s_fRow - 1.0f, size.x, 1.0f), Line());

    if (bOnParameter)
    {
      // the range with its fade ramps: a trapezoid
      const float fTop = y + 10.0f;
      const float fBottom = y + s_fRow - 8.0f;
      const float x0 = ValueToX(pParameter, fLow - fFade);
      const float x1 = ValueToX(pParameter, fLow);
      const float x2 = ValueToX(pParameter, fHigh);
      const float x3 = ValueToX(pParameter, fHigh + fFade);

      plDynamicArray<plVec2> shape;
      shape.PushBack(plVec2(x0, fBottom));
      shape.PushBack(plVec2(x1, fTop));
      shape.PushBack(plVec2(x2, fTop));
      shape.PushBack(plVec2(x0, fBottom));
      shape.PushBack(plVec2(x2, fTop));
      shape.PushBack(plVec2(x3, fBottom));
      m_Writer.Triangles(shape, plAudioEngineUi::Mix(background, trackColor, bLit ? 0.32f : (bAudible ? 0.14f : 0.06f)));

      const plColorGammaUB edge = bLit ? trackColor : (bAudible ? plAudioEngineUi::Mix(background, trackColor, 0.6f) : Border());
      m_Writer.Line(plVec2(x0, fBottom), plVec2(x1, fTop), edge, bLit ? 1.5f : 1.0f);
      m_Writer.Line(plVec2(x1, fTop), plVec2(x2, fTop), edge, bLit ? 1.5f : 1.0f);
      m_Writer.Line(plVec2(x2, fTop), plVec2(x3, fBottom), edge, bLit ? 1.5f : 1.0f);

      // the range edges are handles
      m_Writer.Rect(plRectFloat(x1 - 2.0f, fTop - 4.0f, 4.0f, 8.0f), edge);
      m_Writer.Rect(plRectFloat(x2 - 2.0f, fTop - 4.0f, 4.0f, 8.0f), edge);

      plStringBuilder sVolume;
      if (bLit)
        sVolume.SetFormat("volume {} dB", plArgF(plAudioMixerUtils::LinearToDb(fGain), 1));
      else
        sVolume = "volume off";
      m_Writer.Text(plRectFloat(x1 + 6.0f, fTop + 4.0f, 160.0f, 16.0f), sVolume, bLit ? Foreground() : Dim(), FontSize - 2);
    }
    else
    {
      m_Writer.Rect(plRectFloat(content.x, y + 10.0f, content.width, s_fRow - 18.0f), plAudioEngineUi::Mix(background, Surface(), 0.5f));

      plStringBuilder sHint;
      sHint.SetFormat("not on {} - double-click to place it here", sParameter);
      m_Writer.Text(plRectFloat(content.x + 10.0f, y, content.width - 20.0f, s_fRow), sHint, Dim(), FontSize - 2);
    }
  }
  m_Writer.PopClip();
}

plEditorUiAudioEventTimeline::DragHandle plEditorUiAudioEventTimeline::PickRangeHandle(const plVec2& vPos, plUuid& out_instrumentGuid) const
{
  const plDocumentObject* pParameter = GetCurrentParameter();
  if (pParameter == nullptr || vPos.x < s_fHead)
    return DragHandle::None;

  const plString sParameter = pParameter->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();

  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherInstruments(instruments);

  for (plUInt32 i = 0; i < instruments.GetCount(); ++i)
  {
    const float y = GetRowTop(i);
    if (vPos.y < y || vPos.y >= y + s_fRow)
      continue;

    const auto& accessor = instruments[i]->GetTypeAccessor();
    if (sParameter.IsEmpty() || accessor.GetValue("RangeParameter").ConvertTo<plString>() != sParameter)
      return DragHandle::None;

    const float fA = accessor.GetValue("RangeMin").ConvertTo<float>();
    const float fB = accessor.GetValue("RangeMax").ConvertTo<float>();

    // the edges of the range's top, where the handles are drawn
    if (vPos.y > y + 22.0f)
      return DragHandle::None;

    out_instrumentGuid = instruments[i]->GetGuid();
    if (plMath::Abs(vPos.x - ValueToX(pParameter, plMath::Min(fA, fB))) <= s_fHandleGrab + 1.0f)
      return DragHandle::RangeMin;
    if (plMath::Abs(vPos.x - ValueToX(pParameter, plMath::Max(fA, fB))) <= s_fHandleGrab + 1.0f)
      return DragHandle::RangeMax;

    return DragHandle::None;
  }

  return DragHandle::None;
}

void plEditorUiAudioEventTimeline::ApplyRangeDrag(float x, bool bFinal)
{
  const plDocumentObject* pParameter = GetCurrentParameter();
  const plDocumentObject* pInstrument = m_pDocument->GetObjectManager()->GetObject(m_RangeDragGuid);
  if (pParameter == nullptr || pInstrument == nullptr)
    return;

  float fMin, fMax;
  GetParameterRange(pParameter, fMin, fMax);
  const float fStep = (fMax - fMin) / 200.0f;

  m_fRangeDragValue = plMath::RoundToMultiple(XToValue(pParameter, x), fStep);
  m_bDrawDirty = true;

  if (!bFinal)
    return;

  const char* szProperty = m_ActiveHandle == DragHandle::RangeMin ? "RangeMin" : "RangeMax";
  if (plMath::Abs(pInstrument->GetTypeAccessor().GetValue(szProperty).ConvertTo<float>() - m_fRangeDragValue) < 0.00001f)
    return;

  plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
  accessor.StartTransaction("Edit Parameter Range");
  accessor.SetValueByName(pInstrument, szProperty, m_fRangeDragValue).IgnoreResult();
  accessor.FinishTransaction();
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

void plEditorUiAudioEventTimeline::DrawTrackHeader(const plDocumentObject* pInstrument, plUInt32 uiIndex, plStringView sSub, bool bAnySolo, bool bSelected, bool bSounding)
{
  using namespace plEditorUi;

  const plVec2 size = GetSurface()->GetLogicalSize();
  const float fRulerBottom = GetCanvasTop() + s_fRuler;
  const float y = GetRowTop(uiIndex);

  const auto& accessor = pInstrument->GetTypeAccessor();
  const plUuid guid = pInstrument->GetGuid();
  const bool bMuted = accessor.GetValue("Mute").ConvertTo<bool>();
  const bool bSolo = IsSolo(guid);
  const bool bAudible = !bMuted && !(bAnySolo && !bSolo);

  // track colour edge (wider while it sounds), name, the sub line, M and S
  m_Writer.PushClip(plRectFloat(0.0f, fRulerBottom, s_fHead, plMath::Max(size.y - fRulerBottom, 1.0f)));
  {
    m_Writer.Rect(plRectFloat(0.0f, y, s_fHead, s_fRow), bSelected ? Selection() : Surface());
    m_Writer.Rect(plRectFloat(0.0f, y, bSounding ? 6.0f : 3.0f, s_fRow), TrackColor(uiIndex));

    plStringBuilder sName = accessor.GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      sName.SetFormat("Instrument {0}", uiIndex + 1);

    if (guid != m_RenameGuid)
      m_Writer.Text(plRectFloat(12.0f, y + 8.0f, s_fHead - 66.0f, 20.0f), sName, bAudible ? Foreground() : Muted(), FontSize);

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
  const plDocumentObject* pLongestEntry = nullptr;
  const double fRealLength = GetLongestEntry(pInstrument, &pLongestEntry);
  const bool bLengthKnown = fRealLength > 0.0;

  // while auditioning, the instruments that sound light up
  const bool bSounding = m_bPlaying && uiIndex < m_Sounding.GetCount() && m_Sounding[uiIndex] != 0;

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

  DrawTrackHeader(pInstrument, uiIndex, sSub, bAnySolo, bSelected, bSounding);

  // the clip block in the lane
  const plRectFloat content(s_fHead, fRulerBottom, plMath::Max(size.x - s_fHead, 1.0f), plMath::Max(size.y - fRulerBottom, 1.0f));
  m_Writer.PushClip(content);
  {
    m_Writer.Rect(plRectFloat(0.0f, y + s_fRow - 1.0f, size.x, 1.0f), Line());

    const double fDelay = GetInstrumentDelay(pInstrument);
    const double fDrawLength = bLengthKnown ? fRealLength : s_fPlaceholderLength;
    const float fClipX = TimeToX(fDelay);
    const float fClipWidth = plMath::Max(24.0f, static_cast<float>(fDrawLength * m_fPixelsPerSecond));
    const plRectFloat clip(fClipX, y + 6.0f, fClipWidth, s_fRow - 12.0f);
    const float fStrength = bAudible ? 1.0f : 0.35f;
    const plColorGammaUB background = Background();

    m_Writer.Rect(clip, plAudioEngineUi::Mix(background, trackColor, (bSounding ? 0.32f : 0.18f) * fStrength), 3.0f);

    if (bAudible)
      m_Writer.RectOutline(clip, bSounding ? trackColor : plAudioEngineUi::Mix(background, trackColor, 0.8f), bSounding ? 1.5f : 1.0f, 3.0f);
    else
      plAudioEngineUi::DashedRect(m_Writer, clip, Border());

    if (bSelected)
      m_Writer.RectOutline(clip, Accent(), 1.5f, 3.0f);

    m_Writer.PushClip(clip);
    {
      // the waveform overview of the longest entry, between the name row and the info row
      const plDynamicArray<float>* pPeaks = pLongestEntry != nullptr ? &GetEntryPeaks(pLongestEntry) : nullptr;
      if (pPeaks != nullptr && !pPeaks->IsEmpty())
      {
        plDynamicArray<plVec2> bars;
        const float fBarsWidth = static_cast<float>(fRealLength * m_fPixelsPerSecond);
        const float fFirst = plMath::Max(0.0f, plMath::Floor((content.x - clip.x) / 2.0f) * 2.0f);
        const float fLast = plMath::Min(fBarsWidth, content.x + content.width - clip.x);

        for (float x = fFirst; x < fLast; x += 2.0f)
        {
          const plUInt32 uiBucket = plMath::Min(static_cast<plUInt32>(x / fBarsWidth * pPeaks->GetCount()), pPeaks->GetCount() - 1);
          const float a = (*pPeaks)[uiBucket] * 10.0f;
          AddQuad(bars, clip.x + x, y + 31.0f - a, 1.0f, a * 2.0f + 1.0f);
        }

        if (!bars.IsEmpty())
          m_Writer.Triangles(bars, plAudioEngineUi::Mix(background, trackColor, fStrength));
      }
      else
      {
        // not built or not decodable: a flat line where the waveform goes
        m_Writer.Rect(plRectFloat(clip.x + 2.0f, y + 31.0f, clip.width - 4.0f, 1.0f), plAudioEngineUi::Mix(background, trackColor, 0.5f * fStrength));
      }

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

      // Ctrl+wheel zooms the time axis around the pointer
      if (e.HasModifier(plEditorUiPointerEvent::Control) && GetCurrentParameter() == nullptr)
      {
        const double fTimeAtPointer = XToTime(e.m_vPosition.x);
        m_fPixelsPerSecond = plMath::Clamp(m_fPixelsPerSecond * plMath::Pow(s_fZoomStep, static_cast<double>(e.m_fWheel)), s_fMinPixelsPerSecond, s_fMaxPixelsPerSecond);
        m_fScrollX = static_cast<float>(s_fHead + s_fTimeInset + fTimeAtPointer * m_fPixelsPerSecond - e.m_vPosition.x);
        ClampScroll();
        m_bDrawDirty = true;
        return;
      }

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

      plUuid rangeGuid;
      if (PickHandle(e.m_vPosition) != DragHandle::None || PickRangeHandle(e.m_vPosition, rangeGuid) != DragHandle::None)
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

      const plDocumentObject* pParameter = GetCurrentParameter();

      // parameter tab: the range edges, then scrubbing anywhere in the lanes
      if (pParameter != nullptr)
      {
        plUuid rangeGuid;
        if (const DragHandle handle = PickRangeHandle(e.m_vPosition, rangeGuid); handle != DragHandle::None)
        {
          m_ActiveHandle = handle;
          m_RangeDragGuid = rangeGuid;
          ApplyRangeDrag(e.m_vPosition.x, false);
          return;
        }

        if (e.m_vPosition.x >= s_fHead)
        {
          m_bScrubbing = true;
          SetAuditionValue(pParameter, XToValue(pParameter, e.m_vPosition.x));
          return;
        }
      }

      // loop region and fade handles win over selection
      if (pParameter == nullptr)
      {
        if (const DragHandle handle = PickHandle(e.m_vPosition); handle != DragHandle::None)
        {
          m_ActiveHandle = handle;
          ApplyHandleDrag(handle, e.m_vPosition.x, false);
          return;
        }

        // the ruler moves the cursor auditions start from
        if (e.m_vPosition.y < GetCanvasTop() + s_fRuler && e.m_vPosition.x >= s_fHead)
        {
          m_bScrubbing = true;
          m_fCursor = plMath::Min(XToTime(e.m_vPosition.x), GetTimeExtent());
          m_bDrawDirty = true;
          return;
        }
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
      {
        BeginRename(pHit->m_Guid);
        return;
      }

      // parameter tab: a row of an instrument that is not on the parameter is placed on it around the clicked value
      if (const plDocumentObject* pParameter = GetCurrentParameter(); pParameter != nullptr && e.m_vPosition.x >= s_fHead)
      {
        plHybridArray<const plDocumentObject*, 8> instruments;
        GatherInstruments(instruments);

        const plString sParameter = pParameter->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
        for (plUInt32 i = 0; i < instruments.GetCount(); ++i)
        {
          const float y = GetRowTop(i);
          if (e.m_vPosition.y < y || e.m_vPosition.y >= y + s_fRow || sParameter.IsEmpty() ||
              instruments[i]->GetTypeAccessor().GetValue("RangeParameter").ConvertTo<plString>() == sParameter)
            continue;

          float fMin, fMax;
          GetParameterRange(pParameter, fMin, fMax);
          const float fValue = XToValue(pParameter, e.m_vPosition.x);
          const float fSpan = (fMax - fMin) * 0.1f;

          plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
          accessor.StartTransaction("Place Instrument on Parameter");
          accessor.SetValueByName(instruments[i], "RangeParameter", sParameter.GetView()).IgnoreResult();
          accessor.SetValueByName(instruments[i], "RangeMin", plMath::Max(fValue - fSpan, fMin)).IgnoreResult();
          accessor.SetValueByName(instruments[i], "RangeMax", plMath::Min(fValue + fSpan, fMax)).IgnoreResult();
          accessor.SetValueByName(instruments[i], "RangeFade", fSpan).IgnoreResult();
          accessor.FinishTransaction();
          break;
        }
      }
      return;
    }

    case Type::DragStart:
    case Type::DragMove:
    {
      if (m_ActiveHandle == DragHandle::RangeMin || m_ActiveHandle == DragHandle::RangeMax)
      {
        ApplyRangeDrag(e.m_vPosition.x, false);
      }
      else if (m_bScrubbing)
      {
        if (const plDocumentObject* pParameter = GetCurrentParameter())
        {
          SetAuditionValue(pParameter, XToValue(pParameter, e.m_vPosition.x));
        }
        else
        {
          m_fCursor = plMath::Min(XToTime(e.m_vPosition.x), GetTimeExtent());
          m_bDrawDirty = true;
        }
      }
      else if (m_ActiveHandle != DragHandle::None)
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
      m_bScrubbing = false;

      if (m_ActiveHandle == DragHandle::RangeMin || m_ActiveHandle == DragHandle::RangeMax)
      {
        ApplyRangeDrag(e.m_vPosition.x, true);
        m_ActiveHandle = DragHandle::None;
        m_RangeDragGuid = plUuid();
        return;
      }

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
      m_RangeDragGuid = plUuid();
      m_bScrubbing = false;
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
  using Key = plEditorUiKeyEvent::Key;

  if (e.m_bHandled)
    return;

  if (m_RenameGuid.IsValid())
  {
    if (e.m_Key == Key::Escape)
    {
      CancelRename();
      e.m_bHandled = true;
    }
    return;
  }

  const plDocumentObject* pParameter = GetCurrentParameter();
  const bool bShift = (e.m_uiModifiers & plEditorUiPointerEvent::Shift) != 0;

  switch (e.m_Key)
  {
    case Key::Space:
      if (m_OnTogglePlay.IsValid())
        m_OnTogglePlay();
      e.m_bHandled = true;
      return;

    case Key::Home:
      m_fCursor = 0.0;
      m_fScrollX = 0.0f;
      m_bDrawDirty = true;
      e.m_bHandled = true;
      return;

    case Key::Left:
    case Key::Right:
    {
      const float fDirection = e.m_Key == Key::Left ? -1.0f : 1.0f;

      // a parameter tab scrubs its value in hundredths of the range, the time axis moves the cursor by 0.1 s
      if (pParameter != nullptr)
      {
        float fMin, fMax;
        GetParameterRange(pParameter, fMin, fMax);
        const plString sName = pParameter->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
        const float fValue = GetAuditionValue(sName, pParameter->GetTypeAccessor().GetValue("Default").ConvertTo<float>());
        SetAuditionValue(pParameter, plMath::Clamp(fValue + fDirection * (fMax - fMin) * (bShift ? 0.1f : 0.01f), fMin, fMax));
      }
      else
      {
        m_fCursor = plMath::Clamp(m_fCursor + fDirection * (bShift ? 1.0 : 0.1), 0.0, GetTimeExtent());
        m_bDrawDirty = true;
      }

      e.m_bHandled = true;
      return;
    }

    case Key::Character:
      if (e.m_uiCharacter == 'f' || e.m_uiCharacter == 'F')
      {
        FitView();
        e.m_bHandled = true;
      }
      return;

    default:
      return;
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
