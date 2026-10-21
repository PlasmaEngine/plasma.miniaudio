#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAsset.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventDeckView.h>
#include <EditorUi/Drawings.h>
#include <EditorUi/Menu.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GameUiPlugin/Components/UiCanvasComponent.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

#include <QtMath>

namespace
{
  constexpr float s_fPad = 8.0f;
  constexpr float s_fHeader = plAudioEngineUi::SectionHeight;
  constexpr float s_fEntryTop = 62.0f;
  constexpr float s_fEntryPitch = 28.0f;
  constexpr float s_fRowTop = 34.0f;
  constexpr float s_fRowPitch = 30.0f;
  constexpr float s_fLabelWidth = 96.0f;
  constexpr float s_fRandomizationMax = 24.0f;

  /// The automation targets the add menu offers per parameter (plAudioEventAutomationTarget 0..SendLevel).
  constexpr plUInt32 s_uiTargets = plAudioEventAutomationTarget::SendLevel + 1;

  const plHashedString s_sDeckEntry = plMakeHashedString("Deck.Drawing");
  const plHashedString s_sAutomationEntry = plMakeHashedString("Automation.Drawing");

  constexpr const char* s_szModes[] = {"Random", "Shuffle (no repeat)", "Sequential"};

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

  plString TextBinding(plStringView sField)
  {
    return plStringBuilder(sField, ".Text");
  }

  plString SubmitEvent(plStringView sField)
  {
    return plStringBuilder(sField, ".Submit");
  }

  plString IndexedName(const char* szPrefix, plUInt32 uiIndex)
  {
    plStringBuilder s;
    s.SetFormat("{}.{}", szPrefix, uiIndex);
    return s;
  }

  /// The index after a "Prefix." name, or -1.
  plInt32 IndexOf(plStringView sName, const char* szPrefix)
  {
    const plStringBuilder sPrefix(szPrefix, ".");
    if (!sName.StartsWith(sPrefix))
      return -1;

    plStringView sIndex = sName;
    sIndex.Shrink(sPrefix.GetCharacterCount(), 0);

    plUInt32 uiIndex = 0;
    if (plConversionUtils::StringToUInt(sIndex, uiIndex).Failed())
      return -1;

    return static_cast<plInt32>(uiIndex);
  }

  /// The line a playlist entry shows, with its tooltip.
  void DescribeEntry(const plDocumentObject* pEntry, plString& out_sText, plString& out_sToolTip, bool& out_bSilence)
  {
    const plString sEvent = pEntry->GetTypeAccessor().GetValue("Event").ConvertTo<plString>();
    const plString sClip = pEntry->GetTypeAccessor().GetValue("Clip").ConvertTo<plString>();
    const plString sFile = pEntry->GetTypeAccessor().GetValue("File").ConvertTo<plString>();
    out_bSilence = false;

    if (!sEvent.IsEmpty())
    {
      const auto pSubAsset = plAssetCurator::GetSingleton()->FindSubAsset(sEvent);
      if (pSubAsset.isValid())
      {
        out_sText = plStringBuilder("Event: ", pSubAsset->GetName());
        out_sToolTip = "Nested event - double-click its clip on the timeline to open it";
      }
      else
      {
        out_sText = "missing event";
        out_sToolTip = sEvent;
      }
    }
    else if (!sClip.IsEmpty())
    {
      plAudioClipAssetMetaInfo clipInfo;
      plString sClipName;
      if (plAudioClipAssetUtils::GetClipInfo(sClip, clipInfo, &sClipName))
      {
        out_sText = sClipName;
        out_sToolTip = clipInfo.m_sSourceFile;
      }
      else
      {
        out_sText = "missing clip";
        out_sToolTip = sClip;
      }
    }
    else if (sFile.IsEmpty())
    {
      out_sText = "\xE2\x80\x94 silence \xE2\x80\x94";
      out_sToolTip = "Weighted silence entry - pick a clip via '...'";
      out_bSilence = true;
    }
    else
    {
      out_sText = plPathUtils::GetFileNameAndExtension(sFile.GetView());
      out_sToolTip = plStringBuilder(sFile, "\nEmbedded sound file - 'Extract Clips' turns it into an Audio Clip");
    }
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// plEditorUiAudioEventDeck

plEditorUiAudioEventDeck::plEditorUiAudioEventDeck(plDocument* pDocument)
  : plEditorUiContent("AudioEventDeck")
  , m_pDocument(pDocument)
{
  if (IsReady())
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioEventDeck::OnPointer, this));
}

plEditorUiAudioEventDeck::~plEditorUiAudioEventDeck()
{
  if (IsReady())
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
}

void plEditorUiAudioEventDeck::SetInstrument(const plUuid& instrumentGuid)
{
  m_InstrumentGuid = instrumentGuid;
  m_bRebuild = true;
  RequestRedraw();
}

const plDocumentObject* plEditorUiAudioEventDeck::GetInstrument() const
{
  if (!m_InstrumentGuid.IsValid())
    return nullptr;

  const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(m_InstrumentGuid);
  return (pObject != nullptr && pObject->GetParentProperty().IsEqual("Instruments")) ? pObject : nullptr;
}

void plEditorUiAudioEventDeck::GatherPlaylist(plHybridArray<const plDocumentObject*, 8>& out_entries) const
{
  GatherChildrenOfProperty(GetInstrument(), "Playlist", out_entries);
}

float plEditorUiAudioEventDeck::GetContentHeight() const
{
  plHybridArray<const plDocumentObject*, 8> entries;
  GatherPlaylist(entries);

  const float fInstrument = s_fEntryTop + entries.GetCount() * s_fEntryPitch + s_fEntryPitch + s_fPad;
  const float fPlayback = s_fRowTop + 6.0f * s_fRowPitch + 4.0f;
  return plMath::Max(fInstrument, fPlayback);
}

float plEditorUiAudioEventDeck::GetColumnSplit() const
{
  return plMath::Round((IsReady() ? GetSurface()->GetLogicalSize().x : 600.0f) * 0.5f);
}

plRectFloat plEditorUiAudioEventDeck::GetSliderRect(Slider slider) const
{
  const float x0 = GetColumnSplit();
  const float fWidth = (IsReady() ? GetSurface()->GetLogicalSize().x : 600.0f) - x0;
  const float fControl = plMath::Max(fWidth - s_fPad - s_fLabelWidth - 8.0f - s_fPad, 40.0f);
  const float y = s_fRowTop + (slider == Slider::Volume ? 1.0f : 2.0f) * s_fRowPitch;
  return plRectFloat(x0 + s_fPad + s_fLabelWidth + 8.0f, y, plMath::Max(fControl - 60.0f, 20.0f), plEditorUi::ControlHeight);
}

void plEditorUiAudioEventDeck::OnUiUpdate()
{
  if (!IsReady())
    return;

  // a number field commits when it loses focus
  const plStringView sFocused = GetSurface()->GetCanvas().GetFocusedElementName();
  if (!m_sFocusedField.IsEmpty() && sFocused != m_sFocusedField)
  {
    const plString sField = m_sFocusedField;
    m_sFocusedField.Clear();
    CommitField(sField);
  }

  if (sFocused.StartsWith("Deck.Field."))
    m_sFocusedField = sFocused;

  if (GetSurface()->GetLogicalSize() != m_vBuiltSize)
    m_bRebuild = true;

  // the document is rebuilt only while no field is being edited
  if (m_bRebuild && m_sFocusedField.IsEmpty())
    BuildDocument();

  if (m_bDrawDirty)
    Draw();
}

void plEditorUiAudioEventDeck::BuildDocument()
{
  using namespace plEditorUi;

  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();
  plBlackboard& board = GetSurface()->GetBlackboard();

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = Surface();
  DrawList(pRoot, "Deck.Drawing", s_sDeckEntry.GetView());

  const plDocumentObject* pInstrument = GetInstrument();
  const float fSplit = GetColumnSplit();
  const float fRight = m_vBuiltSize.x;

  // INSTRUMENT column
  plAudioEngineUi::HitButton(pRoot, plRectFloat(s_fPad, 32.0f, fSplit - 2.0f * s_fPad, ControlHeight), "Deck.Instrument", "Selects the instrument edited below", &board);

  if (pInstrument != nullptr)
  {
    plHybridArray<const plDocumentObject*, 8> entries;
    GatherPlaylist(entries);

    for (plUInt32 i = 0; i < entries.GetCount(); ++i)
    {
      const float y = s_fEntryTop + i * s_fEntryPitch;
      const float fRemoveX = fSplit - s_fPad - 20.0f;
      const float fMoreX = fRemoveX - 22.0f;
      const float fWeightX = fMoreX - 4.0f - 48.0f;
      const float fHintX = fWeightX - 14.0f;
      const float fPlayX = fHintX - 4.0f - 20.0f;

      plString sText, sToolTip;
      bool bSilence = false;
      DescribeEntry(entries[i], sText, sToolTip, bSilence);

      plAudioEngineUi::HitButton(pRoot, plRectFloat(s_fPad, y, fPlayX - 4.0f - s_fPad, ControlHeight), IndexedName("Deck.Entry", i), sToolTip, &board);

      plUiButton* pPlay = IconButton(pRoot, Icon::Play, IndexedName("Deck.Play", i));
      plAudioEngineUi::Place(pPlay, plRectFloat(fPlayX, y + 2.0f, 20.0f, 20.0f));
      SetToolTip(board, IndexedName("Deck.Play", i), "Auditions this entry alone, through the engine process");

      const plString sField = IndexedName("Deck.Field.Weight", i);
      NumberField(pRoot, plRectFloat(fWeightX, y, 48.0f, ControlHeight), sField, TextBinding(sField), SubmitEvent(sField));
      SetToolTip(board, sField, "Random pick weight of this entry");

      plUiButton* pMore = IconButton(pRoot, Icon::More, IndexedName("Deck.Pick", i));
      plAudioEngineUi::Place(pMore, plRectFloat(fMoreX, y + 2.0f, 20.0f, 20.0f));
      SetToolTip(board, IndexedName("Deck.Pick", i), "Pick the Audio Clip of this entry");

      plUiButton* pRemove = IconButton(pRoot, Icon::Clear, IndexedName("Deck.Remove", i));
      plAudioEngineUi::Place(pRemove, plRectFloat(fRemoveX, y + 2.0f, 20.0f, 20.0f));
      SetToolTip(board, IndexedName("Deck.Remove", i), "Remove this entry");
    }

    // the add buttons after the playlist
    const float y = s_fEntryTop + entries.GetCount() * s_fEntryPitch;
    float x = s_fPad;
    const struct
    {
      const char* m_szLabel;
      const char* m_szAction;
      const char* m_szToolTip;
    } adds[] = {
      {"Add Files...", "Deck.AddFiles", "Appends sound files to this playlist as Audio Clips (the clip next to each file is reused or created)"},
      {"Clip...", "Deck.AddClip", "Appends an existing Audio Clip to this playlist"},
      {"Event...", "Deck.AddEvent", "Appends another Audio Event to this playlist: it plays as a nested event with its own instruments and parameters"},
      {"Silence", "Deck.AddSilence", "Adds a weighted silence entry"},
    };

    for (const auto& add : adds)
    {
      const float fWidth = plAudioEngineUi::TextButtonWidth(add.m_szLabel, true);
      plAudioEngineUi::HitButton(pRoot, plRectFloat(x, y, fWidth, ControlHeight), add.m_szAction, add.m_szToolTip, &board);
      x += fWidth + 4.0f;
    }

    m_uiBuiltEntries = entries.GetCount();
  }

  // PLAYBACK column
  if (pInstrument != nullptr)
  {
    const float x0 = fSplit + s_fPad + s_fLabelWidth + 8.0f;
    const float fControl = plMath::Max(fRight - x0 - s_fPad, 40.0f);

    plAudioEngineUi::HitButton(pRoot, plRectFloat(x0, s_fRowTop, fControl, ControlHeight), "Deck.Mode", {}, &board);
    plAudioEngineUi::HitButton(pRoot, plRectFloat(fSplit + s_fPad, s_fRowTop + 3.0f * s_fRowPitch, 80.0f, ControlHeight), "Deck.Loop", "Re-triggers the playlist while the event is active", &board);

    NumberField(pRoot, plRectFloat(x0, s_fRowTop + 4.0f * s_fRowPitch, 96.0f, ControlHeight), "Deck.Field.StartDelay", TextBinding("Deck.Field.StartDelay"), SubmitEvent("Deck.Field.StartDelay"), {}, "s");
    NumberField(pRoot, plRectFloat(x0, s_fRowTop + 5.0f * s_fRowPitch, 54.0f, ControlHeight), "Deck.Field.RetriggerMin", TextBinding("Deck.Field.RetriggerMin"), SubmitEvent("Deck.Field.RetriggerMin"));
    NumberField(pRoot, plRectFloat(x0 + 78.0f, s_fRowTop + 5.0f * s_fRowPitch, 60.0f, ControlHeight), "Deck.Field.RetriggerMax", TextBinding("Deck.Field.RetriggerMax"), SubmitEvent("Deck.Field.RetriggerMax"), {}, "s");
  }

  m_bBuiltWithInstrument = pInstrument != nullptr;

  SetDocument(Finish(pRoot));
  UpdateFieldTexts();
  m_bDrawDirty = true;
}

void plEditorUiAudioEventDeck::UpdateFieldTexts()
{
  const plDocumentObject* pInstrument = GetInstrument();
  if (pInstrument == nullptr)
    return;

  plBlackboard& board = GetSurface()->GetBlackboard();
  auto set = [&](plStringView sField, double fValue, plUInt32 uiDecimals)
  {
    if (sField == m_sFocusedField)
      return;

    plStringBuilder s;
    s.SetFormat("{}", plArgF(fValue, uiDecimals));
    board.SetEntryValue(TextBinding(sField), plString(s));
  };

  const auto& accessor = pInstrument->GetTypeAccessor();
  set("Deck.Field.StartDelay", accessor.GetValue("StartDelay").ConvertTo<double>(), 2);
  set("Deck.Field.RetriggerMin", accessor.GetValue("MinRetriggerDelay").ConvertTo<double>(), 2);
  set("Deck.Field.RetriggerMax", accessor.GetValue("MaxRetriggerDelay").ConvertTo<double>(), 2);

  plHybridArray<const plDocumentObject*, 8> entries;
  GatherPlaylist(entries);
  for (plUInt32 i = 0; i < entries.GetCount(); ++i)
    set(IndexedName("Deck.Field.Weight", i), entries[i]->GetTypeAccessor().GetValue("Weight").ConvertTo<double>(), 1);
}

void plEditorUiAudioEventDeck::Draw()
{
  m_bDrawDirty = false;
  m_Writer.Clear();

  const plVec2 size = GetSurface()->GetLogicalSize();
  const float fSplit = GetColumnSplit();

  DrawInstrumentColumn(fSplit);
  DrawPlaybackColumn(fSplit, size.x - fSplit);

  // column border and the line under the row
  m_Writer.Rect(plRectFloat(fSplit, 0.0f, 1.0f, size.y), plEditorUi::Line());
  m_Writer.Rect(plRectFloat(0.0f, size.y - 1.0f, size.x, 1.0f), plEditorUi::Line());

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sDeckEntry);
}

void plEditorUiAudioEventDeck::DrawInstrumentColumn(float fWidth)
{
  using namespace plEditorUi;

  const plDocumentObject* pInstrument = GetInstrument();
  const plColorGammaUB audio = plAudioEngineUi::AudioColor();

  plHybridArray<const plDocumentObject*, 8> entries;
  GatherPlaylist(entries);

  // header: INSTRUMENT — MULTI SOUND "NAME"
  plStringBuilder sHeader("Instrument");
  if (pInstrument != nullptr)
  {
    plStringBuilder sName = pInstrument->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      sName = "Instrument";

    sHeader.SetFormat("Instrument \xE2\x80\x94 {} \"{}\"", entries.GetCount() > 1 ? "Multi Sound" : "Single Sound", sName);
  }

  plAudioEngineUi::SectionHeader(m_Writer, plRectFloat(0.0f, 0.0f, fWidth, s_fHeader), sHeader, audio);

  // instrument dropdown
  plString sCurrent = "<no instruments>";
  if (pInstrument != nullptr)
  {
    plHybridArray<const plDocumentObject*, 8> instruments;
    GatherChildrenOfProperty(pInstrument->GetParent(), "Instruments", instruments);

    for (plUInt32 i = 0; i < instruments.GetCount(); ++i)
    {
      if (instruments[i] != pInstrument)
        continue;

      plStringBuilder sName = instruments[i]->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
      if (sName.IsEmpty())
        sName.SetFormat("Instrument {0}", i + 1);
      sCurrent = sName;
    }
  }

  plAudioEngineUi::Dropdown(m_Writer, plRectFloat(s_fPad, 32.0f, fWidth - 2.0f * s_fPad, ControlHeight), {}, sCurrent, pInstrument != nullptr);

  if (pInstrument == nullptr)
  {
    m_Writer.Text(plRectFloat(s_fPad + 4.0f, s_fEntryTop + 4.0f, fWidth - 2.0f * s_fPad, 18.0f), "No instrument.", Muted(), FontSize - 1);
    m_Writer.Text(plRectFloat(s_fPad + 4.0f, s_fEntryTop + 22.0f, fWidth - 2.0f * s_fPad, 18.0f), "'+ Instrument' in the toolbar adds one.", Dim(), FontSize - 1);
    return;
  }

  const plDocumentObject* pSelected = m_pDocument->GetSelectionManager()->GetCurrentObject();

  for (plUInt32 i = 0; i < entries.GetCount(); ++i)
  {
    const float y = s_fEntryTop + i * s_fEntryPitch;
    const float fRemoveX = fWidth - s_fPad - 20.0f;
    const float fMoreX = fRemoveX - 22.0f;
    const float fWeightX = fMoreX - 4.0f - 48.0f;
    const float fHintX = fWeightX - 14.0f;
    const float fPlayX = fHintX - 4.0f - 20.0f;

    plString sText, sToolTip;
    bool bSilence = false;
    DescribeEntry(entries[i], sText, sToolTip, bSilence);

    const plRectFloat button(s_fPad, y, fPlayX - 4.0f - s_fPad, ControlHeight);
    m_Writer.Rect(button, Plate(), 4.0f);
    m_Writer.RectOutline(button, pSelected == entries[i] ? Accent() : Line(), 1.0f, 4.0f);
    plAudioEngineUi::Icon(m_Writer, plRectFloat(button.x + 8.0f, y + 5.0f, 14.0f, 14.0f), plAudioEngineUi::IconClip, bSilence ? Dim() : audio, true);
    m_Writer.Text(plRectFloat(button.x + 28.0f, y, button.width - 34.0f, ControlHeight), sText, bSilence ? Dim() : Foreground(), FontSize - 1);

    m_Writer.Text(plRectFloat(fHintX, y, 12.0f, ControlHeight), "w", Dim(), FontSize - 2, plUiTextHAlign::Center);
  }

  // add buttons
  const float y = s_fEntryTop + entries.GetCount() * s_fEntryPitch;
  float x = s_fPad;
  for (const char* szLabel : {"Add Files...", "Clip...", "Event...", "Silence"})
  {
    const float fButton = plAudioEngineUi::TextButtonWidth(szLabel, true);
    plAudioEngineUi::TextButton(m_Writer, plRectFloat(x, y, fButton, ControlHeight), szLabel, IconPath(Icon::Add));
    x += fButton + 4.0f;
  }
}

void plEditorUiAudioEventDeck::DrawPlaybackColumn(float x, float fWidth)
{
  using namespace plEditorUi;

  const plDocumentObject* pInstrument = GetInstrument();
  const bool bEnabled = pInstrument != nullptr;

  plAudioEngineUi::SectionHeader(m_Writer, plRectFloat(x, 0.0f, fWidth, s_fHeader), "Playback", plAudioEngineUi::AudioColor());

  const float fLabelX = x + s_fPad;
  const float fControlX = fLabelX + s_fLabelWidth + 8.0f;
  const float fControl = plMath::Max(x + fWidth - fControlX - s_fPad, 40.0f);
  const plColorGammaUB label = bEnabled ? Muted() : Dim();

  auto rowY = [](plUInt32 uiRow)
  { return s_fRowTop + uiRow * s_fRowPitch; };

  // playlist mode
  plInt64 iMode = 0;
  double fVolumeRand = 0.0;
  double fPitchRand = 0.0;
  bool bLoop = false;

  if (pInstrument != nullptr)
  {
    const auto& accessor = pInstrument->GetTypeAccessor();
    iMode = plMath::Clamp<plInt64>(accessor.GetValue("PlaylistMode").ConvertTo<plInt64>(), 0, 2);
    fVolumeRand = accessor.GetValue("VolumeRandomization").ConvertTo<double>();
    fPitchRand = accessor.GetValue("PitchRandomization").ConvertTo<double>();
    bLoop = accessor.GetValue("Loop").ConvertTo<bool>();
  }

  if (m_DragSlider == Slider::Volume)
    fVolumeRand = m_fDragValue;
  else if (m_DragSlider == Slider::Pitch)
    fPitchRand = m_fDragValue;

  m_Writer.Text(plRectFloat(fLabelX, rowY(0), s_fLabelWidth, ControlHeight), "Playlist mode", label, FontSize - 1);
  plAudioEngineUi::Dropdown(m_Writer, plRectFloat(fControlX, rowY(0), fControl, ControlHeight), {}, s_szModes[iMode], bEnabled);

  // randomization sliders with their values
  auto slider = [&](plUInt32 uiRow, Slider which, plStringView sLabel, double fValue, plStringView sUnit)
  {
    m_Writer.Text(plRectFloat(fLabelX, rowY(uiRow), s_fLabelWidth, ControlHeight), sLabel, label, FontSize - 1);
    plAudioEngineUi::Slider(m_Writer, GetSliderRect(which), static_cast<float>(fValue / s_fRandomizationMax), bEnabled, m_DragSlider == which);

    plStringBuilder sValue;
    sValue.SetFormat("\xC2\xB1{} {}", plArgF(fValue, 1), sUnit);
    m_Writer.Text(plRectFloat(fControlX + fControl - 54.0f, rowY(uiRow), 54.0f, ControlHeight), sValue, label, FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
  };

  slider(1, Slider::Volume, "Volume rand.", fVolumeRand, "dB");
  slider(2, Slider::Pitch, "Pitch rand.", fPitchRand, "st");

  // loop
  plEditorUiDraw::CheckBox(m_Writer, plRectFloat(fLabelX, rowY(3) + 5.0f, plEditorUiDraw::CheckBoxSize, plEditorUiDraw::CheckBoxSize), bLoop);
  m_Writer.Text(plRectFloat(fLabelX + 22.0f, rowY(3), 60.0f, ControlHeight), "Loop", bEnabled ? Foreground() : Dim(), FontSize - 1);

  // start delay and retrigger gap; the fields are retained elements over these rows
  m_Writer.Text(plRectFloat(fLabelX, rowY(4), s_fLabelWidth, ControlHeight), "Start delay", label, FontSize - 1);
  m_Writer.Text(plRectFloat(fLabelX, rowY(5), s_fLabelWidth, ControlHeight), "Retrigger gap", label, FontSize - 1);
  m_Writer.Text(plRectFloat(fControlX + 54.0f, rowY(5), 24.0f, ControlHeight), "to", Dim(), FontSize - 2, plUiTextHAlign::Center);

  if (!bEnabled)
  {
    for (const plRectFloat& rect : {plRectFloat(fControlX, rowY(4), 96.0f, ControlHeight), plRectFloat(fControlX, rowY(5), 54.0f, ControlHeight), plRectFloat(fControlX + 78.0f, rowY(5), 60.0f, ControlHeight)})
    {
      m_Writer.Rect(rect, Field(), 4.0f);
      m_Writer.RectOutline(rect, Line(), 1.0f, 4.0f);
    }
  }
}

plString plEditorUiAudioEventDeck::GetToolTipAt(const plVec2& vLogical) const
{
  const float fSplit = GetColumnSplit();
  const plRectFloat retrigger(fSplit + s_fPad, s_fRowTop + 5.0f * s_fRowPitch, s_fLabelWidth, plEditorUi::ControlHeight);
  if (retrigger.Contains(vLogical))
    return "Random loop re-trigger delay (scatterer). Zero = gapless.";

  return plEditorUiContent::GetToolTipAt(vLogical);
}

void plEditorUiAudioEventDeck::OnUiEvent(const plMsgUiEvent& event)
{
  const plStringView sId = event.m_sIdentifier.GetView();

  if (sId == "Deck.Instrument")
    ShowInstrumentMenu();
  else if (sId == "Deck.Mode")
    ShowModeMenu();
  else if (sId == "Deck.Loop")
  {
    if (const plDocumentObject* pInstrument = GetInstrument())
      SetInstrumentValue("Loop", !pInstrument->GetTypeAccessor().GetValue("Loop").ConvertTo<bool>(), "Set Loop");
  }
  else if (sId == "Deck.AddFiles" && m_OnAddFiles.IsValid())
    m_OnAddFiles();
  else if (sId == "Deck.AddClip" && m_OnAddClip.IsValid())
    m_OnAddClip();
  else if (sId == "Deck.AddEvent" && m_OnAddEvent.IsValid())
    m_OnAddEvent();
  else if (sId == "Deck.AddSilence" && m_OnAddSilence.IsValid())
    m_OnAddSilence();
  else if (sId.EndsWith(".Submit"))
  {
    plStringView sField = sId;
    sField.Shrink(0, 7);
    CommitField(sField);
  }
  else
  {
    plHybridArray<const plDocumentObject*, 8> entries;
    GatherPlaylist(entries);

    if (const plInt32 iEntry = IndexOf(sId, "Deck.Entry"); iEntry >= 0 && iEntry < static_cast<plInt32>(entries.GetCount()))
    {
      m_pDocument->GetSelectionManager()->SetSelection(entries[iEntry]);
    }
    else if (const plInt32 iPlay = IndexOf(sId, "Deck.Play"); iPlay >= 0 && iPlay < static_cast<plInt32>(entries.GetCount()))
    {
      if (m_OnAuditionEntry.IsValid())
        m_OnAuditionEntry(entries[iPlay]->GetGuid());
    }
    else if (const plInt32 iPick = IndexOf(sId, "Deck.Pick"); iPick >= 0 && iPick < static_cast<plInt32>(entries.GetCount()))
    {
      if (m_OnPickEntryClip.IsValid())
        m_OnPickEntryClip(entries[iPick]->GetGuid());
    }
    else if (const plInt32 iRemove = IndexOf(sId, "Deck.Remove"); iRemove >= 0 && iRemove < static_cast<plInt32>(entries.GetCount()))
    {
      plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
      accessor.StartTransaction("Remove Playlist Entry");
      if (accessor.RemoveObject(entries[iRemove]).Failed())
      {
        accessor.CancelTransaction();
        return;
      }
      accessor.FinishTransaction();
    }
  }
}

void plEditorUiAudioEventDeck::CommitField(plStringView sField)
{
  const plDocumentObject* pInstrument = GetInstrument();
  if (pInstrument == nullptr)
    return;

  const plVariant text = GetSurface()->GetBlackboard().GetEntryValue(plTempHashedString(TextBinding(sField)));
  double fValue = 0.0;
  if (!text.IsA<plString>() || plConversionUtils::StringToFloat(text.Get<plString>(), fValue).Failed())
  {
    UpdateFieldTexts();
    return;
  }

  const plDocumentObject* pTarget = pInstrument;
  const char* szProperty = nullptr;
  const char* szTransaction = nullptr;

  if (sField == "Deck.Field.StartDelay")
  {
    szProperty = "StartDelay";
    szTransaction = "Set Start Delay";
    fValue = plMath::Clamp(fValue, 0.0, 300.0);
  }
  else if (sField == "Deck.Field.RetriggerMin" || sField == "Deck.Field.RetriggerMax")
  {
    szProperty = sField == "Deck.Field.RetriggerMin" ? "MinRetriggerDelay" : "MaxRetriggerDelay";
    szTransaction = "Set Retrigger Delay";
    fValue = plMath::Clamp(fValue, 0.0, 300.0);
  }
  else if (const plInt32 iEntry = IndexOf(sField, "Deck.Field.Weight"); iEntry >= 0)
  {
    plHybridArray<const plDocumentObject*, 8> entries;
    GatherPlaylist(entries);
    if (iEntry >= static_cast<plInt32>(entries.GetCount()))
      return;

    pTarget = entries[iEntry];
    szProperty = "Weight";
    szTransaction = "Set Weight";
    fValue = plMath::Clamp(fValue, 0.0, 100.0);
  }
  else
  {
    return;
  }

  if (plMath::Abs(pTarget->GetTypeAccessor().GetValue(szProperty).ConvertTo<double>() - fValue) < 0.0001)
  {
    UpdateFieldTexts();
    return;
  }

  plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
  accessor.StartTransaction(szTransaction);
  accessor.SetValueByName(pTarget, szProperty, static_cast<float>(fValue)).IgnoreResult();
  accessor.FinishTransaction();
}

void plEditorUiAudioEventDeck::SetInstrumentValue(const char* szProperty, const plVariant& value, const char* szTransaction)
{
  const plDocumentObject* pInstrument = GetInstrument();
  if (pInstrument == nullptr)
    return;

  plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
  accessor.StartTransaction(szTransaction);
  accessor.SetValueByName(pInstrument, szProperty, value).IgnoreResult();
  accessor.FinishTransaction();
}

void plEditorUiAudioEventDeck::ShowInstrumentMenu()
{
  const plDocumentObject* pInstrument = GetInstrument();
  if (pInstrument == nullptr)
    return;

  plHybridArray<const plDocumentObject*, 8> instruments;
  GatherChildrenOfProperty(pInstrument->GetParent(), "Instruments", instruments);

  plEditorUiMenu menu;
  plHybridArray<plUuid, 8> guids;
  for (plUInt32 i = 0; i < instruments.GetCount(); ++i)
  {
    plStringBuilder sName = instruments[i]->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      sName.SetFormat("Instrument {0}", i + 1);

    plEditorUiMenuItem& item = menu.AddAction(sName, i);
    item.m_bCheckable = true;
    item.m_bChecked = instruments[i] == pInstrument;
    guids.PushBack(instruments[i]->GetGuid());
  }

  plEditorUiMenuView::Show(*GetSurface(), plVec2(s_fPad, 32.0f + plEditorUi::ControlHeight), menu, [this, guids](plUInt32 uiId)
    {
      if (uiId < guids.GetCount() && m_OnInstrumentPicked.IsValid())
        m_OnInstrumentPicked(guids[uiId]);
    });
}

void plEditorUiAudioEventDeck::ShowModeMenu()
{
  const plDocumentObject* pInstrument = GetInstrument();
  if (pInstrument == nullptr)
    return;

  const plInt64 iMode = pInstrument->GetTypeAccessor().GetValue("PlaylistMode").ConvertTo<plInt64>();

  plEditorUiMenu menu;
  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_szModes); ++i)
  {
    plEditorUiMenuItem& item = menu.AddAction(s_szModes[i], i);
    item.m_bCheckable = true;
    item.m_bChecked = iMode == static_cast<plInt64>(i);
  }

  const float x = GetColumnSplit() + s_fPad + s_fLabelWidth + 8.0f;
  plEditorUiMenuView::Show(*GetSurface(), plVec2(x, s_fRowTop + plEditorUi::ControlHeight), menu, [this](plUInt32 uiId)
    { SetInstrumentValue("PlaylistMode", static_cast<plInt64>(uiId), "Set Playlist Mode"); });
}

void plEditorUiAudioEventDeck::OnPointer(const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;

  const plDocumentObject* pInstrument = GetInstrument();

  switch (e.m_Type)
  {
    case Type::Press:
    {
      if (e.m_Button != plEditorUiPointerEvent::Button::Left || pInstrument == nullptr || !e.m_sElement.IsEmpty())
        return;

      for (Slider slider : {Slider::Volume, Slider::Pitch})
      {
        const plRectFloat rect = GetSliderRect(slider);
        if (plRectFloat(rect.x - 4.0f, rect.y, rect.width + 8.0f, rect.height).Contains(e.m_vPosition))
        {
          m_DragSlider = slider;
          m_fDragValue = plMath::Round(plAudioEngineUi::SliderFractionAt(rect, e.m_vPosition.x) * s_fRandomizationMax * 10.0f) / 10.0f;
          m_bDrawDirty = true;
          return;
        }
      }
      return;
    }

    case Type::Move:
    case Type::DragStart:
    case Type::DragMove:
    {
      if (m_DragSlider == Slider::None)
        return;

      m_fDragValue = plMath::Round(plAudioEngineUi::SliderFractionAt(GetSliderRect(m_DragSlider), e.m_vPosition.x) * s_fRandomizationMax * 10.0f) / 10.0f;
      m_bDrawDirty = true;
      return;
    }

    case Type::Release:
    case Type::Drop:
    {
      if (m_DragSlider == Slider::None)
        return;

      // one step per drag, written on release
      const Slider slider = m_DragSlider;
      m_DragSlider = Slider::None;
      m_bDrawDirty = true;

      if (slider == Slider::Volume)
        SetInstrumentValue("VolumeRandomization", m_fDragValue, "Set Volume Randomization");
      else
        SetInstrumentValue("PitchRandomization", m_fDragValue, "Set Pitch Randomization");
      return;
    }

    case Type::DragCancel:
      m_DragSlider = Slider::None;
      m_bDrawDirty = true;
      return;

    default:
      return;
  }
}

//////////////////////////////////////////////////////////////////////////
// plEditorUiAudioEventAutomation

namespace
{
  constexpr float s_fCurveTop = 52.0f;
  constexpr float s_fCurveHeight = 110.0f;
  constexpr float s_fAutomationHeight = s_fCurveTop + s_fCurveHeight + 8.0f + plEditorUi::ControlHeight + 8.0f;
  constexpr float s_fPointGrab = 7.0f;
} // namespace

plEditorUiAudioEventAutomation::plEditorUiAudioEventAutomation(plDocument* pDocument)
  : plEditorUiContent("AudioEventAutomation")
  , m_pDocument(pDocument)
{
  if (IsReady())
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioEventAutomation::OnPointer, this));
}

plEditorUiAudioEventAutomation::~plEditorUiAudioEventAutomation()
{
  if (IsReady())
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
}

void plEditorUiAudioEventAutomation::SetAutomation(const plUuid& automationGuid)
{
  m_AutomationGuid = automationGuid;
  m_bDrawDirty = true;
  RequestRedraw();
}

float plEditorUiAudioEventAutomation::GetMinimumHeight()
{
  return s_fAutomationHeight;
}

void plEditorUiAudioEventAutomation::GatherPoints(plHybridArray<CurvePoint, 8>& out_points) const
{
  out_points.Clear();

  const plDocumentObject* pAutomation = m_AutomationGuid.IsValid() ? m_pDocument->GetObjectManager()->GetObject(m_AutomationGuid) : nullptr;
  if (pAutomation == nullptr)
    return;

  plHybridArray<const plDocumentObject*, 8> objects;
  GatherChildrenOfProperty(pAutomation, "Points", objects);

  for (const plDocumentObject* pPoint : objects)
  {
    CurvePoint& point = out_points.ExpandAndGetRef();
    point.m_Guid = pPoint->GetGuid();
    point.m_fX = pPoint->GetTypeAccessor().GetValue("X").ConvertTo<double>();
    point.m_fY = pPoint->GetTypeAccessor().GetValue("Y").ConvertTo<double>();

    // a point being dragged shows where it goes
    if (m_bDragging && point.m_Guid == m_DragPointGuid)
    {
      point.m_fX = m_fDragX;
      point.m_fY = m_fDragY;
    }
  }

  out_points.Sort([](const CurvePoint& a, const CurvePoint& b)
    { return a.m_fX < b.m_fX; });
}

void plEditorUiAudioEventAutomation::GetRanges(double& out_fMinX, double& out_fMaxX, double& out_fMaxY) const
{
  out_fMinX = 0.0;
  out_fMaxX = 1.0;
  out_fMaxY = 1.0;

  // the parameter's authored range is the curve's x domain, so dragging maps to real values
  const plDocumentObject* pAutomation = m_AutomationGuid.IsValid() ? m_pDocument->GetObjectManager()->GetObject(m_AutomationGuid) : nullptr;
  if (pAutomation != nullptr)
  {
    const plString sParam = pAutomation->GetTypeAccessor().GetValue("Parameter").ConvertTo<plString>();

    plHybridArray<const plDocumentObject*, 8> parameters;
    GatherChildrenOfProperty(pAutomation->GetParent(), "Parameters", parameters);

    for (const plDocumentObject* pParameter : parameters)
    {
      if (pParameter->GetTypeAccessor().GetValue("Name").ConvertTo<plString>() == sParam)
      {
        out_fMinX = pParameter->GetTypeAccessor().GetValue("Min").ConvertTo<double>();
        out_fMaxX = pParameter->GetTypeAccessor().GetValue("Max").ConvertTo<double>();
        break;
      }
    }
  }

  if (out_fMaxX <= out_fMinX)
    out_fMaxX = out_fMinX + 1.0;

  // the stored points, not the dragged preview, so the scale holds still during a drag
  if (pAutomation != nullptr)
  {
    plHybridArray<const plDocumentObject*, 8> objects;
    GatherChildrenOfProperty(pAutomation, "Points", objects);

    for (const plDocumentObject* pPoint : objects)
    {
      out_fMaxY = plMath::Max(out_fMaxY, pPoint->GetTypeAccessor().GetValue("Y").ConvertTo<double>());
      out_fMinX = plMath::Min(out_fMinX, pPoint->GetTypeAccessor().GetValue("X").ConvertTo<double>());
      out_fMaxX = plMath::Max(out_fMaxX, pPoint->GetTypeAccessor().GetValue("X").ConvertTo<double>());
    }
  }

  out_fMaxY = plMath::Max(out_fMaxY, 1.0);

  // a cutoff curve is in Hz: the whole audible range
  if (pAutomation != nullptr && pAutomation->GetTypeAccessor().GetValue("Target").ConvertTo<plInt64>() == plAudioEventAutomationTarget::FilterCutoff)
    out_fMaxY = plMath::Max(out_fMaxY, 22000.0);
}

plRectFloat plEditorUiAudioEventAutomation::GetCurveRect() const
{
  const float fWidth = IsReady() ? GetSurface()->GetLogicalSize().x : 300.0f;
  return plRectFloat(s_fPad, s_fCurveTop, plMath::Max(fWidth - 2.0f * s_fPad, 20.0f), s_fCurveHeight);
}

plRectFloat plEditorUiAudioEventAutomation::GetPlotRect() const
{
  const plRectFloat curve = GetCurveRect();
  return plRectFloat(curve.x + 8.0f, curve.y + 8.0f, curve.width - 16.0f, curve.height - 22.0f);
}

plVec2 plEditorUiAudioEventAutomation::ToView(double fX, double fY) const
{
  double fMinX, fMaxX, fMaxY;
  GetRanges(fMinX, fMaxX, fMaxY);

  const plRectFloat plot = GetPlotRect();
  const double fTx = (fX - fMinX) / (fMaxX - fMinX);
  const double fTy = fY / fMaxY;
  return plVec2(plot.x + static_cast<float>(fTx * plot.width), plot.y + plot.height - static_cast<float>(fTy * plot.height));
}

void plEditorUiAudioEventAutomation::FromView(const plVec2& vPos, double& out_fX, double& out_fY) const
{
  double fMinX, fMaxX, fMaxY;
  GetRanges(fMinX, fMaxX, fMaxY);

  const plRectFloat plot = GetPlotRect();
  const double fTx = plMath::Clamp((vPos.x - plot.x) / plMath::Max(plot.width, 1.0f), 0.0f, 1.0f);
  const double fTy = plMath::Clamp((plot.y + plot.height - vPos.y) / plMath::Max(plot.height, 1.0f), 0.0f, 1.0f);

  out_fX = fMinX + fTx * (fMaxX - fMinX);
  out_fY = fTy * fMaxY;
}

plInt32 plEditorUiAudioEventAutomation::FindPointAt(const plVec2& vPos) const
{
  plHybridArray<CurvePoint, 8> points;
  GatherPoints(points);

  for (plUInt32 i = 0; i < points.GetCount(); ++i)
  {
    if ((ToView(points[i].m_fX, points[i].m_fY) - vPos).GetLength() <= s_fPointGrab)
      return static_cast<plInt32>(i);
  }

  return -1;
}

void plEditorUiAudioEventAutomation::OnUiUpdate()
{
  if (!IsReady())
    return;

  if (GetSurface()->GetLogicalSize() != m_vBuiltSize)
    m_bRebuild = true;

  if (m_bRebuild)
    BuildDocument();

  if (m_bDrawDirty)
    Draw();
}

void plEditorUiAudioEventAutomation::BuildDocument()
{
  using namespace plEditorUi;

  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = Surface();
  DrawList(pRoot, "Automation.Drawing", s_sAutomationEntry.GetView());

  const float fButton = plAudioEngineUi::TextButtonWidth("automate property...", true);
  plAudioEngineUi::HitButton(pRoot, plRectFloat(s_fPad, s_fCurveTop + s_fCurveHeight + 8.0f, fButton, ControlHeight), "Automation.Add", "Adds an automation curve driven by an event parameter", &GetSurface()->GetBlackboard());

  SetDocument(Finish(pRoot));
  m_bDrawDirty = true;
}

void plEditorUiAudioEventAutomation::Draw()
{
  using namespace plEditorUi;

  m_bDrawDirty = false;
  m_Writer.Clear();

  const plVec2 size = GetSurface()->GetLogicalSize();
  const plColorGammaUB audio = plAudioEngineUi::AudioColor();
  const plDocumentObject* pAutomation = m_AutomationGuid.IsValid() ? m_pDocument->GetObjectManager()->GetObject(m_AutomationGuid) : nullptr;

  // header: AUTOMATION (PARAMETER) and the target line
  plStringBuilder sHeader("Automation");
  plStringBuilder sTarget = "no automation \xC2\xB7 add one below";
  if (pAutomation != nullptr)
  {
    plStringBuilder sParam = pAutomation->GetTypeAccessor().GetValue("Parameter").ConvertTo<plString>();
    if (sParam.IsEmpty())
      sParam = "?";

    sHeader.SetFormat("Automation ({})", sParam);
    plAudioEventAssetDocument::FormatAutomationTarget(pAutomation, sTarget);
    sTarget.Append(" \xC2\xB7 curve \xC2\xB7 change the target in Details");
  }

  plAudioEngineUi::SectionHeader(m_Writer, plRectFloat(0.0f, 0.0f, size.x, s_fHeader), sHeader, audio);
  m_Writer.Text(plRectFloat(s_fPad, 30.0f, size.x - 2.0f * s_fPad, 18.0f), sTarget, Dim(), FontSize - 2);

  // the curve box
  const plRectFloat curve = GetCurveRect();
  m_Writer.Rect(curve, Background(), 4.0f);
  m_Writer.RectOutline(curve, Line(), 1.0f, 4.0f);

  plHybridArray<CurvePoint, 8> points;
  GatherPoints(points);

  if (pAutomation == nullptr || points.IsEmpty())
  {
    m_Writer.Text(curve, pAutomation == nullptr ? "no automation" : "double-click to add a point", Dim(), FontSize - 1, plUiTextHAlign::Center);
  }
  else
  {
    double fMinX, fMaxX, fMaxY;
    GetRanges(fMinX, fMaxX, fMaxY);

    const plRectFloat plot = GetPlotRect();

    // y = 1: no modulation
    const float fBaseY = ToView(fMinX, 1.0).y;
    m_Writer.Line(plVec2(curve.x, fBaseY), plVec2(curve.x + curve.width, fBaseY), Border(), 1.0f, 2.0f, 3.0f);

    plHybridArray<plVec2, 16> line;
    for (const CurvePoint& point : points)
      line.PushBack(ToView(point.m_fX, point.m_fY));

    m_Writer.Polyline(line, audio, 2.0f);

    for (plUInt32 i = 0; i < points.GetCount(); ++i)
    {
      const bool bDragged = m_bDragging && points[i].m_Guid == m_DragPointGuid;
      m_Writer.Circle(line[i], bDragged ? 4.5f : 3.5f, bDragged ? Accent() : audio);
    }

    plStringBuilder sMin, sMax;
    sMin.SetFormat("{}", plArgF(fMinX, 2));
    sMax.SetFormat("{}", plArgF(fMaxX, 2));
    m_Writer.Text(plRectFloat(curve.x + 4.0f, curve.y + curve.height - 14.0f, plot.width * 0.5f, 12.0f), sMin, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    m_Writer.Text(plRectFloat(curve.x + curve.width * 0.5f, curve.y + curve.height - 14.0f, curve.width * 0.5f - 4.0f, 12.0f), sMax, Dim(), MinFontSize, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
  }

  // "+ automate property..."
  const float fButton = plAudioEngineUi::TextButtonWidth("automate property...", true);
  plAudioEngineUi::TextButton(m_Writer, plRectFloat(s_fPad, s_fCurveTop + s_fCurveHeight + 8.0f, fButton, ControlHeight), "automate property...", IconPath(Icon::Add));

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sAutomationEntry);
}

plString plEditorUiAudioEventAutomation::GetToolTipAt(const plVec2& vLogical) const
{
  if (GetCurveRect().Contains(vLogical) && m_AutomationGuid.IsValid())
    return "Drag points to shape the curve \xC2\xB7 double-click to add a point \xC2\xB7 right-click a point to remove it";

  return plEditorUiContent::GetToolTipAt(vLogical);
}

void plEditorUiAudioEventAutomation::OnUiEvent(const plMsgUiEvent& event)
{
  if (event.m_sIdentifier.GetView() == "Automation.Add")
    ShowAddMenu();
}

void plEditorUiAudioEventAutomation::ShowAddMenu()
{
  const auto& rootChildren = m_pDocument->GetObjectManager()->GetRootObject()->GetChildren();

  plHybridArray<const plDocumentObject*, 8> parameters;
  GatherChildrenOfProperty(rootChildren.IsEmpty() ? nullptr : rootChildren[0], "Parameters", parameters);

  plEditorUiMenu menu;
  plHybridArray<plString, 8> names;

  for (const plDocumentObject* pParameter : parameters)
  {
    const plString sName = pParameter->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      continue;

    const plUInt32 uiIndex = names.GetCount();
    names.PushBack(sName);

    // one action per target; the target is the id's remainder
    plStringBuilder sLabel;
    sLabel.SetFormat("Volume by {}", sName);
    menu.AddAction(sLabel, uiIndex * s_uiTargets + plAudioEventAutomationTarget::EventVolume);
    sLabel.SetFormat("Pitch by {}", sName);
    menu.AddAction(sLabel, uiIndex * s_uiTargets + plAudioEventAutomationTarget::EventPitch);
    sLabel.SetFormat("Instrument volume by {}", sName);
    menu.AddAction(sLabel, uiIndex * s_uiTargets + plAudioEventAutomationTarget::InstrumentVolume);
    sLabel.SetFormat("Filter cutoff by {}", sName);
    menu.AddAction(sLabel, uiIndex * s_uiTargets + plAudioEventAutomationTarget::FilterCutoff);
    sLabel.SetFormat("Send level by {}", sName);
    menu.AddAction(sLabel, uiIndex * s_uiTargets + plAudioEventAutomationTarget::SendLevel);
  }

  if (names.IsEmpty())
    menu.AddAction("add a parameter first ('+ Sheet' tab)", 0).m_bEnabled = false;

  plEditorUiMenuView::Show(*GetSurface(), plVec2(s_fPad, s_fCurveTop + s_fCurveHeight + 8.0f + plEditorUi::ControlHeight), menu, [this, names](plUInt32 uiId)
    {
      if (uiId / s_uiTargets >= names.GetCount() || !m_OnAddAutomation.IsValid())
        return;

      m_OnAddAutomation(static_cast<plUInt8>(uiId % s_uiTargets), names[uiId / s_uiTargets]);
    });
}

void plEditorUiAudioEventAutomation::OnPointer(const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;
  using Button = plEditorUiPointerEvent::Button;

  const plDocumentObject* pAutomation = m_AutomationGuid.IsValid() ? m_pDocument->GetObjectManager()->GetObject(m_AutomationGuid) : nullptr;

  switch (e.m_Type)
  {
    case Type::Press:
    {
      if (pAutomation == nullptr || !e.m_sElement.IsEmpty() || !GetCurveRect().Contains(e.m_vPosition))
        return;

      const plInt32 iPoint = FindPointAt(e.m_vPosition);
      if (iPoint < 0)
        return;

      plHybridArray<CurvePoint, 8> points;
      GatherPoints(points);

      if (e.m_Button == Button::Left)
      {
        m_bDragging = true;
        m_DragPointGuid = points[iPoint].m_Guid;
        m_fDragX = points[iPoint].m_fX;
        m_fDragY = points[iPoint].m_fY;
        m_bDrawDirty = true;
        return;
      }

      // right-click removes a point; two stay so the curve can be evaluated
      if (e.m_Button == Button::Right && points.GetCount() > 2)
      {
        if (const plDocumentObject* pPoint = m_pDocument->GetObjectManager()->GetObject(points[iPoint].m_Guid))
        {
          plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
          accessor.StartTransaction("Remove Curve Point");
          if (accessor.RemoveObject(pPoint).Failed())
          {
            accessor.CancelTransaction();
            return;
          }
          accessor.FinishTransaction();
        }
      }
      return;
    }

    case Type::Move:
    case Type::DragStart:
    case Type::DragMove:
    {
      if (!m_bDragging)
        return;

      FromView(e.m_vPosition, m_fDragX, m_fDragY);
      m_bDrawDirty = true;
      return;
    }

    case Type::Release:
    case Type::Drop:
    {
      if (!m_bDragging)
        return;

      // one undo step per drag
      m_bDragging = false;
      m_bDrawDirty = true;

      const plDocumentObject* pPoint = m_pDocument->GetObjectManager()->GetObject(m_DragPointGuid);
      m_DragPointGuid = plUuid();
      if (pPoint == nullptr)
        return;

      const auto& accessor = pPoint->GetTypeAccessor();
      if (plMath::Abs(accessor.GetValue("X").ConvertTo<double>() - m_fDragX) < 1e-6 && plMath::Abs(accessor.GetValue("Y").ConvertTo<double>() - m_fDragY) < 1e-6)
        return;

      plObjectCommandAccessor commands(m_pDocument->GetCommandHistory());
      commands.StartTransaction("Move Curve Point");
      commands.SetValueByName(pPoint, "X", static_cast<float>(m_fDragX)).IgnoreResult();
      commands.SetValueByName(pPoint, "Y", static_cast<float>(m_fDragY)).IgnoreResult();
      commands.FinishTransaction();
      return;
    }

    case Type::DragCancel:
      m_bDragging = false;
      m_DragPointGuid = plUuid();
      m_bDrawDirty = true;
      return;

    case Type::DoubleClick:
    {
      if (e.m_Button != Button::Left || pAutomation == nullptr || !e.m_sElement.IsEmpty() || !GetCurveRect().Contains(e.m_vPosition) || FindPointAt(e.m_vPosition) >= 0)
        return;

      double fX = 0.0, fY = 0.0;
      FromView(e.m_vPosition, fX, fY);

      plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
      accessor.StartTransaction("Add Curve Point");

      plUuid pointGuid;
      if (accessor.AddObjectByName(pAutomation, "Points", -1, plGetStaticRTTI<plAudioEventCurvePoint>(), pointGuid).Failed())
      {
        accessor.CancelTransaction();
        return;
      }

      const plDocumentObject* pPoint = m_pDocument->GetObjectManager()->GetObject(pointGuid);
      accessor.SetValueByName(pPoint, "X", static_cast<float>(fX)).IgnoreResult();
      accessor.SetValueByName(pPoint, "Y", static_cast<float>(fY)).IgnoreResult();
      accessor.FinishTransaction();
      return;
    }

    default:
      return;
  }
}
