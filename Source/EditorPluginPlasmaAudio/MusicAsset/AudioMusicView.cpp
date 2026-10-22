#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/DragDrop/DragDropInfo.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicAsset.h>
#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicEdit.h>
#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicView.h>
#include <EditorUi/DragDrop.h>
#include <EditorUi/Menu.h>
#include <EditorUi/SearchField.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GameUiPlugin/Components/UiCanvasComponent.h>

namespace
{
  const plHashedString s_sDrawingEntry = plMakeHashedString("Music.Drawing");
  const plHashedString s_sRenameText = plMakeHashedString("Mu.Rename.Text");
  constexpr const char* s_szRename = "Mu.Rename";
  constexpr const char* s_szRenameSubmit = "Mu.Rename.Submit";

  constexpr float s_fPad = 10.0f;
  constexpr float s_fToolRow = 38.0f;
  constexpr float s_fStatusRow = 24.0f;
  constexpr float s_fClipsWidth = 190.0f;
  constexpr float s_fInspectorWidth = 300.0f;
  constexpr float s_fSectionHead = 28.0f;
  constexpr float s_fLaneHeader = 120.0f;
  constexpr float s_fLaneHeight = 58.0f;
  constexpr float s_fLaneGap = 6.0f;
  constexpr float s_fBlockGap = 3.0f;
  constexpr float s_fBlockMin = 60.0f;
  constexpr float s_fDropMin = 120.0f;
  constexpr double s_fMaxPixelsPerSecond = 34.0;
  constexpr float s_fMatrixHeader = 22.0f;
  constexpr float s_fMatrixRow = 40.0f;
  constexpr float s_fCellGap = 3.0f;
  constexpr float s_fClipRow = 26.0f;
  constexpr float s_fClipHint = 24.0f;
  constexpr plUInt32 s_uiClipSlots = 80;
  constexpr float s_fRow = 28.0f;
  constexpr float s_fLabel = 92.0f;
  constexpr float s_fTextLine = 15.0f;
  constexpr float s_fMessagesHeight = 170.0f;
  constexpr plUInt32 s_uiMaxMessages = 40;
  constexpr double s_fToastSeconds = 2.6;

  /// Stems whose clip differs from the segment's length by more than this do not fit it.
  constexpr double s_fFitTolerance = 0.02;

  /// The volume and fade sliders' ranges.
  constexpr float s_fVolumeMin = -30.0f;
  constexpr float s_fVolumeMax = 6.0f;
  constexpr float s_fFadeMax = 8.0f;

  constexpr const char* s_szQuantize[] = {"immediately", "next beat", "next bar", "end of segment", "exit cue"};
  constexpr plUInt32 s_Meters[] = {3, 4, 6, 7};
  constexpr const char* s_szMeters[] = {"3/4", "4/4", "6/8", "7/8"};

  plString IndexedName(const char* szPrefix, plUInt32 uiIndex)
  {
    plStringBuilder s;
    s.SetFormat("{}{}", szPrefix, uiIndex);
    return s;
  }

  /// The number after sPrefix, or plInvalidIndex.
  plUInt32 IndexAfter(plStringView sId, plStringView sPrefix)
  {
    if (!sId.StartsWith(sPrefix))
      return plInvalidIndex;

    plStringView sRest = sId;
    sRest.Shrink(sPrefix.GetElementCount(), 0);

    plUInt32 uiIndex = 0;
    if (plConversionUtils::StringToUInt(sRest, uiIndex).Failed())
      return plInvalidIndex;

    return uiIndex;
  }

  plString TextBinding(plStringView sField)
  {
    return plStringBuilder(sField, ".Text");
  }

  plString SubmitEvent(plStringView sField)
  {
    return plStringBuilder(sField, ".Submit");
  }

  /// The words of sText in lines no wider than fWidth (code face).
  plString WrapText(plStringView sText, float fWidth, plUInt32 uiFontSize, plUInt32& out_uiLines)
  {
    plHybridArray<plStringView, 32> words;
    sText.Split(false, words, " ");

    plStringBuilder sResult, sLine;
    out_uiLines = 0;

    for (plStringView sWord : words)
    {
      plStringBuilder sCandidate = sLine;
      if (!sCandidate.IsEmpty())
        sCandidate.Append(" ");
      sCandidate.Append(sWord);

      if (!sLine.IsEmpty() && plEditorUi::MeasureCodeText(sCandidate, uiFontSize) > fWidth)
      {
        sResult.Append(sLine, "\n");
        ++out_uiLines;
        sLine = sWord;
      }
      else
      {
        sLine = sCandidate;
      }
    }

    if (!sLine.IsEmpty())
    {
      sResult.Append(sLine);
      ++out_uiLines;
    }

    return sResult;
  }

  plUInt32 FindSegment(const plAudioMusicAssetProperties& props, plStringView sName)
  {
    for (plUInt32 i = 0; i < props.m_Segments.GetCount(); ++i)
    {
      if (props.m_Segments[i].m_sName.IsEqual_NoCase(sName))
        return i;
    }
    return plInvalidIndex;
  }

  double SegmentSeconds(const plAudioMusicSegment& segment)
  {
    return 60.0 / plMath::Clamp(static_cast<double>(segment.m_fTempo), 20.0, 400.0) * plMath::Max(segment.m_uiBeatsPerBar, 1u) * plMath::Max(segment.m_uiBars, 1u);
  }

  /// Whether the segment is in no state: it only plays between states.
  bool IsLoose(const plAudioMusicAssetProperties& props, const plAudioMusicSegment& segment)
  {
    for (const plAudioMusicState& state : props.m_States)
    {
      for (const plString& sName : state.m_Segments)
      {
        if (sName.IsEqual_NoCase(segment.m_sName))
          return false;
      }
    }
    return true;
  }

  /// "4/4", or "5/4" for a meter the segmented choice does not offer.
  plString MeterName(plUInt32 uiBeatsPerBar)
  {
    for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_Meters); ++i)
    {
      if (s_Meters[i] == uiBeatsPerBar)
        return s_szMeters[i];
    }

    plStringBuilder s;
    s.SetFormat("{}/4", uiBeatsPerBar);
    return s;
  }

  plColorGammaUB AudioWash(float fAmount)
  {
    return plAudioEngineUi::Mix(plEditorUi::Background(), plAudioEngineUi::AudioColor(), fAmount);
  }

  /// The Audio Clip a drag from outside carries: an asset GUID from the asset browser, else a dropped clip document.
  plString DraggedClip(const plEditorUiDragPayload& payload)
  {
    plString sReference;
    plDynamicArray<plString> guids;
    plDragDropFormats::GetStrings(payload, plDragDropFormats::s_szAssetGuid, guids);
    if (!guids.IsEmpty())
      sReference = guids[0];

    if (sReference.IsEmpty() && !payload.m_Files.IsEmpty())
      sReference = payload.m_Files[0];

    if (sReference.IsEmpty())
      return {};

    const auto pAsset = plAssetCurator::GetSingleton()->FindSubAsset(sReference);
    if (!pAsset.isValid() || pAsset->m_Data.m_sSubAssetsDocumentTypeName.GetView() != "Audio Clip")
      return {};

    plStringBuilder sGuid;
    plConversionUtils::ToString(pAsset->m_Data.m_Guid, sGuid);
    return sGuid;
  }

  /// Toolbar geometry, shared by the hit buttons and the drawing.
  struct ToolbarLayout
  {
    plRectFloat m_Play;
    plRectFloat m_Stop;
    float m_fStateLabel = 0.0f;
    plHybridArray<plRectFloat, 8> m_States;
    float m_fStingerLabel = 0.0f;
    plHybridArray<plRectFloat, 8> m_Stingers;
    plRectFloat m_AddState;
    plRectFloat m_AddSegment;
    plRectFloat m_AddStinger;
    float m_fBeats = 0.0f;
  };

  ToolbarLayout LayoutToolbar(const plAudioMusicAssetProperties& props, float fWidth)
  {
    using namespace plEditorUi;

    ToolbarLayout layout;
    const float y = (s_fToolRow - ControlHeight) * 0.5f;
    float x = s_fPad;

    layout.m_Play = plRectFloat(x, y, plAudioEngineUi::TextButtonWidth("Play", true), ControlHeight);
    x += layout.m_Play.width + 4.0f;
    layout.m_Stop = plRectFloat(x, y, plAudioEngineUi::TextButtonWidth("Stop", true), ControlHeight);
    x += layout.m_Stop.width + 16.0f;

    layout.m_fStateLabel = x;
    x += MeasureText("State", FontSize - 1) + 8.0f;
    for (const plAudioMusicState& state : props.m_States)
    {
      const float fButton = plAudioEngineUi::TextButtonWidth(state.m_sName, false);
      layout.m_States.PushBack(plRectFloat(x, y, fButton, ControlHeight));
      x += fButton + 4.0f;
    }

    x += 12.0f;
    layout.m_fStingerLabel = x;
    x += MeasureText("Stinger", FontSize - 1) + 8.0f;
    for (const plAudioMusicStinger& stinger : props.m_Stingers)
    {
      const float fButton = plAudioEngineUi::TextButtonWidth(stinger.m_sName, false);
      layout.m_Stingers.PushBack(plRectFloat(x, y, fButton, ControlHeight));
      x += fButton + 4.0f;
    }

    // from the right: the position, the beat lights, the add buttons
    float fRight = fWidth - s_fPad - 92.0f - 12.0f - 4.0f * 14.0f;
    layout.m_fBeats = fRight;
    fRight -= 12.0f;

    const float fStinger = plAudioEngineUi::TextButtonWidth("+ Stinger", false);
    const float fSegment = plAudioEngineUi::TextButtonWidth("+ Segment", false);
    const float fState = plAudioEngineUi::TextButtonWidth("+ State", false);
    fRight = plMath::Max(fRight, x + fState + fSegment + fStinger + 8.0f);

    layout.m_AddStinger = plRectFloat(fRight - fStinger, y, fStinger, ControlHeight);
    layout.m_AddSegment = plRectFloat(layout.m_AddStinger.x - 4.0f - fSegment, y, fSegment, ControlHeight);
    layout.m_AddState = plRectFloat(layout.m_AddSegment.x - 4.0f - fState, y, fState, ControlHeight);
    return layout;
  }
} // namespace

plEditorUiAudioMusicView::plEditorUiAudioMusicView(plAudioMusicAssetDocument* pDocument)
  : plEditorUiContent("AudioMusic")
  , m_pDocument(pDocument)
{
  m_fClockOrigin = plTime::Now().GetSeconds();

  if (IsReady())
  {
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioMusicView::OnPointer, this));
    m_KeySubscription = GetSurface()->m_KeyEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioMusicView::OnKey, this));
    m_DropSubscription = GetSurface()->m_DropEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioMusicView::OnDrop, this));

    m_pClipSearch = PL_DEFAULT_NEW(plEditorUiSearchField, *GetSurface(), "Mu.ClipSearch");
    m_SearchSubscription = m_pClipSearch->m_Events.AddEventHandler(plMakeDelegate(&plEditorUiAudioMusicView::OnSearch, this));
  }

  GatherClips();
}

plEditorUiAudioMusicView::~plEditorUiAudioMusicView()
{
  if (IsReady())
  {
    m_pClipSearch->m_Events.RemoveEventHandler(m_SearchSubscription);
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
    GetSurface()->m_KeyEvents.RemoveEventHandler(m_KeySubscription);
    GetSurface()->m_DropEvents.RemoveEventHandler(m_DropSubscription);
  }
}

//////////////////////////////////////////////////////////////////////////
// Document

double plEditorUiAudioMusicView::Now() const
{
  return plTime::Now().GetSeconds() - m_fClockOrigin;
}

plUInt32 plEditorUiAudioMusicView::GetLooseLane() const
{
  return m_pDocument->GetProperties()->m_States.GetCount();
}

double plEditorUiAudioMusicView::GetSegmentSeconds(plUInt32 uiSegment) const
{
  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  return uiSegment < pProp->m_Segments.GetCount() ? SegmentSeconds(pProp->m_Segments[uiSegment]) : 2.0;
}

float plEditorUiAudioMusicView::GetStateTempo(plUInt32 uiState) const
{
  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  if (uiState < pProp->m_States.GetCount())
  {
    for (const plString& sName : pProp->m_States[uiState].m_Segments)
    {
      const plUInt32 uiSegment = FindSegment(*pProp, sName);
      if (uiSegment != plInvalidIndex)
        return pProp->m_Segments[uiSegment].m_fTempo;
    }
  }

  return 120.0f;
}

plUInt32 plEditorUiAudioMusicView::GetInitialState() const
{
  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  for (plUInt32 i = 0; i < pProp->m_States.GetCount(); ++i)
  {
    if (pProp->m_States[i].m_sName.IsEqual_NoCase(pProp->m_sInitialState))
      return i;
  }
  return 0;
}

plUInt32 plEditorUiAudioMusicView::FindStinger(plStringView sName) const
{
  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  for (plUInt32 i = 0; i < pProp->m_Stingers.GetCount(); ++i)
  {
    if (pProp->m_Stingers[i].m_sName.IsEqual_NoCase(sName))
      return i;
  }
  return plInvalidIndex;
}

void plEditorUiAudioMusicView::GatherClips()
{
  m_Clips.Clear();
  m_ClipsGathered = plTime::Now();

  if (plAssetCurator::GetSingleton() == nullptr)
    return;

  {
    const auto subAssets = plAssetCurator::GetSingleton()->GetKnownSubAssets();
    for (auto it = subAssets->GetIterator(); it.IsValid(); ++it)
    {
      const plSubAsset& subAsset = it.Value();
      if (subAsset.m_Data.m_sSubAssetsDocumentTypeName.GetView() != "Audio Clip")
        continue;

      Clip& clip = m_Clips.ExpandAndGetRef();
      plStringBuilder sGuid;
      clip.m_sGuid = plConversionUtils::ToString(subAsset.m_Data.m_Guid, sGuid);
      clip.m_sName = subAsset.GetName();

      if (subAsset.m_pAssetInfo != nullptr && subAsset.m_pAssetInfo->m_Info != nullptr)
      {
        if (const plAudioClipAssetMetaInfo* pMeta = subAsset.m_pAssetInfo->m_Info->GetMetaInfo<plAudioClipAssetMetaInfo>())
          clip.m_fSeconds = pMeta->m_fDuration;
      }
    }
  }

  m_Clips.Sort([](const Clip& a, const Clip& b)
    { return a.m_sName.Compare_NoCase(b.m_sName) < 0; });
}

void plEditorUiAudioMusicView::GetVisibleClips(plDynamicArray<const Clip*>& out_clips) const
{
  out_clips.Clear();
  for (const Clip& clip : m_Clips)
  {
    if (m_sClipFilter.IsEmpty() || clip.m_sName.FindSubString_NoCase(m_sClipFilter) != nullptr)
      out_clips.PushBack(&clip);
  }
}

void plEditorUiAudioMusicView::Refresh()
{
  ValidateSelection();
  m_bRebuild = true;
  RequestRedraw();
}

void plEditorUiAudioMusicView::SetBuildError(plStringView sError)
{
  m_sBuildError = sError;
  RequestRedraw();
}

void plEditorUiAudioMusicView::ValidateSelection()
{
  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  bool bValid = true;

  switch (m_Selection)
  {
    case SelectionKind::Segment:
      bValid = m_uiSelected < pProp->m_Segments.GetCount();
      break;
    case SelectionKind::State:
      bValid = m_uiSelected < pProp->m_States.GetCount();
      break;
    case SelectionKind::Stinger:
      bValid = m_uiSelected < pProp->m_Stingers.GetCount();
      break;
    case SelectionKind::Transition:
      bValid = m_uiSelectedTo < pProp->m_States.GetCount() && (m_uiSelected == plInvalidIndex || m_uiSelected < pProp->m_States.GetCount());
      break;
    default:
      break;
  }

  if (!bValid)
    m_Selection = SelectionKind::None;

  if (m_uiRenameState != plInvalidIndex && m_uiRenameState >= pProp->m_States.GetCount())
    m_uiRenameState = plInvalidIndex;
}

void plEditorUiAudioMusicView::Select(SelectionKind kind, plUInt32 uiIndex, plUInt32 uiTo)
{
  const bool bSame = m_Selection == kind && m_uiSelected == uiIndex && m_uiSelectedTo == uiTo;

  m_Selection = kind;
  m_uiSelected = uiIndex;
  m_uiSelectedTo = uiTo;

  if (!bSame)
    m_fInspectorScroll = 0.0f;

  m_bRebuild = true;
  RequestRedraw();

  if (!m_OnSelect.IsValid())
    return;

  switch (kind)
  {
    case SelectionKind::Segment:
      m_OnSelect(Selection::Segment, uiIndex);
      break;
    case SelectionKind::State:
      m_OnSelect(Selection::State, uiIndex);
      break;
    case SelectionKind::Stinger:
      m_OnSelect(Selection::Stinger, uiIndex);
      break;
    case SelectionKind::Transition:
      m_OnSelect(Selection::Transition, plAudioMusicEdit::FindTransition(m_pDocument, uiIndex, uiTo));
      break;
    default:
      m_OnSelect(Selection::Music, 0);
      break;
  }
}

void plEditorUiAudioMusicView::ShowToast(plStringView sText)
{
  m_sToast = sText;
  m_fToastTime = Now();
  RequestRedraw();
}

void plEditorUiAudioMusicView::GetIssues(plStringBuilder& out_sFirst, plUInt32& out_uiCount) const
{
  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  out_sFirst.Clear();
  out_uiCount = 0;

  // the build's own problem first, then what the editor sees
  if (!m_sBuildError.IsEmpty())
  {
    out_sFirst = m_sBuildError;
    ++out_uiCount;
  }

  for (const plAudioMusicState& state : pProp->m_States)
  {
    if (state.m_Segments.IsEmpty() && out_uiCount++ == 0)
      out_sFirst.SetFormat("{} has no segments", state.m_sName);
  }

  for (const plAudioMusicSegment& segment : pProp->m_Segments)
  {
    const double fSeconds = SegmentSeconds(segment);
    for (const plAudioMusicStem& stem : segment.m_Stems)
    {
      if (plMath::Abs(plAudioMusicEdit::GetClipSeconds(stem.m_sClip) - fSeconds) > s_fFitTolerance)
      {
        if (out_uiCount++ == 0)
          out_sFirst.SetFormat("{}: a stem does not fit its bars", segment.m_sName);
        break;
      }
    }
  }
}

//////////////////////////////////////////////////////////////////////////
// Layout

void plEditorUiAudioMusicView::ComputeLayout(Layout& out_layout) const
{
  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  Layout& L = out_layout;

  const plVec2 size = GetSurface()->GetLogicalSize();
  const float fTop = s_fToolRow;
  const float fBottom = plMath::Max(size.y - s_fStatusRow, fTop + 40.0f);
  const float fInspectorX = plMath::Max(size.x - s_fInspectorWidth, s_fClipsWidth + 320.0f);

  L.m_Clips = plRectFloat(0.0f, fTop, s_fClipsWidth, fBottom - fTop);
  L.m_Center = plRectFloat(s_fClipsWidth, fTop, fInspectorX - s_fClipsWidth, fBottom - fTop);
  L.m_Inspector = plRectFloat(fInspectorX, fTop, plMath::Max(size.x - fInspectorX, 0.0f), fBottom - fTop);
  L.m_Messages = plRectFloat(0, 0, 0, 0);

  // the messages to the game under the inspector while the music plays or has played
  if (m_bAuditioning || !m_Messages.IsEmpty())
  {
    const float fHeight = plMath::Min(s_fMessagesHeight, L.m_Inspector.height * 0.5f);
    L.m_Messages = plRectFloat(L.m_Inspector.x, fBottom - fHeight, L.m_Inspector.width, fHeight);
    L.m_Inspector.height -= fHeight;
  }

  const float fListTop = fTop + s_fSectionHead + plEditorUi::ControlHeight + 6.0f;
  L.m_ClipList = plRectFloat(0.0f, fListTop, s_fClipsWidth, plMath::Max(fBottom - s_fClipHint - fListTop, 0.0f));

  // the lanes: one per state and the loose lane; the scale fits the longest one beside its drop space
  const plUInt32 uiStates = pProp->m_States.GetCount();
  const float x0 = L.m_Center.x + 8.0f;
  const float fTrackX = x0 + s_fLaneHeader + 6.0f;
  const float fTrackWidth = plMath::Max(L.m_Center.x + L.m_Center.width - 8.0f - fTrackX, 80.0f);

  plHybridArray<plHybridArray<plUInt32, 8>, 8> laneSegments;
  laneSegments.SetCount(uiStates + 1);
  for (plUInt32 uiState = 0; uiState < uiStates; ++uiState)
  {
    for (const plString& sName : pProp->m_States[uiState].m_Segments)
    {
      const plUInt32 uiSegment = FindSegment(*pProp, sName);
      if (uiSegment != plInvalidIndex)
        laneSegments[uiState].PushBack(uiSegment);
    }
  }

  for (plUInt32 i = 0; i < pProp->m_Segments.GetCount(); ++i)
  {
    if (IsLoose(*pProp, pProp->m_Segments[i]))
      laneSegments[uiStates].PushBack(i);
  }

  L.m_fPixelsPerSecond = s_fMaxPixelsPerSecond;
  for (const auto& segments : laneSegments)
  {
    double fSeconds = 0.0;
    for (plUInt32 uiSegment : segments)
      fSeconds += GetSegmentSeconds(uiSegment);

    const double fSpace = fTrackWidth - 6.0f - s_fDropMin - segments.GetCount() * s_fBlockGap;
    if (fSeconds > 0.0)
      L.m_fPixelsPerSecond = plMath::Min(L.m_fPixelsPerSecond, fSpace / fSeconds);
  }
  L.m_fPixelsPerSecond = plMath::Max(L.m_fPixelsPerSecond, 4.0);

  const float fUnscrolledTop = L.m_Center.y;
  float y = L.m_Center.y + s_fSectionHead - m_fCenterScroll;

  for (plUInt32 uiLane = 0; uiLane <= uiStates; ++uiLane)
  {
    Lane& lane = L.m_Lanes.ExpandAndGetRef();
    lane.m_Header = plRectFloat(x0, y, s_fLaneHeader, s_fLaneHeight);
    lane.m_Track = plRectFloat(fTrackX, y, fTrackWidth, s_fLaneHeight);

    float x = fTrackX + 3.0f;
    for (plUInt32 uiPosition = 0; uiPosition < laneSegments[uiLane].GetCount(); ++uiPosition)
    {
      const plUInt32 uiSegment = laneSegments[uiLane][uiPosition];
      const float fWidth = plMath::Max(s_fBlockMin, static_cast<float>(GetSegmentSeconds(uiSegment) * L.m_fPixelsPerSecond));

      Block& block = L.m_Blocks.ExpandAndGetRef();
      block.m_uiLane = uiLane;
      block.m_uiPosition = uiPosition;
      block.m_uiSegment = uiSegment;
      block.m_Rect = plRectFloat(x, y + 3.0f, fWidth, s_fLaneHeight - 6.0f);
      x += fWidth + s_fBlockGap;
    }

    y += s_fLaneHeight + s_fLaneGap;
  }

  // the transitions matrix: a row per state and one for "any state", a column per state
  y += 8.0f;
  L.m_fMatrixTop = y;

  float fLabel = plEditorUi::MeasureCodeText("ANY STATE", plEditorUi::MinFontSize);
  for (const plAudioMusicState& state : pProp->m_States)
  {
    plStringBuilder sUpper = state.m_sName;
    sUpper.ToUpper();
    fLabel = plMath::Max(fLabel, plEditorUi::MeasureCodeText(sUpper, plEditorUi::MinFontSize));
  }
  L.m_fRowLabelWidth = plMath::Clamp(fLabel + 16.0f, 80.0f, 160.0f);

  const float fColumns = L.m_Center.width - 16.0f - L.m_fRowLabelWidth;
  L.m_fColumnWidth = uiStates > 0 ? plMath::Clamp(fColumns / uiStates - s_fCellGap, 90.0f, 170.0f) : 0.0f;

  const float fRowsTop = y + s_fSectionHead + s_fMatrixHeader;
  for (plUInt32 uiRow = 0; uiRow <= uiStates; ++uiRow)
  {
    for (plUInt32 uiColumn = 0; uiColumn < uiStates; ++uiColumn)
    {
      Cell& cell = L.m_Cells.ExpandAndGetRef();
      cell.m_uiFrom = uiRow == 0 ? plInvalidIndex : uiRow - 1;
      cell.m_uiTo = uiColumn;
      cell.m_Rect = plRectFloat(x0 + L.m_fRowLabelWidth + uiColumn * (L.m_fColumnWidth + s_fCellGap), fRowsTop + uiRow * (s_fMatrixRow + s_fCellGap), L.m_fColumnWidth, s_fMatrixRow);
    }
  }

  const float fEnd = fRowsTop + (uiStates + 1) * (s_fMatrixRow + s_fCellGap) + 10.0f;
  L.m_fCenterHeight = fEnd + m_fCenterScroll - fUnscrolledTop;
}

void plEditorUiAudioMusicView::ComputeInspector(const Layout& layout, plDynamicArray<Item>& out_items, float& out_fHeight) const
{
  using namespace plEditorUi;

  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  out_items.Clear();

  const float x = layout.m_Inspector.x + 12.0f;
  const float fRight = layout.m_Inspector.x + layout.m_Inspector.width - 12.0f;
  const float fControlX = x + s_fLabel + 8.0f;
  const float fControlWidth = plMath::Max(fRight - fControlX, 40.0f);
  const float fTop = layout.m_Inspector.y + 4.0f - m_fInspectorScroll;
  float y = fTop;

  auto add = [&](ItemType type, const plRectFloat& rect, plStringView sId = {}, plStringView sText = {}, plStringView sSub = {}) -> Item&
  {
    Item& item = out_items.ExpandAndGetRef();
    item.m_Type = type;
    item.m_Rect = rect;
    item.m_sId = sId;
    item.m_sText = sText;
    item.m_sSub = sSub;
    return item;
  };

  auto header = [&](plStringView sText, plStringView sSub = {})
  {
    add(ItemType::Header, plRectFloat(x - 2.0f, y + 4.0f, fRight - x, 20.0f), {}, sText, sSub);
    y += 28.0f;
  };

  auto label = [&](plStringView sText, float fLeft = 0.0f, float fWidth = s_fLabel)
  {
    add(ItemType::Label, plRectFloat(x + fLeft, y, fWidth, ControlHeight), {}, sText);
  };

  auto text = [&](plStringView sText, Tone tone = ToneNormal, float fLeft = 0.0f)
  {
    plUInt32 uiLines = 0;
    const plString sWrapped = WrapText(sText, fRight - x - fLeft, MinFontSize, uiLines);
    add(ItemType::Text, plRectFloat(x + fLeft, y + 2.0f, fRight - x - fLeft, uiLines * s_fTextLine), {}, sWrapped).m_Tone = tone;
    y += uiLines * s_fTextLine + 8.0f;
  };

  auto danger = [&](plStringView sId, plStringView sText)
  {
    y += 4.0f;
    add(ItemType::Danger, plRectFloat(x, y, MeasureText(sText, FontSize - 2) + 28.0f, ControlHeight), sId, sText);
    y += ControlHeight + 10.0f;
  };

  auto options = [&](const char* szPrefix, plArrayPtr<const char* const> labels, plUInt32 uiOn)
  {
    const float fWidth = fControlWidth / labels.GetCount();
    for (plUInt32 i = 0; i < labels.GetCount(); ++i)
      add(ItemType::Option, plRectFloat(fControlX + i * fWidth, y, fWidth, ControlHeight), IndexedName(szPrefix, i), labels[i]).m_bOn = i == uiOn;
  };

  if (m_Selection == SelectionKind::Segment)
  {
    const plAudioMusicSegment& segment = pProp->m_Segments[m_uiSelected];
    const double fSeconds = SegmentSeconds(segment);

    header("Segment");

    label("Name");
    add(ItemType::Field, plRectFloat(fControlX, y, fControlWidth, ControlHeight), "Mu.F.SegName");
    y += s_fRow;

    label("Tempo");
    add(ItemType::Number, plRectFloat(fControlX, y, fControlWidth, ControlHeight), "Mu.F.Tempo", {}, "BPM");
    y += s_fRow;

    label("Meter");
    plUInt32 uiMeter = plInvalidIndex;
    for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_Meters); ++i)
    {
      if (s_Meters[i] == segment.m_uiBeatsPerBar)
        uiMeter = i;
    }
    options("Mu.Meter.", plMakeArrayPtr(s_szMeters), uiMeter);
    y += s_fRow;

    label("Bars");
    add(ItemType::Number, plRectFloat(fControlX, y, fControlWidth - 62.0f, ControlHeight), "Mu.F.Bars");
    {
      plStringBuilder sLength;
      sLength.SetFormat("{} s", plArgF(fSeconds, 2));
      add(ItemType::Label, plRectFloat(fRight - 58.0f, y, 58.0f, ControlHeight), {}, {}, sLength);
    }
    y += s_fRow;

    // the exit cues: a chip after every bar but the last
    label("Exit cues");
    if (segment.m_uiBars <= 1)
    {
      add(ItemType::Label, plRectFloat(fControlX, y, fControlWidth, ControlHeight), {}, {}, "one bar: none");
      y += s_fRow;
    }
    else
    {
      float fChipX = fControlX;
      float fChipY = y + 3.0f;
      for (plUInt32 uiBar = 1; uiBar < segment.m_uiBars && uiBar <= 64; ++uiBar)
      {
        plStringBuilder sChip;
        sChip.SetFormat("after {}", uiBar);
        const float fWidth = MeasureCodeText(sChip, MinFontSize) + 12.0f;
        if (fChipX + fWidth > fRight && fChipX > fControlX)
        {
          fChipX = fControlX;
          fChipY += 21.0f;
        }

        add(ItemType::Chip, plRectFloat(fChipX, fChipY, fWidth, 18.0f), IndexedName("Mu.Cue.", uiBar), sChip).m_bOn = segment.m_ExitCues.Contains(uiBar);
        fChipX += fWidth + 3.0f;
      }
      y = fChipY + 18.0f + 8.0f;
    }

    // stems that do not fit the bars
    plStringBuilder sMismatch;
    for (const plAudioMusicStem& stem : segment.m_Stems)
    {
      const double fClip = plAudioMusicEdit::GetClipSeconds(stem.m_sClip);
      if (plMath::Abs(fClip - fSeconds) <= s_fFitTolerance)
        continue;

      if (!sMismatch.IsEmpty())
        sMismatch.Append(", ");
      sMismatch.AppendFormat("{} is {} s", plAudioMusicEdit::GetClipName(stem.m_sClip), plArgF(fClip, 2));
    }

    if (!sMismatch.IsEmpty())
    {
      sMismatch.AppendFormat(": the segment is {} s. It is cut at its end; a shorter clip leaves a gap.", plArgF(fSeconds, 2));
      text(sMismatch, ToneWarning);
    }
    else if (!segment.m_Stems.IsEmpty())
    {
      text("Every stem is as long as the segment.", ToneGood);
    }

    header("Stems", "start together with the segment");

    for (plUInt32 k = 0; k < segment.m_Stems.GetCount(); ++k)
    {
      const plAudioMusicStem& stem = segment.m_Stems[k];
      const plUInt32 uiCard = out_items.GetCount();
      const float fCardTop = y;
      add(ItemType::Card, plRectFloat(x - 2.0f, y, fRight - x + 4.0f, 0.0f));

      y += 4.0f;
      add(ItemType::Label, plRectFloat(x + 8.0f, y, fRight - x - 40.0f, 20.0f), {}, plAudioMusicEdit::GetClipName(stem.m_sClip)).m_bOn = true;
      add(ItemType::Remove, plRectFloat(fRight - 22.0f, y, 20.0f, 20.0f), IndexedName("Mu.StemDel.", k));
      y += 24.0f;

      const float fSliderX = x + 8.0f + 70.0f;
      const float fSliderWidth = plMath::Max(fRight - 8.0f - 52.0f - fSliderX, 30.0f);

      label("Volume", 8.0f, 70.0f);
      add(ItemType::Slider, plRectFloat(fSliderX, y, fSliderWidth, ControlHeight), IndexedName("Mu.StemVol.", k)).m_fValue = (stem.m_fVolumeDb - s_fVolumeMin) / (s_fVolumeMax - s_fVolumeMin);
      {
        plStringBuilder sValue;
        sValue.SetFormat("{} dB", plArgF(stem.m_fVolumeDb, 0));
        add(ItemType::Label, plRectFloat(fRight - 56.0f, y, 48.0f, ControlHeight), {}, {}, sValue);
      }
      y += s_fRow;

      label("Intensity", 8.0f, 70.0f);
      {
        plStringBuilder sFollow = "always plays";
        if (stem.m_bFollowIntensity)
          sFollow.SetFormat("plays from {} to {}", plArgF(stem.m_fIntensityMin, 2), plArgF(stem.m_fIntensityMax, 2));
        add(ItemType::Toggle, plRectFloat(fSliderX, y, fRight - 8.0f - fSliderX, ControlHeight), IndexedName("Mu.StemInt.", k), {}, sFollow).m_bOn = stem.m_bFollowIntensity;
      }
      y += s_fRow;

      if (stem.m_bFollowIntensity)
      {
        label("From", 8.0f, 70.0f);
        add(ItemType::Slider, plRectFloat(fSliderX, y, fSliderWidth, ControlHeight), IndexedName("Mu.StemLo.", k)).m_fValue = stem.m_fIntensityMin;

        plStringBuilder sValue;
        sValue.SetFormat("{}", plArgF(stem.m_fIntensityMin, 2));
        add(ItemType::Label, plRectFloat(fRight - 56.0f, y, 48.0f, ControlHeight), {}, {}, sValue);
        y += s_fRow;
      }

      out_items[uiCard].m_Rect.height = y - fCardTop + 2.0f;
      y += 6.0f;
    }

    add(ItemType::DropZone, plRectFloat(x - 2.0f, y, fRight - x + 4.0f, 36.0f), "Mu.DropStem", "drop a clip to add a stem");
    y += 44.0f;

    // where the segment plays
    plStringBuilder sUsers;
    for (const plAudioMusicState& state : pProp->m_States)
    {
      for (const plString& sName : state.m_Segments)
      {
        if (!sName.IsEqual_NoCase(segment.m_sName))
          continue;

        sUsers.Append(sUsers.IsEmpty() ? "" : ", ", state.m_sName);
        break;
      }
    }

    plStringBuilder sVias;
    for (const plAudioMusicTransition& transition : pProp->m_Transitions)
    {
      if (transition.m_sVia.IsEqual_NoCase(segment.m_sName))
        sVias.AppendFormat("{}{} -> {}", sVias.IsEmpty() ? "" : ", ", transition.m_sFrom.IsEmpty() ? plStringView("any") : transition.m_sFrom.GetView(), transition.m_sTo);
    }

    plStringBuilder sWhere;
    sWhere.SetFormat("In {}", sUsers.IsEmpty() ? plStringView("no state") : sUsers.GetView());
    if (!sVias.IsEmpty())
      sWhere.AppendFormat(" \xC2\xB7 between {}", sVias);
    text(sWhere);

    danger("Mu.DelSeg", "Delete segment");
  }
  else if (m_Selection == SelectionKind::State)
  {
    const plAudioMusicState& state = pProp->m_States[m_uiSelected];

    header("State");

    label("Name");
    add(ItemType::Field, plRectFloat(fControlX, y, fControlWidth, ControlHeight), "Mu.F.StateName");
    y += s_fRow;

    label("Starts here");
    add(ItemType::Toggle, plRectFloat(fControlX, y, fControlWidth, ControlHeight), "Mu.Initial", {}, "initial state").m_bOn = GetInitialState() == m_uiSelected;
    y += s_fRow + 4.0f;

    header("Segments", "in play order; drag to reorder");

    for (plUInt32 p = 0; p < state.m_Segments.GetCount(); ++p)
    {
      const plUInt32 uiSegment = FindSegment(*pProp, state.m_Segments[p]);

      plStringBuilder sLength("missing");
      if (uiSegment != plInvalidIndex)
        sLength.SetFormat("{} s", plArgF(GetSegmentSeconds(uiSegment), 1));

      add(ItemType::Row, plRectFloat(x - 2.0f, y, fRight - x + 4.0f - 26.0f, 24.0f), IndexedName("Mu.Item.", p), state.m_Segments[p], sLength);
      add(ItemType::Remove, plRectFloat(fRight - 20.0f, y + 2.0f, 20.0f, 20.0f), IndexedName("Mu.ItemDel.", p));
      y += 27.0f;
    }

    if (state.m_Segments.IsEmpty())
      text("Drop a clip on the lane, or + Segment.");
    else
      y += 6.0f;

    plStringBuilder sGame;
    sGame.SetFormat("The game asks for it with SetMusicState(\"{}\") or State = {}.", state.m_sName, m_uiSelected);
    text(sGame);

    danger("Mu.DelState", "Delete state");
  }
  else if (m_Selection == SelectionKind::Transition)
  {
    const plUInt32 uiTransition = plAudioMusicEdit::FindTransition(m_pDocument, m_uiSelected, m_uiSelectedTo);
    const plStringView sFrom = m_uiSelected == plInvalidIndex ? plStringView("any state") : pProp->m_States[m_uiSelected].m_sName.GetView();
    const plStringView sTo = pProp->m_States[m_uiSelectedTo].m_sName;

    plStringBuilder sChange;
    sChange.SetFormat("{} -> {}", sFrom, sTo);

    if (uiTransition == plInvalidIndex)
    {
      header("Transition");

      plStringBuilder sText;
      sText.SetFormat("{} has no rule of its own: {}.", sChange, m_uiSelected == plInvalidIndex ? "none" : "it changes on the next bar and cuts");
      text(sText);

      const float fWidth = plAudioEngineUi::TextButtonWidth("+ Set a rule", false);
      add(ItemType::Button, plRectFloat(x, y, fWidth, ControlHeight), "Mu.MkTrans", "+ Set a rule");
      y += ControlHeight + 8.0f;
    }
    else
    {
      const plAudioMusicTransition& transition = pProp->m_Transitions[uiTransition];
      header("Transition", sChange);

      label("Change at");
      add(ItemType::Dropdown, plRectFloat(fControlX, y, fControlWidth, ControlHeight), "Mu.ChangeAt", s_szQuantize[plMath::Min<plUInt32>(transition.m_eChangeAt.GetValue(), 4)]);
      y += s_fRow;

      label("Fade");
      const float fSliderWidth = fControlWidth - 62.0f;
      add(ItemType::Slider, plRectFloat(fControlX, y, fSliderWidth, ControlHeight), "Mu.Fade").m_fValue = transition.m_fFadeBeats / s_fFadeMax;
      {
        plStringBuilder sFade = "cut";
        if (transition.m_fFadeBeats > 0.0f)
          sFade.SetFormat("{} beats", plArgF(transition.m_fFadeBeats, 1));
        add(ItemType::Label, plRectFloat(fRight - 58.0f, y, 58.0f, ControlHeight), {}, {}, sFade);
      }
      y += s_fRow;

      label("Via");
      plStringBuilder sVia = "none";
      const plUInt32 uiVia = FindSegment(*pProp, transition.m_sVia);
      if (uiVia != plInvalidIndex)
        sVia.SetFormat("{} ({} bars)", pProp->m_Segments[uiVia].m_sName, pProp->m_Segments[uiVia].m_uiBars);
      add(ItemType::Dropdown, plRectFloat(fControlX, y, fControlWidth, ControlHeight), "Mu.Via", sVia);
      y += s_fRow;

      label("Start at");
      constexpr const char* szStartAt[] = {"segment start", "same position"};
      options("Mu.StartAt.", plMakeArrayPtr(szStartAt), transition.m_eStartAt.GetValue() == plAudioMusicStartAt::SamePosition ? 1 : 0);
      y += s_fRow + 4.0f;

      if (transition.m_eChangeAt == plAudioMusicQuantize::ExitCue)
        text("Leaves at the next exit cue of the playing segment, or at its end.");

      danger("Mu.DelTrans", "Remove rule");
    }
  }
  else if (m_Selection == SelectionKind::Stinger)
  {
    const plAudioMusicStinger& stinger = pProp->m_Stingers[m_uiSelected];

    header("Stinger");

    label("Name");
    add(ItemType::Field, plRectFloat(fControlX, y, fControlWidth, ControlHeight), "Mu.F.StingName");
    y += s_fRow;

    label("Clip");
    add(ItemType::Dropdown, plRectFloat(fControlX, y, fControlWidth, ControlHeight), "Mu.StingClip", stinger.m_sClip.IsEmpty() ? plString("none") : plAudioMusicEdit::GetClipName(stinger.m_sClip));
    y += s_fRow;

    label("Plays at");
    add(ItemType::Dropdown, plRectFloat(fControlX, y, fControlWidth, ControlHeight), "Mu.StingAt", s_szQuantize[plMath::Min<plUInt32>(stinger.m_eAt.GetValue(), 4)]);
    y += s_fRow + 4.0f;

    plStringBuilder sGame;
    sGame.SetFormat("The game plays it with PlayStinger(\"{}\").", stinger.m_sName);
    text(sGame);

    danger("Mu.DelSting", "Delete stinger");
  }
  else
  {
    header("Music");
    text("Select a segment, a state, a transition or a stinger.");
    text("Drag clips from the left onto a state's lane to make segments.");
  }

  out_fHeight = y - fTop + 8.0f;
}

//////////////////////////////////////////////////////////////////////////
// Building

void plEditorUiAudioMusicView::OnUiUpdate()
{
  if (!IsReady())
    return;

  const plStringView sFocused = GetSurface()->GetCanvas().GetFocusedElementName();

  // a field commits when it loses focus
  if (!m_sFocusedField.IsEmpty() && sFocused != m_sFocusedField)
  {
    const plString sField = m_sFocusedField;
    m_sFocusedField.Clear();
    CommitField(sField);
  }

  if (sFocused.StartsWith("Mu.F."))
    m_sFocusedField = sFocused;

  if (m_uiRenameState != plInvalidIndex && !m_bRebuild)
  {
    if (sFocused == s_szRename)
      m_bRenameHadFocus = true;
    else if (m_bRenameHadFocus)
      CommitRename();
  }

  // clips appear and change while the window is open
  if (plTime::Now() - m_ClipsGathered > plTime::MakeFromSeconds(2.0) && m_DragKind == DragKind::None)
    GatherClips();

  if (GetSurface()->GetLogicalSize() != m_vBuiltSize)
    m_bRebuild = true;

  // the document is rebuilt only while no field is being edited
  if (m_bRebuild && m_sFocusedField.IsEmpty())
    BuildDocument();

  Draw();

  // the playhead and the toast move
  if (m_bAuditioning || Now() - m_fToastTime < s_fToastSeconds)
    RequestRedraw(2);
}

void plEditorUiAudioMusicView::BuildDocument()
{
  using namespace plEditorUi;

  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();

  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  plBlackboard* pBoard = &GetSurface()->GetBlackboard();

  Layout layout;
  ComputeLayout(layout);

  // the scrolls stay inside their content
  m_fCenterScroll = plMath::Clamp(m_fCenterScroll, 0.0f, plMath::Max(layout.m_fCenterHeight - layout.m_Center.height, 0.0f));

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = Surface();
  DrawList(pRoot, "Music.Drawing", s_sDrawingEntry.GetView());

  // toolbar
  const ToolbarLayout toolbar = LayoutToolbar(*pProp, m_vBuiltSize.x);
  plAudioEngineUi::HitButton(pRoot, toolbar.m_Play, "Mu.Play", "Play the music in the engine process from its initial state", pBoard);
  plAudioEngineUi::HitButton(pRoot, toolbar.m_Stop, "Mu.Stop", "Stop with the music's stop fade", pBoard);

  for (plUInt32 i = 0; i < toolbar.m_States.GetCount(); ++i)
    plAudioEngineUi::HitButton(pRoot, toolbar.m_States[i], IndexedName("Mu.State.", i), "Ask for this state: the music changes where its rule allows (or starts in it)", pBoard);

  for (plUInt32 i = 0; i < toolbar.m_Stingers.GetCount(); ++i)
    plAudioEngineUi::HitButton(pRoot, toolbar.m_Stingers[i], IndexedName("Mu.Sting.", i), "Select the stinger; while the music plays, play it on its beat", pBoard);

  plAudioEngineUi::HitButton(pRoot, toolbar.m_AddState, "Mu.AddState", "A new state; name it, then drop clips on its lane", pBoard);
  plAudioEngineUi::HitButton(pRoot, toolbar.m_AddSegment, "Mu.AddSegment", "A new empty segment in the selected state (or the first)", pBoard);
  plAudioEngineUi::HitButton(pRoot, toolbar.m_AddStinger, "Mu.AddStinger", "A new stinger the game plays over the music", pBoard);

  // clips: the filter and a slot per visible row (the rows scroll under them)
  {
    plUiPanel* pSearch = Panel(pRoot);
    plAudioEngineUi::Place(pSearch, plRectFloat(8.0f, s_fToolRow + s_fSectionHead, s_fClipsWidth - 16.0f, ControlHeight));
    pSearch->m_Color = plColorGammaUB(0, 0, 0, 0);
    m_pClipSearch->Build(pSearch, "filter clips...");
    m_pClipSearch->SetText(m_sClipFilter);

    const plUInt32 uiSlots = plMath::Min(static_cast<plUInt32>(layout.m_ClipList.height / s_fClipRow), s_uiClipSlots);
    for (plUInt32 k = 0; k < uiSlots; ++k)
      plAudioEngineUi::HitButton(pRoot, plRectFloat(6.0f, layout.m_ClipList.y + k * s_fClipRow, s_fClipsWidth - 12.0f, s_fClipRow - 2.0f), IndexedName("Mu.ClipRow.", k), "Drag onto a state's lane, a segment or 'Not in a state'", pBoard);
  }

  // lanes and blocks, where they show inside the center column
  auto visibleIn = [](const plRectFloat& rect, const plRectFloat& area) -> bool
  { return rect.y >= area.y && rect.y + rect.height <= area.y + area.height; };

  for (plUInt32 uiLane = 0; uiLane < pProp->m_States.GetCount(); ++uiLane)
  {
    if (visibleIn(layout.m_Lanes[uiLane].m_Header, layout.m_Center) && uiLane != m_uiRenameState)
      plAudioEngineUi::HitButton(pRoot, layout.m_Lanes[uiLane].m_Header, IndexedName("Mu.Lane.", uiLane), "Select the state; double-click to rename it", pBoard);
  }

  for (plUInt32 b = 0; b < layout.m_Blocks.GetCount(); ++b)
  {
    if (visibleIn(layout.m_Blocks[b].m_Rect, layout.m_Center))
      plAudioEngineUi::HitButton(pRoot, layout.m_Blocks[b].m_Rect, IndexedName("Mu.Seg.", b), "Select the segment; drag it to reorder, onto another lane to play it there too", pBoard);
  }

  for (plUInt32 c = 0; c < layout.m_Cells.GetCount(); ++c)
  {
    const Cell& cell = layout.m_Cells[c];
    if (cell.m_uiFrom != cell.m_uiTo && visibleIn(cell.m_Rect, layout.m_Center))
      plAudioEngineUi::HitButton(pRoot, cell.m_Rect, IndexedName("Mu.Cell.", c), "How the music changes from the row's state to the column's", pBoard);
  }

  // the inline rename of a state
  if (m_uiRenameState != plInvalidIndex)
  {
    const plRectFloat& header = layout.m_Lanes[m_uiRenameState].m_Header;
    plUiTextInput* pInput = Input(pRoot, s_szRename, s_sRenameText.GetView(), "", s_szRenameSubmit);
    plAudioEngineUi::Place(pInput, plRectFloat(header.x + 4.0f, header.y + 4.0f, header.width - 8.0f, 22.0f));
    pInput->m_BorderColor = Accent();
    pInput->m_vCornerRadius = plVec4(3);
  }

  // the inspector's controls, where they show inside its column
  plHybridArray<Item, 32> items;
  float fInspectorHeight = 0.0f;
  m_fInspectorScroll = plMath::Max(m_fInspectorScroll, 0.0f);
  ComputeInspector(layout, items, fInspectorHeight);

  if (m_fInspectorScroll > plMath::Max(fInspectorHeight - layout.m_Inspector.height, 0.0f))
  {
    m_fInspectorScroll = plMath::Max(fInspectorHeight - layout.m_Inspector.height, 0.0f);
    ComputeInspector(layout, items, fInspectorHeight);
  }

  for (const Item& item : items)
  {
    if (!visibleIn(item.m_Rect, layout.m_Inspector))
      continue;

    switch (item.m_Type)
    {
      case ItemType::Field:
      {
        plUiTextInput* pInput = Input(pRoot, item.m_sId, TextBinding(item.m_sId), "", SubmitEvent(item.m_sId));
        plAudioEngineUi::Place(pInput, item.m_Rect);
        break;
      }

      case ItemType::Number:
        NumberField(pRoot, item.m_Rect, item.m_sId, TextBinding(item.m_sId), SubmitEvent(item.m_sId), {}, item.m_sSub);
        break;

      case ItemType::Toggle:
      case ItemType::Option:
      case ItemType::Chip:
      case ItemType::Dropdown:
      case ItemType::Button:
      case ItemType::Danger:
      case ItemType::Row:
      case ItemType::Remove:
      case ItemType::DropZone:
        plAudioEngineUi::HitButton(pRoot, item.m_Rect, item.m_sId, {}, pBoard);
        break;

      default:
        break;
    }
  }

  SetDocument(Finish(pRoot));
  UpdateFieldTexts();

  // the rename starts with the whole name selected, so typing replaces it
  if (m_uiRenameState != plInvalidIndex && GetSurface()->GetCanvas().FocusElement(s_szRename, m_vBuiltSize) && !m_bRenameHadFocus)
    GetSurface()->GetCanvas().InjectEditKey(plUiEditKey::SelectAll);
}

void plEditorUiAudioMusicView::UpdateFieldTexts()
{
  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  plBlackboard& board = GetSurface()->GetBlackboard();

  auto set = [&](plStringView sField, plStringView sText)
  {
    if (sField != m_sFocusedField)
      board.SetEntryValue(TextBinding(sField), plString(sText));
  };

  if (m_Selection == SelectionKind::Segment)
  {
    const plAudioMusicSegment& segment = pProp->m_Segments[m_uiSelected];
    set("Mu.F.SegName", segment.m_sName);

    plStringBuilder s;
    s.SetFormat("{}", plArgF(segment.m_fTempo, segment.m_fTempo == plMath::Round(segment.m_fTempo) ? 0 : 1));
    set("Mu.F.Tempo", s);

    s.SetFormat("{}", segment.m_uiBars);
    set("Mu.F.Bars", s);
  }
  else if (m_Selection == SelectionKind::State)
  {
    set("Mu.F.StateName", pProp->m_States[m_uiSelected].m_sName);
  }
  else if (m_Selection == SelectionKind::Stinger)
  {
    set("Mu.F.StingName", pProp->m_Stingers[m_uiSelected].m_sName);
  }
}

void plEditorUiAudioMusicView::CommitField(plStringView sField)
{
  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  const plString sText = GetSurface()->GetBlackboard().GetEntryValue(plTempHashedString(TextBinding(sField))).ConvertTo<plString>();

  auto taken = [&](plStringView sOld)
  {
    plStringBuilder sTrimmed = sText;
    sTrimmed.Trim(" \t");
    if (!sTrimmed.IsEmpty() && !sTrimmed.IsEqual_NoCase(sOld))
    {
      plStringBuilder sNote;
      sNote.SetFormat("'{}' is taken", sTrimmed);
      ShowToast(sNote);
    }
  };

  if (sField == "Mu.F.SegName" && m_Selection == SelectionKind::Segment)
  {
    const plString sOld = pProp->m_Segments[m_uiSelected].m_sName;
    plAudioMusicEdit::RenameSegment(m_pDocument, m_uiSelected, sText);
    if (pProp->m_Segments[m_uiSelected].m_sName == sOld)
      taken(sOld);
  }
  else if (sField == "Mu.F.StateName" && m_Selection == SelectionKind::State)
  {
    const plString sOld = pProp->m_States[m_uiSelected].m_sName;
    plAudioMusicEdit::RenameState(m_pDocument, m_uiSelected, sText);
    if (pProp->m_States[m_uiSelected].m_sName == sOld)
      taken(sOld);
  }
  else if (sField == "Mu.F.StingName" && m_Selection == SelectionKind::Stinger)
  {
    const plString sOld = pProp->m_Stingers[m_uiSelected].m_sName;
    plAudioMusicEdit::RenameStinger(m_pDocument, m_uiSelected, sText);
    if (pProp->m_Stingers[m_uiSelected].m_sName == sOld)
      taken(sOld);
  }
  else if (sField == "Mu.F.Tempo" && m_Selection == SelectionKind::Segment)
  {
    double fTempo = 0.0;
    if (plConversionUtils::StringToFloat(sText, fTempo).Succeeded())
      plAudioMusicEdit::SetValue(m_pDocument, plAudioMusicEdit::GetElement(m_pDocument, "Segments", m_uiSelected), "Tempo", static_cast<float>(plMath::Clamp(fTempo, 20.0, 400.0)), "Set Tempo");
  }
  else if (sField == "Mu.F.Bars" && m_Selection == SelectionKind::Segment)
  {
    plUInt32 uiBars = 0;
    if (plConversionUtils::StringToUInt(sText, uiBars).Succeeded())
      plAudioMusicEdit::SetBars(m_pDocument, m_uiSelected, uiBars);
  }

  // a rejected value shows the document's again
  UpdateFieldTexts();
}

void plEditorUiAudioMusicView::BeginRename(plUInt32 uiState)
{
  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  if (uiState >= pProp->m_States.GetCount())
    return;

  m_uiRenameState = uiState;
  m_bRenameHadFocus = false;
  GetSurface()->GetBlackboard().SetEntryValue(s_sRenameText, pProp->m_States[uiState].m_sName);
  m_bRebuild = true;
  RequestRedraw();
}

void plEditorUiAudioMusicView::CommitRename()
{
  if (m_uiRenameState == plInvalidIndex)
    return;

  const plUInt32 uiState = m_uiRenameState;
  const plString sText = GetSurface()->GetBlackboard().GetEntryValue(s_sRenameText).ConvertTo<plString>();

  m_uiRenameState = plInvalidIndex;
  m_bRebuild = true;

  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  if (uiState >= pProp->m_States.GetCount())
    return;

  const plString sOld = pProp->m_States[uiState].m_sName;
  plAudioMusicEdit::RenameState(m_pDocument, uiState, sText);

  plStringBuilder sTrimmed = sText;
  sTrimmed.Trim(" \t");
  if (pProp->m_States[uiState].m_sName == sOld && !sTrimmed.IsEmpty() && !sTrimmed.IsEqual_NoCase(sOld))
  {
    plStringBuilder sNote;
    sNote.SetFormat("'{}' is taken", sTrimmed);
    ShowToast(sNote);
  }
}

//////////////////////////////////////////////////////////////////////////
// Events

void plEditorUiAudioMusicView::OnUiEvent(const plMsgUiEvent& event)
{
  const plStringView sId = event.m_sIdentifier.GetView();
  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();

  if (sId == "Mu.Play")
  {
    if (m_OnPlay.IsValid())
      m_OnPlay();
    return;
  }

  if (sId == "Mu.Stop")
  {
    if (m_OnStop.IsValid())
      m_OnStop();
    return;
  }

  if (sId == s_szRenameSubmit)
  {
    CommitRename();
    return;
  }

  if (sId.StartsWith("Mu.F.") && sId.EndsWith(".Submit"))
  {
    plStringView sField = sId;
    sField.Shrink(0, 7);
    CommitField(sField);
    return;
  }

  if (const plUInt32 uiState = IndexAfter(sId, "Mu.State."); uiState != plInvalidIndex)
  {
    if (m_OnStateChosen.IsValid())
      m_OnStateChosen(uiState);
    return;
  }

  if (const plUInt32 uiStinger = IndexAfter(sId, "Mu.Sting."); uiStinger < pProp->m_Stingers.GetCount())
  {
    Select(SelectionKind::Stinger, uiStinger);
    if (m_bAuditioning && m_OnStinger.IsValid())
      m_OnStinger(uiStinger);
    return;
  }

  if (sId == "Mu.AddState")
  {
    const plUInt32 uiState = plAudioMusicEdit::AddState(m_pDocument);
    if (uiState != plInvalidIndex)
    {
      Select(SelectionKind::State, uiState);
      BeginRename(uiState);
    }
    return;
  }

  if (sId == "Mu.AddSegment")
  {
    plUInt32 uiState = m_Selection == SelectionKind::State ? m_uiSelected : 0;
    if (uiState >= pProp->m_States.GetCount())
      uiState = plInvalidIndex;

    const plUInt32 uiSegment = plAudioMusicEdit::AddEmptySegment(m_pDocument, GetStateTempo(uiState), uiState);
    if (uiSegment != plInvalidIndex)
      Select(SelectionKind::Segment, uiSegment);
    return;
  }

  if (sId == "Mu.AddStinger")
  {
    const plUInt32 uiStinger = plAudioMusicEdit::AddStinger(m_pDocument, {});
    if (uiStinger != plInvalidIndex)
      Select(SelectionKind::Stinger, uiStinger);
    return;
  }

  if (const plUInt32 uiLane = IndexAfter(sId, "Mu.Lane."); uiLane < pProp->m_States.GetCount())
  {
    Select(SelectionKind::State, uiLane);
    return;
  }

  Layout layout;
  ComputeLayout(layout);

  if (const plUInt32 uiBlock = IndexAfter(sId, "Mu.Seg."); uiBlock < layout.m_Blocks.GetCount())
  {
    Select(SelectionKind::Segment, layout.m_Blocks[uiBlock].m_uiSegment);
    return;
  }

  if (const plUInt32 uiCell = IndexAfter(sId, "Mu.Cell."); uiCell < layout.m_Cells.GetCount())
  {
    Select(SelectionKind::Transition, layout.m_Cells[uiCell].m_uiFrom, layout.m_Cells[uiCell].m_uiTo);
    return;
  }

  // the inspector
  plHybridArray<Item, 32> items;
  float fHeight = 0.0f;
  ComputeInspector(layout, items, fHeight);

  plRectFloat itemRect(0, 0, 0, 0);
  for (const Item& item : items)
  {
    if (item.m_sId == sId)
      itemRect = item.m_Rect;
  }

  if (m_Selection == SelectionKind::Segment)
  {
    const plDocumentObject* pSegment = plAudioMusicEdit::GetElement(m_pDocument, "Segments", m_uiSelected);
    const plAudioMusicSegment& segment = pProp->m_Segments[m_uiSelected];

    if (const plUInt32 uiMeter = IndexAfter(sId, "Mu.Meter."); uiMeter < PL_ARRAY_SIZE(s_Meters))
      plAudioMusicEdit::SetValue(m_pDocument, pSegment, "BeatsPerBar", s_Meters[uiMeter], "Set Meter");
    else if (const plUInt32 uiCue = IndexAfter(sId, "Mu.Cue."); uiCue != plInvalidIndex)
      plAudioMusicEdit::ToggleExitCue(m_pDocument, m_uiSelected, uiCue);
    else if (const plUInt32 uiStem = IndexAfter(sId, "Mu.StemDel."); uiStem != plInvalidIndex)
      plAudioMusicEdit::RemoveStem(m_pDocument, m_uiSelected, uiStem);
    else if (const plUInt32 uiFollow = IndexAfter(sId, "Mu.StemInt."); uiFollow < segment.m_Stems.GetCount())
      plAudioMusicEdit::SetValue(m_pDocument, plAudioMusicEdit::GetElement(m_pDocument, pSegment, "Stems", uiFollow), "FollowIntensity", !segment.m_Stems[uiFollow].m_bFollowIntensity, "Set Follow Intensity");
    else if (sId == "Mu.DropStem")
      ShowClipMenu(itemRect, false);
    else if (sId == "Mu.DelSeg")
    {
      plAudioMusicEdit::DeleteSegment(m_pDocument, m_uiSelected);
      Select(SelectionKind::None, 0);
    }
  }
  else if (m_Selection == SelectionKind::State)
  {
    if (sId == "Mu.Initial")
      plAudioMusicEdit::SetInitialState(m_pDocument, m_uiSelected);
    else if (const plUInt32 uiPosition = IndexAfter(sId, "Mu.ItemDel."); uiPosition != plInvalidIndex)
      plAudioMusicEdit::RemoveStateSegment(m_pDocument, m_uiSelected, uiPosition);
    else if (const plUInt32 uiItem = IndexAfter(sId, "Mu.Item."); uiItem < pProp->m_States[m_uiSelected].m_Segments.GetCount())
    {
      const plUInt32 uiSegment = FindSegment(*pProp, pProp->m_States[m_uiSelected].m_Segments[uiItem]);
      if (uiSegment != plInvalidIndex)
        Select(SelectionKind::Segment, uiSegment);
    }
    else if (sId == "Mu.DelState")
    {
      plAudioMusicEdit::DeleteState(m_pDocument, m_uiSelected);
      Select(SelectionKind::None, 0);
    }
  }
  else if (m_Selection == SelectionKind::Transition)
  {
    const plUInt32 uiTransition = plAudioMusicEdit::FindTransition(m_pDocument, m_uiSelected, m_uiSelectedTo);
    const plDocumentObject* pTransition = plAudioMusicEdit::GetElement(m_pDocument, "Transitions", uiTransition);

    if (sId == "Mu.MkTrans")
    {
      plAudioMusicEdit::AddTransition(m_pDocument, m_uiSelected, m_uiSelectedTo);
      Select(SelectionKind::Transition, m_uiSelected, m_uiSelectedTo);
    }
    else if (sId == "Mu.ChangeAt")
      ShowChangeAtMenu(itemRect);
    else if (sId == "Mu.Via")
      ShowViaMenu(itemRect);
    else if (const plUInt32 uiStartAt = IndexAfter(sId, "Mu.StartAt."); uiStartAt <= 1)
      plAudioMusicEdit::SetValue(m_pDocument, pTransition, "StartAt", static_cast<plInt64>(uiStartAt), "Set Start At");
    else if (sId == "Mu.DelTrans")
    {
      plAudioMusicEdit::RemoveTransition(m_pDocument, uiTransition);
      Select(SelectionKind::Transition, m_uiSelected, m_uiSelectedTo);
    }
  }
  else if (m_Selection == SelectionKind::Stinger)
  {
    if (sId == "Mu.StingClip")
      ShowClipMenu(itemRect, true);
    else if (sId == "Mu.StingAt")
      ShowStingerAtMenu(itemRect);
    else if (sId == "Mu.DelSting")
    {
      plAudioMusicEdit::DeleteStinger(m_pDocument, m_uiSelected);
      Select(SelectionKind::None, 0);
    }
  }
}

void plEditorUiAudioMusicView::ShowChangeAtMenu(const plRectFloat& rect)
{
  const plUInt32 uiTransition = plAudioMusicEdit::FindTransition(m_pDocument, m_uiSelected, m_uiSelectedTo);
  if (uiTransition == plInvalidIndex)
    return;

  const plUInt32 uiCurrent = m_pDocument->GetProperties()->m_Transitions[uiTransition].m_eChangeAt.GetValue();

  plEditorUiMenu menu;
  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_szQuantize); ++i)
  {
    plEditorUiMenuItem& item = menu.AddAction(s_szQuantize[i], i);
    item.m_bCheckable = true;
    item.m_bChecked = i == uiCurrent;
  }

  plEditorUiMenuView::Show(*GetSurface(), plVec2(rect.x, rect.y + rect.height), menu, [this, uiTransition](plUInt32 uiId)
    { plAudioMusicEdit::SetValue(m_pDocument, plAudioMusicEdit::GetElement(m_pDocument, "Transitions", uiTransition), "ChangeAt", static_cast<plInt64>(uiId), "Set Change At"); });
}

void plEditorUiAudioMusicView::ShowViaMenu(const plRectFloat& rect)
{
  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  const plUInt32 uiTransition = plAudioMusicEdit::FindTransition(m_pDocument, m_uiSelected, m_uiSelectedTo);
  if (uiTransition == plInvalidIndex)
    return;

  const plString sCurrent = pProp->m_Transitions[uiTransition].m_sVia;

  plEditorUiMenu menu;
  plEditorUiMenuItem& none = menu.AddAction("none", 0);
  none.m_bCheckable = true;
  none.m_bChecked = sCurrent.IsEmpty();

  menu.AddSeparator();
  for (plUInt32 i = 0; i < pProp->m_Segments.GetCount(); ++i)
  {
    plStringBuilder sLabel;
    sLabel.SetFormat("{} ({} bars)", pProp->m_Segments[i].m_sName, pProp->m_Segments[i].m_uiBars);

    plEditorUiMenuItem& item = menu.AddAction(sLabel, i + 1);
    item.m_bCheckable = true;
    item.m_bChecked = pProp->m_Segments[i].m_sName.IsEqual_NoCase(sCurrent);
  }

  plEditorUiMenuView::Show(*GetSurface(), plVec2(rect.x, rect.y + rect.height), menu, [this, uiTransition](plUInt32 uiId)
    {
      const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
      const plString sVia = (uiId > 0 && uiId <= pProp->m_Segments.GetCount()) ? pProp->m_Segments[uiId - 1].m_sName : plString();
      plAudioMusicEdit::SetValue(m_pDocument, plAudioMusicEdit::GetElement(m_pDocument, "Transitions", uiTransition), "Via", sVia, "Set Via"); });
}

void plEditorUiAudioMusicView::ShowStingerAtMenu(const plRectFloat& rect)
{
  const plUInt32 uiStinger = m_uiSelected;
  const plUInt32 uiCurrent = m_pDocument->GetProperties()->m_Stingers[uiStinger].m_eAt.GetValue();

  // a stinger lands on the grid: not later than the next bar
  plEditorUiMenu menu;
  for (plUInt32 i = 0; i <= plAudioMusicQuantize::NextBar; ++i)
  {
    plEditorUiMenuItem& item = menu.AddAction(s_szQuantize[i], i);
    item.m_bCheckable = true;
    item.m_bChecked = i == uiCurrent;
  }

  plEditorUiMenuView::Show(*GetSurface(), plVec2(rect.x, rect.y + rect.height), menu, [this, uiStinger](plUInt32 uiId)
    { plAudioMusicEdit::SetValue(m_pDocument, plAudioMusicEdit::GetElement(m_pDocument, "Stingers", uiStinger), "At", static_cast<plInt64>(uiId), "Set Stinger At"); });
}

void plEditorUiAudioMusicView::ShowClipMenu(const plRectFloat& rect, bool bStinger)
{
  if (m_Clips.IsEmpty())
  {
    ShowToast("The project has no Audio Clips");
    return;
  }

  const plUInt32 uiSelected = m_uiSelected;
  plString sCurrent;
  if (bStinger)
    sCurrent = m_pDocument->GetProperties()->m_Stingers[uiSelected].m_sClip;

  plEditorUiMenu menu;
  plHybridArray<plString, 32> guids;
  for (const Clip& clip : m_Clips)
  {
    plEditorUiMenuItem& item = menu.AddAction(clip.m_sName, guids.GetCount());
    item.m_bCheckable = bStinger;
    item.m_bChecked = bStinger && clip.m_sGuid.IsEqual_NoCase(sCurrent);
    guids.PushBack(clip.m_sGuid);
  }

  plEditorUiMenuView::Show(*GetSurface(), plVec2(rect.x, rect.y + rect.height), menu, [this, guids, uiSelected, bStinger](plUInt32 uiId)
    {
      if (uiId >= guids.GetCount())
        return;

      if (bStinger)
        plAudioMusicEdit::SetValue(m_pDocument, plAudioMusicEdit::GetElement(m_pDocument, "Stingers", uiSelected), "Clip", guids[uiId], "Set Stinger Clip");
      else
        plAudioMusicEdit::AddStem(m_pDocument, uiSelected, guids[uiId]); });
}

void plEditorUiAudioMusicView::OnSearch(const plEditorUiSearchFieldEvent& e)
{
  if (e.m_Type != plEditorUiSearchFieldEvent::Type::TextChanged)
    return;

  plStringBuilder sText = m_pClipSearch->GetText();
  sText.Trim(" \t");
  m_sClipFilter = sText;
  m_uiClipScroll = 0;
  RequestRedraw();
}

void plEditorUiAudioMusicView::OnKey(const plEditorUiKeyEvent& e)
{
  if (e.m_bHandled || m_uiRenameState == plInvalidIndex)
    return;

  if (e.m_Key == plEditorUiKeyEvent::Key::Escape)
  {
    m_uiRenameState = plInvalidIndex;
    m_bRebuild = true;
    RequestRedraw();
    e.m_bHandled = true;
  }
}

bool plEditorUiAudioMusicView::FindSlider(const plVec2& vPos, Item& out_slider) const
{
  Layout layout;
  ComputeLayout(layout);
  if (!layout.m_Inspector.Contains(vPos))
    return false;

  plHybridArray<Item, 32> items;
  float fHeight = 0.0f;
  ComputeInspector(layout, items, fHeight);

  for (const Item& item : items)
  {
    if (item.m_Type == ItemType::Slider && plRectFloat(item.m_Rect.x - 4.0f, item.m_Rect.y, item.m_Rect.width + 8.0f, item.m_Rect.height).Contains(vPos))
    {
      out_slider = item;
      return true;
    }
  }

  return false;
}

void plEditorUiAudioMusicView::CommitSlider()
{
  const plString sSlider = m_sSlider;
  const float fValue = m_fSliderValue;
  m_sSlider.Clear();
  RequestRedraw();

  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();

  if (m_Selection == SelectionKind::Segment)
  {
    const plDocumentObject* pSegment = plAudioMusicEdit::GetElement(m_pDocument, "Segments", m_uiSelected);

    if (const plUInt32 uiStem = IndexAfter(sSlider, "Mu.StemVol."); uiStem != plInvalidIndex)
    {
      const float fDb = plMath::Round(s_fVolumeMin + fValue * (s_fVolumeMax - s_fVolumeMin));
      plAudioMusicEdit::SetValue(m_pDocument, plAudioMusicEdit::GetElement(m_pDocument, pSegment, "Stems", uiStem), "Volume", fDb, "Set Stem Volume");
    }
    else if (const plUInt32 uiLow = IndexAfter(sSlider, "Mu.StemLo."); uiLow != plInvalidIndex)
    {
      const float fMin = plMath::Round(fValue * 20.0f) / 20.0f;
      plAudioMusicEdit::SetValue(m_pDocument, plAudioMusicEdit::GetElement(m_pDocument, pSegment, "Stems", uiLow), "IntensityMin", fMin, "Set Intensity Range");
    }
  }
  else if (m_Selection == SelectionKind::Transition && sSlider == "Mu.Fade")
  {
    const plUInt32 uiTransition = plAudioMusicEdit::FindTransition(m_pDocument, m_uiSelected, m_uiSelectedTo);
    if (uiTransition < pProp->m_Transitions.GetCount())
    {
      const float fBeats = plMath::Round(fValue * s_fFadeMax * 2.0f) / 2.0f;
      plAudioMusicEdit::SetValue(m_pDocument, plAudioMusicEdit::GetElement(m_pDocument, "Transitions", uiTransition), "FadeBeats", fBeats, "Set Fade");
    }
  }
}

//////////////////////////////////////////////////////////////////////////
// Dragging

plEditorUiAudioMusicView::DropTarget plEditorUiAudioMusicView::FindDropTarget(const plVec2& vPos, DragKind kind) const
{
  DropTarget target;

  Layout layout;
  ComputeLayout(layout);

  // a clip onto the inspector's drop zone
  if (kind == DragKind::Clip && m_Selection == SelectionKind::Segment && layout.m_Inspector.Contains(vPos))
  {
    plHybridArray<Item, 32> items;
    float fHeight = 0.0f;
    ComputeInspector(layout, items, fHeight);

    for (const Item& item : items)
    {
      if (item.m_Type == ItemType::DropZone && item.m_Rect.Contains(vPos))
      {
        target.m_Kind = DropTarget::Kind::Stems;
        target.m_uiSegment = m_uiSelected;
      }
    }
    return target;
  }

  // a row of the state's play order onto another
  if (kind == DragKind::Row && m_Selection == SelectionKind::State && layout.m_Inspector.Contains(vPos))
  {
    plHybridArray<Item, 32> items;
    float fHeight = 0.0f;
    ComputeInspector(layout, items, fHeight);

    for (const Item& item : items)
    {
      const plUInt32 uiRow = IndexAfter(item.m_sId, "Mu.Item.");
      if (item.m_Type == ItemType::Row && uiRow != plInvalidIndex && vPos.y >= item.m_Rect.y && vPos.y < item.m_Rect.y + item.m_Rect.height + 3.0f && uiRow != m_uiDragPosition)
      {
        target.m_Kind = DropTarget::Kind::Row;
        target.m_uiPosition = uiRow;
      }
    }
    return target;
  }

  if (!layout.m_Center.Contains(vPos))
    return target;

  if (kind == DragKind::Clip)
  {
    for (const Block& block : layout.m_Blocks)
    {
      if (block.m_Rect.Contains(vPos))
      {
        target.m_Kind = DropTarget::Kind::Segment;
        target.m_uiSegment = block.m_uiSegment;
        return target;
      }
    }
  }

  if (kind != DragKind::Clip && kind != DragKind::Segment)
    return target;

  const plUInt32 uiLoose = GetLooseLane();
  for (plUInt32 uiLane = 0; uiLane < layout.m_Lanes.GetCount(); ++uiLane)
  {
    const plRectFloat& track = layout.m_Lanes[uiLane].m_Track;
    if (!track.Contains(vPos))
      continue;

    const bool bSameLane = kind == DragKind::Segment && uiLane == m_uiDragLane;

    // the place among the lane's blocks (without the dragged one) the pointer is before
    plUInt32 uiPosition = 0;
    float fCaret = track.x + 3.0f;
    for (const Block& block : layout.m_Blocks)
    {
      if (block.m_uiLane != uiLane || (bSameLane && block.m_uiPosition == m_uiDragPosition))
        continue;

      if (block.m_Rect.x + block.m_Rect.width * 0.5f < vPos.x)
      {
        ++uiPosition;
        fCaret = block.m_Rect.x + block.m_Rect.width + 1.0f;
      }
      else
      {
        fCaret = block.m_Rect.x - 2.0f;
        break;
      }
    }

    if (kind == DragKind::Segment)
    {
      // nothing changes: the segment's own place, or from the loose lane onto it
      if ((bSameLane && uiPosition == m_uiDragPosition) || (uiLane == uiLoose && m_uiDragLane == uiLoose))
        return target;
    }

    target.m_Kind = DropTarget::Kind::Lane;
    target.m_uiLane = uiLane;
    target.m_uiPosition = uiLane == uiLoose ? 0 : uiPosition;
    target.m_fCaretX = fCaret;
    return target;
  }

  return target;
}

void plEditorUiAudioMusicView::ApplyDrop(const DropTarget& target, DragKind kind, plStringView sClip)
{
  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  const plUInt32 uiLoose = GetLooseLane();

  if (kind == DragKind::Clip && !sClip.IsEmpty())
  {
    if (target.m_Kind == DropTarget::Kind::Lane)
    {
      const float fTempo = target.m_uiLane < uiLoose ? GetStateTempo(target.m_uiLane) : 120.0f;
      const plUInt32 uiSegment = plAudioMusicEdit::AddSegmentFromClip(m_pDocument, sClip, fTempo, target.m_uiLane < uiLoose ? target.m_uiLane : plInvalidIndex, target.m_uiPosition);
      if (uiSegment == plInvalidIndex)
        return;

      // the bars come from the clip: say whether it fits them
      const plAudioMusicSegment& segment = pProp->m_Segments[uiSegment];
      const double fClip = plAudioMusicEdit::GetClipSeconds(sClip);
      const plString sName = plAudioMusicEdit::GetClipName(sClip);

      plStringBuilder sNote;
      if (plMath::Abs(SegmentSeconds(segment) - fClip) <= s_fFitTolerance)
        sNote.SetFormat("{}: {} bars at {} BPM, from the clip's {} s", sName, segment.m_uiBars, plArgF(fTempo, 0), plArgF(fClip, 2));
      else
        sNote.SetFormat("{}: {} s is not a whole number of bars at {} BPM - set the tempo in the inspector", sName, plArgF(fClip, 2), plArgF(fTempo, 0));

      ShowToast(sNote);
      Select(SelectionKind::Segment, uiSegment);
    }
    else if (target.m_Kind == DropTarget::Kind::Segment || target.m_Kind == DropTarget::Kind::Stems)
    {
      plAudioMusicEdit::AddStem(m_pDocument, target.m_uiSegment, sClip);

      plStringBuilder sNote;
      sNote.SetFormat("{} plays with {}", plAudioMusicEdit::GetClipName(sClip), pProp->m_Segments[target.m_uiSegment].m_sName);
      ShowToast(sNote);
      Select(SelectionKind::Segment, target.m_uiSegment);
    }
    return;
  }

  if (kind == DragKind::Segment && target.m_Kind == DropTarget::Kind::Lane)
  {
    const plString sSegment = pProp->m_Segments[m_uiDragSegment].m_sName;

    if (target.m_uiLane == uiLoose)
    {
      const plString sState = pProp->m_States[m_uiDragLane].m_sName;
      plAudioMusicEdit::RemoveStateSegment(m_pDocument, m_uiDragLane, m_uiDragPosition);

      plStringBuilder sNote;
      sNote.SetFormat("{} no longer plays in {}", sSegment, sState);
      ShowToast(sNote);
    }
    else if (target.m_uiLane == m_uiDragLane)
    {
      plAudioMusicEdit::MoveStateSegment(m_pDocument, target.m_uiLane, m_uiDragPosition, target.m_uiPosition);
    }
    else
    {
      plAudioMusicEdit::InsertStateSegment(m_pDocument, target.m_uiLane, target.m_uiPosition, m_uiDragSegment);

      plStringBuilder sNote;
      sNote.SetFormat("{} now also plays in {}", sSegment, pProp->m_States[target.m_uiLane].m_sName);
      ShowToast(sNote);
    }
    return;
  }

  if (kind == DragKind::Row && target.m_Kind == DropTarget::Kind::Row && m_Selection == SelectionKind::State)
    plAudioMusicEdit::MoveStateSegment(m_pDocument, m_uiSelected, m_uiDragPosition, target.m_uiPosition);
}

void plEditorUiAudioMusicView::OnPointer(const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;
  using Button = plEditorUiPointerEvent::Button;

  auto endDrag = [&]()
  {
    m_DragKind = DragKind::None;
    m_DropTarget = DropTarget();
    m_sDragClip.Clear();
    RequestRedraw();
  };

  switch (e.m_Type)
  {
    case Type::Wheel:
    {
      Layout layout;
      ComputeLayout(layout);

      if (layout.m_Clips.Contains(e.m_vPosition))
      {
        plHybridArray<const Clip*, 64> clips;
        GetVisibleClips(clips);
        const plUInt32 uiRows = static_cast<plUInt32>(layout.m_ClipList.height / s_fClipRow);
        const plInt32 iMax = plMath::Max(static_cast<plInt32>(clips.GetCount()) - static_cast<plInt32>(uiRows), 0);
        m_uiClipScroll = static_cast<plUInt32>(plMath::Clamp(static_cast<plInt32>(m_uiClipScroll) - static_cast<plInt32>(e.m_fWheel * 3.0f), 0, iMax));
        RequestRedraw();
      }
      else if (layout.m_Center.Contains(e.m_vPosition))
      {
        m_fCenterScroll = plMath::Clamp(m_fCenterScroll - e.m_fWheel * 40.0f, 0.0f, plMath::Max(layout.m_fCenterHeight - layout.m_Center.height, 0.0f));
        m_bRebuild = true;
        RequestRedraw();
      }
      else if (layout.m_Inspector.Contains(e.m_vPosition))
      {
        m_fInspectorScroll = plMath::Max(m_fInspectorScroll - e.m_fWheel * 40.0f, 0.0f);
        m_bRebuild = true;
        RequestRedraw();
      }
      return;
    }

    case Type::DoubleClick:
    {
      if (e.m_Button != Button::Left)
        return;

      if (const plUInt32 uiState = IndexAfter(e.m_sElement, "Mu.Lane."); uiState != plInvalidIndex)
        BeginRename(uiState);
      return;
    }

    case Type::Press:
    {
      if (e.m_Button != Button::Left || !e.m_sElement.IsEmpty())
        return;

      if (Item slider; FindSlider(e.m_vPosition, slider))
      {
        m_sSlider = slider.m_sId;
        m_fSliderValue = plAudioEngineUi::SliderFractionAt(slider.m_Rect, e.m_vPosition.x);
        RequestRedraw();
      }
      return;
    }

    case Type::Move:
    case Type::DragMove:
    case Type::DragStart:
    {
      // a slider follows the pointer while its button is held
      if (!m_sSlider.IsEmpty())
      {
        Layout layout;
        ComputeLayout(layout);

        plHybridArray<Item, 32> items;
        float fHeight = 0.0f;
        ComputeInspector(layout, items, fHeight);

        for (const Item& item : items)
        {
          if (item.m_sId == m_sSlider)
            m_fSliderValue = plAudioEngineUi::SliderFractionAt(item.m_Rect, e.m_vPosition.x);
        }

        RequestRedraw();
        return;
      }

      if (e.m_Type == Type::DragStart && e.m_Button == Button::Left)
      {
        const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();

        if (const plUInt32 uiSlot = IndexAfter(e.m_sDragElement, "Mu.ClipRow."); uiSlot != plInvalidIndex)
        {
          plHybridArray<const Clip*, 64> clips;
          GetVisibleClips(clips);
          if (m_uiClipScroll + uiSlot < clips.GetCount())
          {
            m_DragKind = DragKind::Clip;
            m_sDragClip = clips[m_uiClipScroll + uiSlot]->m_sGuid;
            m_sDragLabel = clips[m_uiClipScroll + uiSlot]->m_sName;
          }
        }
        else if (const plUInt32 uiBlock = IndexAfter(e.m_sDragElement, "Mu.Seg."); uiBlock != plInvalidIndex)
        {
          Layout layout;
          ComputeLayout(layout);
          if (uiBlock < layout.m_Blocks.GetCount())
          {
            const Block& block = layout.m_Blocks[uiBlock];
            m_DragKind = DragKind::Segment;
            m_uiDragLane = block.m_uiLane;
            m_uiDragPosition = block.m_uiPosition;
            m_uiDragSegment = block.m_uiSegment;
            m_sDragLabel = pProp->m_Segments[block.m_uiSegment].m_sName;
          }
        }
        else if (const plUInt32 uiRow = IndexAfter(e.m_sDragElement, "Mu.Item."); uiRow != plInvalidIndex && m_Selection == SelectionKind::State)
        {
          if (uiRow < pProp->m_States[m_uiSelected].m_Segments.GetCount())
          {
            m_DragKind = DragKind::Row;
            m_uiDragPosition = uiRow;
            m_sDragLabel = pProp->m_States[m_uiSelected].m_Segments[uiRow];
          }
        }
      }

      if (m_DragKind != DragKind::None && e.m_Type != Type::Move)
      {
        m_vDragPos = e.m_vPosition;
        m_DropTarget = FindDropTarget(e.m_vPosition, m_DragKind);
        RequestRedraw();
      }
      return;
    }

    case Type::Release:
    {
      if (!m_sSlider.IsEmpty())
        CommitSlider();
      return;
    }

    case Type::Drop:
    {
      if (!m_sSlider.IsEmpty())
      {
        CommitSlider();
        return;
      }

      if (m_DragKind != DragKind::None)
      {
        const DropTarget target = FindDropTarget(e.m_vPosition, m_DragKind);
        const DragKind kind = m_DragKind;
        const plString sClip = m_sDragClip;
        endDrag();
        ApplyDrop(target, kind, sClip);
      }
      return;
    }

    case Type::DragCancel:
      m_sSlider.Clear();
      endDrag();
      return;

    default:
      return;
  }
}

void plEditorUiAudioMusicView::OnDrop(const plEditorUiDropEvent& e)
{
  if (e.m_Type == plEditorUiDropEvent::Type::Leave || e.m_pPayload == nullptr)
  {
    if (m_DragKind == DragKind::Clip)
    {
      m_DragKind = DragKind::None;
      m_DropTarget = DropTarget();
      RequestRedraw();
    }
    return;
  }

  const plString sClip = DraggedClip(*e.m_pPayload);
  if (sClip.IsEmpty())
  {
    e.m_sReason = "Only Audio Clips drop here.";
    return;
  }

  const DropTarget target = FindDropTarget(e.m_vPosition, DragKind::Clip);
  e.m_bAccepted = target.m_Kind != DropTarget::Kind::None;

  if (e.m_Type == plEditorUiDropEvent::Type::Move)
  {
    m_DragKind = DragKind::Clip;
    m_sDragClip = sClip;
    m_sDragLabel = plAudioMusicEdit::GetClipName(sClip);
    m_vDragPos = e.m_vPosition;
    m_DropTarget = target;
    RequestRedraw();
    return;
  }

  m_DragKind = DragKind::None;
  m_DropTarget = DropTarget();
  RequestRedraw();

  if (e.m_bAccepted)
    ApplyDrop(target, DragKind::Clip, sClip);
}

//////////////////////////////////////////////////////////////////////////
// Audition

void plEditorUiAudioMusicView::SetAuditioning(bool bAuditioning)
{
  if (bAuditioning && !m_bAuditioning)
  {
    m_uiAnchorSegment = 0xFFFF;
    m_uiLastBeatIndex = 0xFFFFFFFF;
  }

  m_bAuditioning = bAuditioning;
  RequestRedraw();
}

void plEditorUiAudioMusicView::SetStatus(const Status& status)
{
  const double fNow = Now();
  m_Status = status;

  if (!status.m_bPlaying || status.m_uiSegment >= m_pDocument->GetProperties()->m_Segments.GetCount() || status.m_uiBar == 0 || status.m_uiBeat == 0)
  {
    m_uiAnchorSegment = 0xFFFF;
    m_uiLastBeatIndex = 0xFFFFFFFF;
    RequestRedraw();
    return;
  }

  const plUInt32 uiBeatsPerBar = plMath::Max(status.m_uiBeatsPerBar, 1u);
  const plUInt32 uiBeatIndex = (status.m_uiBar - 1) * uiBeatsPerBar + (status.m_uiBeat - 1);
  const double fBeatSeconds = 60.0 / plMath::Max(static_cast<double>(status.m_fTempo), 1.0);
  const double fEstimate = fNow - uiBeatIndex * fBeatSeconds;

  // a new segment (or the same one again, which counts its beats from 0 again) anchors at its beat 0; every report
  // comes after its beat began, so the earliest estimate is the closest
  const bool bNew = status.m_uiSegment != m_uiAnchorSegment || status.m_bTransitionSegment != m_bAnchorTransition || uiBeatIndex < m_uiLastBeatIndex;
  if (bNew || fEstimate < m_fSegmentAnchor)
    m_fSegmentAnchor = fEstimate;

  m_uiAnchorSegment = status.m_uiSegment;
  m_bAnchorTransition = status.m_bTransitionSegment;
  m_uiLastBeatIndex = uiBeatIndex;
  RequestRedraw();
}

void plEditorUiAudioMusicView::NoteStingerRequest(plUInt32 uiStinger)
{
  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  if (uiStinger >= pProp->m_Stingers.GetCount())
    return;

  plStringBuilder sNote;
  sNote.SetFormat("Stinger {} on its {}", pProp->m_Stingers[uiStinger].m_sName, s_szQuantize[plMath::Min<plUInt32>(pProp->m_Stingers[uiStinger].m_eAt.GetValue(), 4)]);
  ShowToast(sNote);
}

void plEditorUiAudioMusicView::AddMessage(plStringView sText, MessageKind kind)
{
  if (m_Messages.GetCount() >= s_uiMaxMessages)
    m_Messages.RemoveAtAndCopy(0);

  const bool bFirst = m_Messages.IsEmpty();

  Message& message = m_Messages.ExpandAndGetRef();
  message.m_sText = sText;
  message.m_Kind = kind;

  if (kind == MessageKind::Stinger)
    message.m_sText = plStringBuilder("stinger ", sText);

  if (m_Status.m_bPlaying && m_Status.m_uiBar > 0)
  {
    plStringBuilder sPosition;
    sPosition.SetFormat("{}.{}", m_Status.m_uiBar, m_Status.m_uiBeat);
    message.m_sPosition = sPosition;
  }

  // the messages take room under the inspector
  if (bFirst)
    m_bRebuild = true;

  RequestRedraw();
}

void plEditorUiAudioMusicView::ClearMessages()
{
  m_Messages.Clear();
  m_bRebuild = true;
  RequestRedraw();
}

//////////////////////////////////////////////////////////////////////////
// Drawing

void plEditorUiAudioMusicView::Draw()
{
  using namespace plEditorUi;

  m_Writer.Clear();

  const plVec2 size = GetSurface()->GetLogicalSize();
  Layout layout;
  ComputeLayout(layout);

  m_Writer.Rect(plRectFloat(0.0f, 0.0f, size.x, size.y), Background());

  DrawToolbar();
  DrawClips(layout);
  DrawLanes(layout);
  DrawMatrix(layout);
  DrawInspector(layout);

  if (layout.m_Messages.height > 0.0f)
    DrawMessages(layout.m_Messages);

  DrawStatus(plRectFloat(0.0f, size.y - s_fStatusRow, size.x, s_fStatusRow));

  // the toast over the lanes' bottom
  if (Now() - m_fToastTime < s_fToastSeconds && !m_sToast.IsEmpty())
  {
    const float fWidth = MeasureCodeText(m_sToast, MinFontSize + 1) + 24.0f;
    const plRectFloat rect(layout.m_Center.x + (layout.m_Center.width - fWidth) * 0.5f, size.y - s_fStatusRow - 40.0f, fWidth, 26.0f);
    m_Writer.Rect(rect, Raised(), 4.0f);
    m_Writer.RectOutline(rect, Border(), 1.0f, 4.0f);
    m_Writer.Text(rect, m_sToast, Foreground(), MinFontSize + 1, plUiTextHAlign::Center, plUiTextVAlign::Center, true);
  }

  DrawDrag();

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sDrawingEntry);
}

void plEditorUiAudioMusicView::DrawToolbar()
{
  using namespace plEditorUi;

  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  const plVec2 size = GetSurface()->GetLogicalSize();
  const ToolbarLayout toolbar = LayoutToolbar(*pProp, size.x);

  m_Writer.Rect(plRectFloat(0.0f, 0.0f, size.x, s_fToolRow), Surface());
  m_Writer.Rect(plRectFloat(0.0f, s_fToolRow - 1.0f, size.x, 1.0f), Line());

  // Play and Stop with their glyphs
  const bool bCanPlay = m_sBuildError.IsEmpty();
  plAudioEngineUi::TextButton(m_Writer, toolbar.m_Play, "     Play", {}, bCanPlay);
  {
    const plVec2 c(toolbar.m_Play.x + 14.0f, toolbar.m_Play.y + toolbar.m_Play.height * 0.5f);
    const plVec2 triangle[] = {plVec2(c.x - 4.0f, c.y - 5.0f), plVec2(c.x + 5.0f, c.y), plVec2(c.x - 4.0f, c.y + 5.0f)};
    m_Writer.Triangles(triangle, m_bAuditioning ? Accent() : (bCanPlay ? Foreground() : Dim()));
  }

  plAudioEngineUi::TextButton(m_Writer, toolbar.m_Stop, "     Stop", {}, m_bAuditioning);
  m_Writer.Rect(plRectFloat(toolbar.m_Stop.x + 10.0f, toolbar.m_Stop.y + toolbar.m_Stop.height * 0.5f - 4.5f, 9.0f, 9.0f), m_bAuditioning ? Foreground() : Dim(), 1.0f);

  m_Writer.Text(plRectFloat(toolbar.m_fStateLabel, 0.0f, 60.0f, s_fToolRow), "State", Dim(), MinFontSize + 1, plUiTextHAlign::Left, plUiTextVAlign::Center, true);

  // the playing state is lit, a state asked for and not reached yet is amber
  const bool bPlaying = m_Status.m_bPlaying;
  for (plUInt32 i = 0; i < toolbar.m_States.GetCount(); ++i)
  {
    const plRectFloat& rect = toolbar.m_States[i];
    plAudioEngineUi::TextButton(m_Writer, rect, pProp->m_States[i].m_sName);

    if (bPlaying && i == m_Status.m_uiState && !m_Status.m_bTransitionSegment)
      m_Writer.RectOutline(rect, Accent(), 1.5f, 4.0f);
    else if (bPlaying && i == m_Status.m_uiPendingState)
      m_Writer.RectOutline(rect, Warning(), 1.5f, 4.0f);
  }

  if (!toolbar.m_Stingers.IsEmpty())
    m_Writer.Text(plRectFloat(toolbar.m_fStingerLabel, 0.0f, 70.0f, s_fToolRow), "Stinger", Dim(), MinFontSize + 1, plUiTextHAlign::Left, plUiTextVAlign::Center, true);

  for (plUInt32 i = 0; i < toolbar.m_Stingers.GetCount(); ++i)
  {
    plAudioEngineUi::TextButton(m_Writer, toolbar.m_Stingers[i], pProp->m_Stingers[i].m_sName);
    if (m_Selection == SelectionKind::Stinger && m_uiSelected == i)
      m_Writer.RectOutline(toolbar.m_Stingers[i], Accent(), 1.0f, 4.0f);
  }

  // the add buttons: dashed, accent text
  auto addButton = [&](const plRectFloat& rect, plStringView sText)
  {
    plAudioEngineUi::DashedRect(m_Writer, rect, Border());
    m_Writer.Text(rect, sText, Accent(), FontSize - 1, plUiTextHAlign::Center, plUiTextVAlign::Center);
  };
  addButton(toolbar.m_AddState, "+ State");
  addButton(toolbar.m_AddSegment, "+ Segment");
  addButton(toolbar.m_AddStinger, "+ Stinger");

  // beat lights and the position
  const plUInt32 uiLights = bPlaying ? plMath::Clamp(m_Status.m_uiBeatsPerBar, 1u, 12u) : 4u;
  for (plUInt32 i = 0; i < uiLights; ++i)
  {
    const bool bOn = bPlaying && i + 1 == m_Status.m_uiBeat;
    m_Writer.Rect(plRectFloat(toolbar.m_fBeats + i * 14.0f, s_fToolRow * 0.5f - 5.0f, 10.0f, 10.0f), bOn ? Accent() : Raised(), 2.0f);
  }

  plStringBuilder sPosition("-");
  if (bPlaying && m_Status.m_uiBar > 0)
    sPosition.SetFormat("bar {} \xC2\xB7 beat {}", m_Status.m_uiBar, m_Status.m_uiBeat);
  m_Writer.Text(plRectFloat(size.x - s_fPad - 92.0f, 0.0f, 92.0f, s_fToolRow), sPosition, Muted(), MinFontSize + 1, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
}

void plEditorUiAudioMusicView::DrawClips(const Layout& layout)
{
  using namespace plEditorUi;

  const plRectFloat& area = layout.m_Clips;
  m_Writer.Rect(area, Surface());
  m_Writer.Rect(plRectFloat(area.x + area.width - 1.0f, area.y, 1.0f, area.height), Line());

  m_Writer.Text(plRectFloat(area.x + s_fPad, area.y, 60.0f, s_fSectionHead), "CLIPS", Muted(), MinFontSize);
  m_Writer.Text(plRectFloat(area.x + s_fPad + 42.0f, area.y, area.width - 52.0f, s_fSectionHead), "drag onto a lane", Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);

  plHybridArray<const Clip*, 64> clips;
  GetVisibleClips(clips);

  const plRectFloat& list = layout.m_ClipList;
  if (clips.IsEmpty())
    m_Writer.Text(plRectFloat(area.x + s_fPad, list.y + 4.0f, area.width - 20.0f, 18.0f), m_Clips.IsEmpty() ? "The project has no Audio Clips." : "No clip matches.", Dim(), FontSize - 2);

  const plUInt32 uiRows = static_cast<plUInt32>(list.height / s_fClipRow);
  const plColorGammaUB wave = plAudioEngineUi::Mix(Surface(), plAudioEngineUi::AudioColor(), 0.7f);

  for (plUInt32 k = 0; k < uiRows && m_uiClipScroll + k < clips.GetCount(); ++k)
  {
    const Clip& clip = *clips[m_uiClipScroll + k];
    const plRectFloat row(area.x + 6.0f, list.y + k * s_fClipRow, area.width - 12.0f, s_fClipRow - 2.0f);

    if (m_DragKind == DragKind::Clip && m_sDragClip == clip.m_sGuid)
      m_Writer.Rect(row, Raised(), 4.0f);

    // a little waveform glyph
    for (plUInt32 i = 0; i < 7; ++i)
      m_Writer.Rect(plRectFloat(row.x + 8.0f + i * 4.0f, row.y + row.height * 0.5f - 7.0f, 2.0f, 14.0f), wave, 1.0f);

    plStringBuilder sSeconds;
    sSeconds.SetFormat("{}s", plArgF(clip.m_fSeconds, 2));
    const float fSecondsWidth = MeasureCodeText(sSeconds, MinFontSize) + 4.0f;

    m_Writer.PushClip(plRectFloat(row.x + 42.0f, row.y, row.width - 46.0f - fSecondsWidth, row.height));
    m_Writer.Text(plRectFloat(row.x + 42.0f, row.y, row.width - 46.0f - fSecondsWidth, row.height), clip.m_sName, Foreground(), FontSize - 1);
    m_Writer.PopClip();
    m_Writer.Text(plRectFloat(row.x + row.width - fSecondsWidth - 4.0f, row.y, fSecondsWidth, row.height), sSeconds, Dim(), MinFontSize, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
  }

  m_Writer.Text(plRectFloat(area.x + s_fPad, area.y + area.height - s_fClipHint, area.width - 20.0f, s_fClipHint), "Lengths come from the clips' asset info.", Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
}

void plEditorUiAudioMusicView::DrawBlock(const Block& block, bool bSelected, bool bPlaying)
{
  using namespace plEditorUi;

  const plAudioMusicSegment& segment = m_pDocument->GetProperties()->m_Segments[block.m_uiSegment];
  const plRectFloat& rect = block.m_Rect;
  const plColorGammaUB audio = plAudioEngineUi::AudioColor();
  const plColorGammaUB fill = AudioWash(bPlaying ? 0.32f : 0.18f);

  m_Writer.Rect(rect, fill, 3.0f);

  // the bars inside
  const plUInt32 uiBars = plMath::Max(segment.m_uiBars, 1u);
  const float fBar = rect.width / uiBars;
  if (fBar >= 4.0f)
  {
    for (plUInt32 uiBar = 1; uiBar < uiBars; ++uiBar)
      m_Writer.Rect(plRectFloat(rect.x + uiBar * fBar, rect.y + 3.0f, 1.0f, rect.height - 6.0f), AudioWash(0.3f));
  }

  m_Writer.PushClip(rect);
  m_Writer.Text(plRectFloat(rect.x + 6.0f, rect.y + 3.0f, rect.width - 10.0f, 16.0f), segment.m_sName, Foreground(), FontSize - 2);

  plStringBuilder sMeta;
  sMeta.SetFormat("{} bars \xC2\xB7 {} \xC2\xB7 {}", segment.m_uiBars, plArgF(segment.m_fTempo, 0), MeterName(segment.m_uiBeatsPerBar));
  m_Writer.Text(plRectFloat(rect.x + 6.0f, rect.y + 18.0f, rect.width - 10.0f, 14.0f), sMeta, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);

  // a line per stem; one that follows Intensity fades in
  for (plUInt32 i = 0; i < segment.m_Stems.GetCount() && i < 3; ++i)
  {
    const plRectFloat line(rect.x + 6.0f, rect.y + 35.0f + i * 5.0f, rect.width - 12.0f, 3.0f);
    if (segment.m_Stems[i].m_bFollowIntensity)
      m_Writer.HorizontalGradient(line, AudioWash(0.3f), AudioWash(0.85f));
    else
      m_Writer.Rect(line, AudioWash(0.65f), 1.5f);
  }
  m_Writer.PopClip();

  if (bSelected)
    m_Writer.RectOutline(rect, Accent(), 2.0f, 3.0f);
  else
    m_Writer.RectOutline(rect, plAudioEngineUi::Mix(Background(), audio, bPlaying ? 0.95f : 0.65f), 1.0f, 3.0f);
}

void plEditorUiAudioMusicView::DrawLanes(const Layout& layout)
{
  using namespace plEditorUi;

  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  const plRectFloat& area = layout.m_Center;
  const plUInt32 uiLoose = GetLooseLane();
  const plUInt32 uiInitial = GetInitialState();

  m_Writer.PushClip(area);

  const float fHeadY = area.y - m_fCenterScroll;
  m_Writer.Text(plRectFloat(area.x + s_fPad, fHeadY, 60.0f, s_fSectionHead), "STATES", Muted(), MinFontSize);
  m_Writer.Text(plRectFloat(area.x + s_fPad + 52.0f, fHeadY, 400.0f, s_fSectionHead), "segments play left to right, then again", Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);

  // which block plays: the playing segment on its state's lane, a transition segment on the loose lane
  plUInt32 uiPlayingBlock = plInvalidIndex;
  if (m_Status.m_bPlaying && m_uiAnchorSegment != 0xFFFF)
  {
    const plUInt32 uiLane = m_Status.m_bTransitionSegment ? uiLoose : m_Status.m_uiState;
    for (plUInt32 b = 0; b < layout.m_Blocks.GetCount() && uiPlayingBlock == plInvalidIndex; ++b)
    {
      if (layout.m_Blocks[b].m_uiSegment == m_Status.m_uiSegment && layout.m_Blocks[b].m_uiLane == uiLane)
        uiPlayingBlock = b;
    }

    // a transition segment that is also in a state
    for (plUInt32 b = 0; b < layout.m_Blocks.GetCount() && uiPlayingBlock == plInvalidIndex; ++b)
    {
      if (layout.m_Blocks[b].m_uiSegment == m_Status.m_uiSegment)
        uiPlayingBlock = b;
    }
  }

  for (plUInt32 uiLane = 0; uiLane < layout.m_Lanes.GetCount(); ++uiLane)
  {
    const Lane& lane = layout.m_Lanes[uiLane];
    const bool bLoose = uiLane == uiLoose;
    const bool bTarget = m_DropTarget.m_Kind == DropTarget::Kind::Lane && m_DropTarget.m_uiLane == uiLane;

    // header
    m_Writer.Rect(lane.m_Header, Plate(), 4.0f);
    if (!bLoose)
    {
      const plAudioMusicState& state = pProp->m_States[uiLane];

      if (m_Selection == SelectionKind::State && m_uiSelected == uiLane)
        m_Writer.RectOutline(lane.m_Header, Accent(), 1.0f, 4.0f);
      else if (m_Status.m_bPlaying && m_Status.m_uiPendingState == uiLane)
        m_Writer.RectOutline(lane.m_Header, Warning(), 1.0f, 4.0f);

      if (uiLane != m_uiRenameState)
      {
        // the initial state's name leaves room for its chip
        const float fNameRoom = lane.m_Header.width - (uiLane == uiInitial ? 54.0f : 12.0f);
        const float fNameWidth = plMath::Min(MeasureText(state.m_sName, FontSize - 1), fNameRoom);
        m_Writer.PushClip(plRectFloat(lane.m_Header.x + 8.0f, lane.m_Header.y, fNameRoom, 30.0f));
        m_Writer.Text(plRectFloat(lane.m_Header.x + 8.0f, lane.m_Header.y + 6.0f, lane.m_Header.width - 12.0f, 18.0f), state.m_sName, Foreground(), FontSize - 1);
        m_Writer.PopClip();

        if (uiLane == uiInitial)
        {
          const plRectFloat chip(lane.m_Header.x + 8.0f + fNameWidth + 6.0f, lane.m_Header.y + 8.0f, MeasureCodeText("start", MinFontSize) + 8.0f, 14.0f);
          m_Writer.RectOutline(chip, plAudioEngineUi::Mix(Plate(), Accent(), 0.5f), 1.0f, 3.0f);
          m_Writer.Text(chip, "start", Accent(), MinFontSize, plUiTextHAlign::Center, plUiTextVAlign::Center, true);
        }
      }

      double fSeconds = 0.0;
      plUInt32 uiCount = 0;
      for (const Block& block : layout.m_Blocks)
      {
        if (block.m_uiLane == uiLane)
        {
          fSeconds += GetSegmentSeconds(block.m_uiSegment);
          ++uiCount;
        }
      }

      plStringBuilder sSub;
      sSub.SetFormat("{} seg \xC2\xB7 {} s", uiCount, plArgF(fSeconds, 1));
      m_Writer.Text(plRectFloat(lane.m_Header.x + 8.0f, lane.m_Header.y + 28.0f, lane.m_Header.width - 12.0f, 14.0f), sSub, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    }
    else
    {
      m_Writer.Text(plRectFloat(lane.m_Header.x + 8.0f, lane.m_Header.y + 6.0f, lane.m_Header.width - 12.0f, 18.0f), "Not in a state", Muted(), FontSize - 1);
      m_Writer.Text(plRectFloat(lane.m_Header.x + 8.0f, lane.m_Header.y + 28.0f, lane.m_Header.width - 12.0f, 14.0f), "transition segments", Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    }

    // track: the loose lane striped
    m_Writer.Rect(lane.m_Track, Surface(), 4.0f);
    if (bLoose)
    {
      m_Writer.PushClip(lane.m_Track);
      const plColorGammaUB stripe = plAudioEngineUi::Mix(Surface(), Background(), 0.6f);
      for (float x = lane.m_Track.x - lane.m_Track.height; x < lane.m_Track.x + lane.m_Track.width; x += 16.0f)
        m_Writer.Line(plVec2(x, lane.m_Track.y + lane.m_Track.height), plVec2(x + lane.m_Track.height, lane.m_Track.y), stripe, 6.0f);
      m_Writer.PopClip();
    }

    if (bTarget)
    {
      m_Writer.Rect(lane.m_Track, plAudioEngineUi::Mix(Surface(), Accent(), 0.14f), 4.0f);
      plAudioEngineUi::DashedRect(m_Writer, lane.m_Track, Accent());
    }

    // the blocks, then the drop space after them
    float fEnd = lane.m_Track.x + 3.0f;
    for (plUInt32 b = 0; b < layout.m_Blocks.GetCount(); ++b)
    {
      const Block& block = layout.m_Blocks[b];
      if (block.m_uiLane != uiLane)
        continue;

      DrawBlock(block, m_Selection == SelectionKind::Segment && m_uiSelected == block.m_uiSegment, b == uiPlayingBlock);
      fEnd = block.m_Rect.x + block.m_Rect.width + s_fBlockGap;

      // the dragged block stays behind dimmed; a clip over a segment marks it
      if (m_DragKind == DragKind::Segment && block.m_uiLane == m_uiDragLane && block.m_uiPosition == m_uiDragPosition)
        m_Writer.Rect(block.m_Rect, plAudioEngineUi::WithAlpha(Background(), 150), 3.0f);

      if (m_DropTarget.m_Kind == DropTarget::Kind::Segment && m_DropTarget.m_uiSegment == block.m_uiSegment)
        plAudioEngineUi::DashedRect(m_Writer, block.m_Rect, Accent());
    }

    const float fSpace = lane.m_Track.x + lane.m_Track.width - fEnd;
    if (fSpace >= 60.0f)
    {
      const plStringView sDrop = bLoose ? plStringView("drop a clip for a transition segment") : plStringView("drop a clip");
      m_Writer.Text(plRectFloat(fEnd, lane.m_Track.y, fSpace, lane.m_Track.height), sDrop, bTarget ? Accent() : Dim(), MinFontSize, plUiTextHAlign::Center, plUiTextVAlign::Center, true);
    }

    // where a dragged segment goes in
    if (bTarget && m_DragKind == DragKind::Segment && !bLoose)
      m_Writer.Rect(plRectFloat(m_DropTarget.m_fCaretX - 1.0f, lane.m_Track.y + 2.0f, 2.0f, lane.m_Track.height - 4.0f), Accent());
  }

  // the playhead in the playing block
  if (uiPlayingBlock != plInvalidIndex)
  {
    const Block& block = layout.m_Blocks[uiPlayingBlock];
    const double fFraction = plMath::Clamp((Now() - m_fSegmentAnchor) / GetSegmentSeconds(block.m_uiSegment), 0.0, 1.0);
    const float x = block.m_Rect.x + static_cast<float>(fFraction) * block.m_Rect.width;
    m_Writer.Rect(plRectFloat(x - 1.0f, block.m_Rect.y - 3.0f, 2.0f, block.m_Rect.height + 6.0f), Accent());

    // the exit cues of the playing segment
    const plAudioMusicSegment& segment = pProp->m_Segments[block.m_uiSegment];
    for (plUInt32 uiCue : segment.m_ExitCues)
    {
      if (uiCue == 0 || uiCue >= segment.m_uiBars)
        continue;

      const float fCue = block.m_Rect.x + block.m_Rect.width * uiCue / segment.m_uiBars;
      const plVec2 cue[] = {plVec2(fCue - 4.0f, block.m_Rect.y - 6.0f), plVec2(fCue + 4.0f, block.m_Rect.y - 6.0f), plVec2(fCue, block.m_Rect.y)};
      m_Writer.Triangles(cue, Warning());
    }
  }

  m_Writer.PopClip();
}

void plEditorUiAudioMusicView::DrawMatrix(const Layout& layout)
{
  using namespace plEditorUi;

  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();
  const plRectFloat& area = layout.m_Center;
  const float x0 = area.x + 8.0f;
  const float y = layout.m_fMatrixTop;

  m_Writer.PushClip(area);
  m_Writer.Rect(plRectFloat(area.x, y - 4.0f, area.width, 1.0f), Line());

  m_Writer.Text(plRectFloat(x0 + 2.0f, y, 100.0f, s_fSectionHead), "TRANSITIONS", Muted(), MinFontSize);
  m_Writer.Text(plRectFloat(x0 + 92.0f, y, 300.0f, s_fSectionHead), "from row to column", Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);

  const plUInt32 uiStates = pProp->m_States.GetCount();
  if (uiStates == 0)
  {
    m_Writer.Text(plRectFloat(x0 + 2.0f, y + s_fSectionHead, area.width - 20.0f, 18.0f), "Add a state to set how the music changes between states.", Dim(), FontSize - 2);
    m_Writer.PopClip();
    return;
  }

  // the column and row headers
  const float fHeaderY = y + s_fSectionHead;
  m_Writer.Text(plRectFloat(x0, fHeaderY, layout.m_fRowLabelWidth - 10.0f, s_fMatrixHeader), "FROM \\ TO", Dim(), MinFontSize, plUiTextHAlign::Right, plUiTextVAlign::Center, true);

  auto upper = [](plStringView sText) -> plString
  {
    plStringBuilder s = sText;
    s.ToUpper();
    return s;
  };

  for (plUInt32 c = 0; c < uiStates; ++c)
  {
    const float x = x0 + layout.m_fRowLabelWidth + c * (layout.m_fColumnWidth + s_fCellGap);
    m_Writer.PushClip(plRectFloat(x, fHeaderY, layout.m_fColumnWidth, s_fMatrixHeader));
    m_Writer.Text(plRectFloat(x + 6.0f, fHeaderY, layout.m_fColumnWidth - 8.0f, s_fMatrixHeader), upper(pProp->m_States[c].m_sName), Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    m_Writer.PopClip();
  }

  for (plUInt32 r = 0; r <= uiStates; ++r)
  {
    const float fRowY = fHeaderY + s_fMatrixHeader + r * (s_fMatrixRow + s_fCellGap);
    const plString sRow = r == 0 ? plString("ANY STATE") : upper(pProp->m_States[r - 1].m_sName);
    m_Writer.Text(plRectFloat(x0, fRowY, layout.m_fRowLabelWidth - 10.0f, s_fMatrixRow), sRow, Dim(), MinFontSize, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
  }

  for (const Cell& cell : layout.m_Cells)
  {
    const plRectFloat& rect = cell.m_Rect;

    if (cell.m_uiFrom == cell.m_uiTo)
    {
      m_Writer.Text(plRectFloat(rect.x + 8.0f, rect.y, rect.width - 10.0f, 20.0f), "-", Dim(), FontSize - 2);
      continue;
    }

    const bool bSelected = m_Selection == SelectionKind::Transition && m_uiSelected == cell.m_uiFrom && m_uiSelectedTo == cell.m_uiTo;
    const bool bLit = m_Status.m_bPlaying && (cell.m_uiFrom == m_Status.m_uiState || cell.m_uiFrom == plInvalidIndex) && !m_Status.m_bTransitionSegment;
    m_Writer.Rect(rect, bLit ? plAudioEngineUi::Mix(Plate(), Accent(), 0.1f) : Plate(), 3.0f);
    if (bSelected)
      m_Writer.RectOutline(rect, Accent(), 1.0f, 3.0f);

    m_Writer.PushClip(rect);

    const plUInt32 uiTransition = plAudioMusicEdit::FindTransition(m_pDocument, cell.m_uiFrom, cell.m_uiTo);
    if (uiTransition == plInvalidIndex)
    {
      const plStringView sDefault = cell.m_uiFrom == plInvalidIndex ? plStringView("none") : plStringView("next bar, cut");
      m_Writer.Text(plRectFloat(rect.x + 8.0f, rect.y + 4.0f, rect.width - 10.0f, 16.0f), sDefault, Dim(), FontSize - 2);

      const float fSet = rect.x + 8.0f + MeasureText(sDefault, FontSize - 2) + 8.0f;
      m_Writer.Text(plRectFloat(fSet, rect.y + 4.0f, 60.0f, 16.0f), "+ set", Accent(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);

      if (cell.m_uiFrom != plInvalidIndex)
        m_Writer.Text(plRectFloat(rect.x + 8.0f, rect.y + 21.0f, rect.width - 10.0f, 14.0f), "default", Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    }
    else
    {
      const plAudioMusicTransition& transition = pProp->m_Transitions[uiTransition];

      plStringBuilder sMain = s_szQuantize[plMath::Min<plUInt32>(transition.m_eChangeAt.GetValue(), 4)];
      if (transition.m_fFadeBeats > 0.0f)
        sMain.AppendFormat(", fade {} beats", plArgF(transition.m_fFadeBeats, 1));
      else
        sMain.Append(", cut");

      plStringBuilder sSub = transition.m_eStartAt == plAudioMusicStartAt::SamePosition ? "same position" : "segment start";
      if (!transition.m_sVia.IsEmpty())
        sSub.AppendFormat(" \xC2\xB7 via {}", transition.m_sVia);

      m_Writer.Text(plRectFloat(rect.x + 8.0f, rect.y + 4.0f, rect.width - 10.0f, 16.0f), sMain, Foreground(), FontSize - 2);
      m_Writer.Text(plRectFloat(rect.x + 8.0f, rect.y + 21.0f, rect.width - 10.0f, 14.0f), sSub, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    }

    m_Writer.PopClip();
  }

  m_Writer.PopClip();
}

void plEditorUiAudioMusicView::DrawInspector(const Layout& layout)
{
  using namespace plEditorUi;

  const plRectFloat area(layout.m_Inspector.x, layout.m_Inspector.y, layout.m_Inspector.width, layout.m_Inspector.height + layout.m_Messages.height);
  m_Writer.Rect(area, Surface());
  m_Writer.Rect(plRectFloat(area.x, area.y, 1.0f, area.height), Line());

  plHybridArray<Item, 32> items;
  float fHeight = 0.0f;
  ComputeInspector(layout, items, fHeight);

  m_Writer.PushClip(layout.m_Inspector);
  for (const Item& item : items)
    DrawItem(item);
  m_Writer.PopClip();
}

void plEditorUiAudioMusicView::DrawItem(const Item& item)
{
  using namespace plEditorUi;

  const plRectFloat& rect = item.m_Rect;

  switch (item.m_Type)
  {
    case ItemType::Header:
    {
      plStringBuilder sUpper = item.m_sText;
      sUpper.ToUpper();
      m_Writer.Text(rect, sUpper, Muted(), MinFontSize);

      if (!item.m_sSub.IsEmpty())
      {
        const float x = rect.x + MeasureText(sUpper, MinFontSize) + 8.0f;
        m_Writer.Text(plRectFloat(x, rect.y, rect.x + rect.width - x, rect.height), item.m_sSub, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
      }
      break;
    }

    case ItemType::Label:
    {
      if (!item.m_sText.IsEmpty())
        m_Writer.Text(rect, item.m_sText, item.m_bOn ? Foreground() : Muted(), FontSize - 2);
      if (!item.m_sSub.IsEmpty())
        m_Writer.Text(rect, item.m_sSub, Foreground(), MinFontSize, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
      break;
    }

    case ItemType::Text:
    {
      const plColorGammaUB color = item.m_Tone == ToneWarning ? Warning() : (item.m_Tone == ToneGood ? Accent() : Dim());

      plHybridArray<plStringView, 8> lines;
      item.m_sText.GetView().Split(true, lines, "\n");
      for (plUInt32 i = 0; i < lines.GetCount(); ++i)
        m_Writer.Text(plRectFloat(rect.x, rect.y + i * s_fTextLine, rect.width, s_fTextLine), lines[i], color, MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
      break;
    }

    case ItemType::Toggle:
    {
      const plRectFloat box(rect.x, rect.y + (rect.height - 14.0f) * 0.5f, 14.0f, 14.0f);
      m_Writer.Rect(box, item.m_bOn ? Accent() : Field(), 3.0f);
      m_Writer.RectOutline(box, item.m_bOn ? Accent() : Border(), 1.0f, 3.0f);

      if (item.m_bOn)
      {
        const plVec2 check[] = {plVec2(box.x + 3.0f, box.y + 7.0f), plVec2(box.x + 6.0f, box.y + 10.0f), plVec2(box.x + 11.0f, box.y + 4.0f)};
        m_Writer.Polyline(check, Background(), 2.0f);
      }

      m_Writer.Text(plRectFloat(box.x + 22.0f, rect.y, rect.width - 22.0f, rect.height), item.m_sSub, Dim(), FontSize - 2);
      break;
    }

    case ItemType::Option:
    {
      m_Writer.Rect(rect, item.m_bOn ? plAudioEngineUi::Mix(Surface(), Accent(), 0.16f) : Plate());
      m_Writer.RectOutline(rect, Border(), 1.0f);
      m_Writer.Text(rect, item.m_sText, item.m_bOn ? Foreground() : Muted(), FontSize - 2, plUiTextHAlign::Center, plUiTextVAlign::Center);
      break;
    }

    case ItemType::Chip:
    {
      const plColorGammaUB color = item.m_bOn ? Warning() : Dim();
      m_Writer.RectOutline(rect, item.m_bOn ? plAudioEngineUi::Mix(Surface(), Warning(), 0.6f) : Border(), 1.0f, 3.0f);
      m_Writer.Text(rect, item.m_sText, color, MinFontSize, plUiTextHAlign::Center, plUiTextVAlign::Center, true);
      break;
    }

    case ItemType::Dropdown:
      plAudioEngineUi::Dropdown(m_Writer, rect, {}, item.m_sText);
      break;

    case ItemType::Slider:
    {
      const bool bActive = m_sSlider == item.m_sId;
      plAudioEngineUi::Slider(m_Writer, rect, bActive ? m_fSliderValue : plMath::Clamp(item.m_fValue, 0.0f, 1.0f), true, bActive);
      break;
    }

    case ItemType::Button:
      plAudioEngineUi::DashedRect(m_Writer, rect, Border());
      m_Writer.Text(rect, item.m_sText, Accent(), FontSize - 1, plUiTextHAlign::Center, plUiTextVAlign::Center);
      break;

    case ItemType::Danger:
      m_Writer.RectOutline(rect, plAudioEngineUi::Mix(Surface(), Danger(), 0.45f), 1.0f, 4.0f);
      m_Writer.Text(rect, item.m_sText, Danger(), FontSize - 2, plUiTextHAlign::Center, plUiTextVAlign::Center);
      break;

    case ItemType::Card:
      m_Writer.Rect(rect, Plate(), 4.0f);
      break;

    case ItemType::Row:
    {
      const bool bDragged = m_DragKind == DragKind::Row && IndexAfter(item.m_sId, "Mu.Item.") == m_uiDragPosition;
      const bool bTarget = m_DropTarget.m_Kind == DropTarget::Kind::Row && IndexAfter(item.m_sId, "Mu.Item.") == m_DropTarget.m_uiPosition;

      m_Writer.Rect(rect, Plate(), 3.0f);
      if (bTarget)
        m_Writer.RectOutline(rect, Accent(), 1.0f, 3.0f);

      // a grip, the name and the length
      for (plUInt32 i = 0; i < 3; ++i)
        m_Writer.Rect(plRectFloat(rect.x + 7.0f, rect.y + 8.0f + i * 3.5f, 8.0f, 1.5f), Dim());

      m_Writer.Text(plRectFloat(rect.x + 22.0f, rect.y, rect.width - 80.0f, rect.height), item.m_sText, bDragged ? Dim() : Foreground(), FontSize - 2);
      m_Writer.Text(plRectFloat(rect.x, rect.y, rect.width - 8.0f, rect.height), item.m_sSub, Dim(), MinFontSize, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
      break;
    }

    case ItemType::Remove:
    {
      const plVec2 c = rect.GetCenter();
      m_Writer.Line(plVec2(c.x - 4.0f, c.y - 4.0f), plVec2(c.x + 4.0f, c.y + 4.0f), Dim(), 1.5f);
      m_Writer.Line(plVec2(c.x - 4.0f, c.y + 4.0f), plVec2(c.x + 4.0f, c.y - 4.0f), Dim(), 1.5f);
      break;
    }

    case ItemType::DropZone:
    {
      const bool bTarget = m_DropTarget.m_Kind == DropTarget::Kind::Stems;
      if (bTarget)
        m_Writer.Rect(rect, plAudioEngineUi::Mix(Surface(), Accent(), 0.14f), 4.0f);
      plAudioEngineUi::DashedRect(m_Writer, rect, bTarget ? Accent() : Border());
      m_Writer.Text(rect, item.m_sText, bTarget ? Accent() : Dim(), MinFontSize, plUiTextHAlign::Center, plUiTextVAlign::Center, true);
      break;
    }

    default:
      break;
  }
}

void plEditorUiAudioMusicView::DrawMessages(const plRectFloat& area)
{
  using namespace plEditorUi;

  m_Writer.Rect(plRectFloat(area.x, area.y, area.width, 1.0f), Line());

  const float x = area.x + 12.0f;
  const float fWidth = area.width - 24.0f;
  float y = area.y + 6.0f;

  m_Writer.Text(plRectFloat(x, y, fWidth, 18.0f), "MESSAGES TO GAME", Muted(), MinFontSize);
  y += 22.0f;

  if (m_Messages.IsEmpty())
    m_Writer.Text(plRectFloat(x, y, fWidth, 16.0f), "Transitions, stingers and beats show here.", Dim(), FontSize - 2);

  m_Writer.PushClip(area);
  for (plUInt32 i = m_Messages.GetCount(); i > 0 && y + 16.0f < area.y + area.height; --i)
  {
    const Message& message = m_Messages[i - 1];
    const plColorGammaUB color = message.m_Kind == MessageKind::Transition ? Axis(1) : (message.m_Kind == MessageKind::Beat ? Dim() : Foreground());

    m_Writer.Text(plRectFloat(x, y, 40.0f, 16.0f), message.m_sPosition, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    m_Writer.Text(plRectFloat(x + 44.0f, y, fWidth - 44.0f, 16.0f), message.m_sText, color, FontSize - 2);
    y += 17.0f;
  }
  m_Writer.PopClip();
}

void plEditorUiAudioMusicView::DrawStatus(const plRectFloat& area)
{
  using namespace plEditorUi;

  const plAudioMusicAssetProperties* pProp = m_pDocument->GetProperties();

  m_Writer.Rect(area, Plate());
  m_Writer.Rect(plRectFloat(area.x, area.y, area.width, 1.0f), Line());

  // counts: the label dim, the number bright
  float x = area.x + s_fPad;
  auto count = [&](plStringView sLabel, plUInt32 uiCount)
  {
    plStringBuilder sNumber;
    sNumber.SetFormat("{}", uiCount);

    const float fLabel = MeasureCodeText(sLabel, MinFontSize);
    m_Writer.Text(plRectFloat(x, area.y, fLabel + 4.0f, area.height), sLabel, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    x += fLabel + 6.0f;

    const float fNumber = MeasureCodeText(sNumber, MinFontSize);
    m_Writer.Text(plRectFloat(x, area.y, fNumber + 4.0f, area.height), sNumber, Foreground(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    x += fNumber + 16.0f;
  };

  count("segments", pProp->m_Segments.GetCount());
  count("states", pProp->m_States.GetCount());
  count("rules", pProp->m_Transitions.GetCount());
  count("stingers", pProp->m_Stingers.GetCount());

  plStringBuilder sIssue;
  plUInt32 uiIssues = 0;
  GetIssues(sIssue, uiIssues);

  plStringBuilder sText;
  if (uiIssues > 0)
  {
    sText = sIssue;
    if (uiIssues > 1)
      sText.AppendFormat(" (+{})", uiIssues - 1);
  }
  else
  {
    sText = "ready to play";
  }

  if (m_Status.m_bPlaying && m_Status.m_uiPendingState < pProp->m_States.GetCount())
  {
    if (m_Status.m_fPendingIn >= 0.0)
      sText.AppendFormat("  \xC2\xB7  {} in {} s", pProp->m_States[m_Status.m_uiPendingState].m_sName, plArgF(m_Status.m_fPendingIn, 2));
    else
      sText.AppendFormat("  \xC2\xB7  {} asked for", pProp->m_States[m_Status.m_uiPendingState].m_sName);
  }

  m_Writer.Text(plRectFloat(x, area.y, area.x + area.width - x - s_fPad, area.height), sText, uiIssues > 0 ? Warning() : Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
}

void plEditorUiAudioMusicView::DrawDrag()
{
  using namespace plEditorUi;

  if (m_DragKind == DragKind::None || m_sDragLabel.IsEmpty())
    return;

  // a pill with what is dragged, by the pointer
  const float fWidth = MeasureText(m_sDragLabel, FontSize - 2) + 20.0f;
  const plRectFloat rect(m_vDragPos.x + 14.0f, m_vDragPos.y + 10.0f, fWidth, 22.0f);
  const bool bValid = m_DropTarget.m_Kind != DropTarget::Kind::None;

  m_Writer.Rect(rect, Raised(), 4.0f);
  m_Writer.RectOutline(rect, bValid ? Accent() : Border(), 1.0f, 4.0f);
  m_Writer.Text(rect, m_sDragLabel, Foreground(), FontSize - 2, plUiTextHAlign::Center, plUiTextVAlign::Center);
}
