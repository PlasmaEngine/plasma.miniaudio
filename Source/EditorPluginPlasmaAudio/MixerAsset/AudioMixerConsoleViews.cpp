#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerAsset.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerConsoleViews.h>
#include <EditorUi/Controls.h>
#include <EditorUi/Drawings.h>
#include <EditorUi/Menu.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <GameUiPlugin/Components/UiCanvasComponent.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

namespace
{
  constexpr float s_fPad = 10.0f;
  constexpr float s_fRailHeader = 26.0f;
  constexpr float s_fRailRow = 28.0f;
  constexpr plUInt32 s_uiRailVisibleRows = 4;
  constexpr float s_fParamRow = 26.0f;

  const plHashedString s_sRailEntry = plMakeHashedString("Mixer.Rail");
  const plHashedString s_sInsertEntry = plMakeHashedString("Mixer.Insert");

  template <typename ARRAY>
  void GatherChildren(const plDocumentObject* pParent, const char* szProperty, ARRAY& out_children)
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

  const plDocumentObject* GetMixerProperties(const plDocument* pDocument)
  {
    const auto& children = pDocument->GetObjectManager()->GetRootObject()->GetChildren();
    return children.IsEmpty() ? nullptr : children[0];
  }

  /// Slider position of a parameter value; frequencies map logarithmically.
  float ParamToFraction(const plAudioEffectParameterInfo& info, float fValue)
  {
    if (info.m_bLogarithmic && info.m_fMin > 0.0f)
      return plMath::Clamp(plMath::Ln(fValue / info.m_fMin) / plMath::Ln(info.m_fMax / info.m_fMin), 0.0f, 1.0f);

    return plMath::Clamp((fValue - info.m_fMin) / plMath::Max(info.m_fMax - info.m_fMin, 0.0001f), 0.0f, 1.0f);
  }

  float FractionToParam(const plAudioEffectParameterInfo& info, float fFraction)
  {
    float fValue = 0.0f;
    if (info.m_bLogarithmic && info.m_fMin > 0.0f)
      fValue = info.m_fMin * plMath::Pow(info.m_fMax / info.m_fMin, fFraction);
    else
      fValue = plMath::Lerp(info.m_fMin, info.m_fMax, fFraction);

    if (info.m_fStep > 0.0f)
      fValue = plMath::RoundToMultiple(fValue, info.m_fStep);

    return plMath::Clamp(fValue, info.m_fMin, info.m_fMax);
  }

  /// "-24.0 dB", "1800 Hz", "35 %" in the parameter's display units.
  void FormatParam(const plAudioEffectParameterInfo& info, float fValue, plStringBuilder& out_sText)
  {
    const float fShown = fValue * info.m_fDisplayScale;
    const plUInt32 uiDecimals = (info.m_fStep * info.m_fDisplayScale >= 1.0f || plStringView(info.m_szUnit) == "Hz") ? 0 : 1;

    if (plStringView(info.m_szUnit) == "Hz" && fShown >= 1000.0f)
      out_sText.SetFormat("{} kHz", plArgF(fShown / 1000.0f, 1));
    else if (plStringView(info.m_szUnit).IsEmpty())
      out_sText.SetFormat("{}", plArgF(fShown, uiDecimals));
    else
      out_sText.SetFormat("{} {}", plArgF(fShown, uiDecimals), info.m_szUnit);
  }

  /// Gain in dB of an EQ or filter at a frequency: the shelves and the peak of the mixer's EQ, or the slope of a
  /// low or high pass of the given order. An approximation for drawing, not the filter's exact response.
  float CurveGainAt(plAudioEffectType::Enum type, const float* pParams, float fFrequency)
  {
    switch (type)
    {
      case plAudioEffectType::Eq3:
      {
        const float fLow = pParams[0] / (1.0f + plMath::Square(fFrequency / plMath::Max(pParams[1], 1.0f)));
        const float fMid = pParams[2] * plMath::Exp(-plMath::Square(plMath::Log2(fFrequency / plMath::Max(pParams[3], 1.0f)) * plMath::Max(pParams[4], 0.1f)));
        const float fHigh = pParams[5] / (1.0f + plMath::Square(plMath::Max(pParams[6], 1.0f) / fFrequency));
        return fLow + fMid + fHigh;
      }

      case plAudioEffectType::LowPass:
        return -10.0f * plMath::Log10(1.0f + plMath::Pow(fFrequency / plMath::Max(pParams[0], 1.0f), 2.0f * plMath::Max(pParams[1], 1.0f)));

      case plAudioEffectType::HighPass:
        return -10.0f * plMath::Log10(1.0f + plMath::Pow(plMath::Max(pParams[0], 1.0f) / fFrequency, 2.0f * plMath::Max(pParams[1], 1.0f)));

      default:
        return 0.0f;
    }
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// plEditorUiAudioMixerSnapshotRail

plEditorUiAudioMixerSnapshotRail::plEditorUiAudioMixerSnapshotRail(plDocument* pDocument, const plAudioMixerConsoleState* pState)
  : plEditorUiContent("AudioMixerSnapshotRail")
  , m_pDocument(pDocument)
  , m_pState(pState)
{
  if (IsReady())
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioMixerSnapshotRail::OnPointer, this));
}

plEditorUiAudioMixerSnapshotRail::~plEditorUiAudioMixerSnapshotRail()
{
  if (IsReady())
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
}

float plEditorUiAudioMixerSnapshotRail::GetHeight()
{
  return s_fRailHeader + s_uiRailVisibleRows * s_fRailRow + 4.0f;
}

void plEditorUiAudioMixerSnapshotRail::Refresh()
{
  m_bDrawDirty = true;
}

void plEditorUiAudioMixerSnapshotRail::OnUiUpdate()
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

void plEditorUiAudioMixerSnapshotRail::BuildDocument()
{
  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();

  plUiPanel* pRoot = plEditorUi::Panel();
  pRoot->m_Color = plEditorUi::Surface();
  plEditorUi::DrawList(pRoot, "Mixer.Rail", s_sRailEntry.GetView());

  SetDocument(plEditorUi::Finish(pRoot));
  m_bDrawDirty = true;
}

void plEditorUiAudioMixerSnapshotRail::Draw()
{
  using namespace plEditorUi;

  m_bDrawDirty = false;
  m_Writer.Clear();
  m_Rows.Clear();

  const plVec2 size = GetSurface()->GetLogicalSize();

  plHybridArray<const plDocumentObject*, 8> snapshots;
  GatherChildren(GetMixerProperties(m_pDocument), "Snapshots", snapshots);

  // header: the title and what the rail does
  m_Writer.Rect(plRectFloat(0.0f, 0.0f, size.x, 1.0f), Line());
  m_Writer.Text(plRectFloat(s_fPad, 0.0f, 90.0f, s_fRailHeader), "Snapshots", Foreground(), FontSize - 1);
  m_Writer.Text(plRectFloat(s_fPad + 78.0f, 0.0f, size.x - 2.0f * s_fPad - 78.0f, s_fRailHeader), "intensity is live; higher priority wins on conflicts", Dim(), FontSize - 2);

  const float fMaxScroll = plMath::Max(0.0f, snapshots.GetCount() * s_fRailRow - (size.y - s_fRailHeader));
  m_fScrollY = plMath::Clamp(m_fScrollY, 0.0f, fMaxScroll);

  if (snapshots.IsEmpty())
  {
    m_Writer.Text(plRectFloat(s_fPad, s_fRailHeader, size.x - 2.0f * s_fPad, s_fRailRow), "No snapshots - '+ Snapshot' adds one.", Dim(), FontSize - 1);
    m_Writer.Publish(GetSurface()->GetBlackboard(), s_sRailEntry);
    return;
  }

  // columns: name, priority, intensity slider, value, Capture, overrides
  const float fNameX = s_fPad;
  const float fPrioX = fNameX + 150.0f;
  const float fSliderX = fPrioX + 54.0f;
  const float fSliderWidth = plMath::Clamp(size.x * 0.22f, 90.0f, 220.0f);
  const float fValueX = fSliderX + fSliderWidth + 8.0f;
  const float fCaptureX = fValueX + 40.0f;
  const float fCaptureWidth = plAudioEngineUi::TextButtonWidth("Capture", false);
  const float fSummaryX = fCaptureX + fCaptureWidth + 10.0f;

  const plDocumentObject* pSelected = m_pDocument->GetSelectionManager()->GetCurrentObject();

  m_Writer.PushClip(plRectFloat(0.0f, s_fRailHeader, size.x, size.y - s_fRailHeader));

  float y = s_fRailHeader - m_fScrollY;
  for (plUInt32 i = 0; i < snapshots.GetCount(); ++i, y += s_fRailRow)
  {
    const plDocumentObject* pSnapshot = snapshots[i];
    const auto& accessor = pSnapshot->GetTypeAccessor();

    const plRectFloat row(0.0f, y, size.x, s_fRailRow);
    if (pSnapshot == pSelected)
      m_Writer.Rect(row, Selection());

    plString sName = accessor.GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      sName = "<unnamed>";

    const plRectFloat nameRect(fNameX, y, fPrioX - fNameX - 6.0f, s_fRailRow);
    plAudioEngineUi::DotText(m_Writer, nameRect, sName, plAudioEngineUi::AudioColor(), Foreground());

    plStringBuilder sPrio;
    sPrio.SetFormat("prio {}", accessor.GetValue("Priority").ConvertTo<plInt64>());
    m_Writer.Text(plRectFloat(fPrioX, y, 50.0f, s_fRailRow), sPrio, Dim(), FontSize - 2);

    // the intensity that plays (or the one being dragged)
    const bool bDraggingThis = m_bDragging && m_DragGuid == pSnapshot->GetGuid();
    float fIntensity = (m_pState != nullptr && i < m_pState->m_SnapshotIntensity.GetCount()) ? m_pState->m_SnapshotIntensity[i] : 0.0f;
    if (bDraggingThis)
      fIntensity = m_fDragValue;

    const plRectFloat slider(fSliderX, y + (s_fRailRow - 14.0f) * 0.5f, fSliderWidth, 14.0f);
    plAudioEngineUi::Slider(m_Writer, slider, fIntensity, m_pState != nullptr && m_pState->m_bLive, bDraggingThis);

    plStringBuilder sValue;
    sValue.SetFormat("{}", plArgF(fIntensity, 2));
    m_Writer.Text(plRectFloat(fValueX, y, 36.0f, s_fRailRow), sValue, Muted(), FontSize - 2, plUiTextHAlign::Left, plUiTextVAlign::Center, true);

    const plRectFloat capture(fCaptureX, y + (s_fRailRow - ControlHeight) * 0.5f, fCaptureWidth, ControlHeight);
    plAudioEngineUi::TextButton(m_Writer, capture, "Capture");

    // "Music +3 · Ambience -6": the overrides by bus
    plHybridArray<const plDocumentObject*, 8> overrides;
    GatherChildren(pSnapshot, "Overrides", overrides);

    plStringBuilder sSummary;
    for (const plDocumentObject* pOverride : overrides)
    {
      const auto& ovr = pOverride->GetTypeAccessor();
      if (!sSummary.IsEmpty())
        sSummary.Append(" \xC2\xB7 ");

      const plInt64 iTarget = ovr.GetValue("Target").ConvertTo<plInt64>();
      const plString sBus = ovr.GetValue("Bus").ConvertTo<plString>();
      const float fValue = ovr.GetValue("Value").ConvertTo<float>();

      if (iTarget == plAudioSnapshotTarget::BusVolume)
        sSummary.AppendFormat("{} {} dB", sBus, plArgF(fValue, 1));
      else if (iTarget == plAudioSnapshotTarget::EffectParam)
        sSummary.AppendFormat("{} {}.{}", sBus, ovr.GetValue("Effect").ConvertTo<plString>(), ovr.GetValue("Parameter").ConvertTo<plString>());
      else if (iTarget == plAudioSnapshotTarget::SendLevel)
        sSummary.AppendFormat("{} send {}", sBus, ovr.GetValue("Send").ConvertTo<plString>());
      else
        sSummary.AppendFormat("{} pitch", sBus);
    }

    if (sSummary.IsEmpty())
      sSummary = "no overrides yet";

    m_Writer.Text(plRectFloat(fSummaryX, y, plMath::Max(size.x - fSummaryX - s_fPad, 0.0f), s_fRailRow), sSummary, Dim(), FontSize - 2);

    Row& r = m_Rows.ExpandAndGetRef();
    r.m_Guid = pSnapshot->GetGuid();
    r.m_Name = nameRect;
    r.m_Slider = slider;
    r.m_Capture = capture;
  }

  m_Writer.PopClip();
  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sRailEntry);
}

void plEditorUiAudioMixerSnapshotRail::OnPointer(const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;

  switch (e.m_Type)
  {
    case Type::Wheel:
      m_fScrollY -= e.m_fWheel * s_fRailRow;
      m_bDrawDirty = true;
      return;

    case Type::Press:
    {
      if (e.m_Button != plEditorUiPointerEvent::Button::Left)
        return;

      for (const Row& row : m_Rows)
      {
        const plRectFloat sliderGrab(row.m_Slider.x - 4.0f, row.m_Slider.y - 6.0f, row.m_Slider.width + 8.0f, row.m_Slider.height + 12.0f);

        if (sliderGrab.Contains(e.m_vPosition))
        {
          m_bDragging = true;
          m_DragGuid = row.m_Guid;
          m_DragSlider = row.m_Slider;
          m_fDragValue = plAudioEngineUi::SliderFractionAt(m_DragSlider, e.m_vPosition.x);
          if (m_OnIntensity.IsValid())
            m_OnIntensity(m_DragGuid, m_fDragValue);
          m_bDrawDirty = true;
          return;
        }

        if (row.m_Capture.Contains(e.m_vPosition))
        {
          if (m_OnCapture.IsValid())
            m_OnCapture(row.m_Guid);
          return;
        }

        if (row.m_Name.Contains(e.m_vPosition))
        {
          if (const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(row.m_Guid))
            m_pDocument->GetSelectionManager()->SetSelection(pObject);
          return;
        }
      }
      return;
    }

    case Type::Move:
    case Type::DragStart:
    case Type::DragMove:
      if (!m_bDragging)
        return;

      m_fDragValue = plAudioEngineUi::SliderFractionAt(m_DragSlider, e.m_vPosition.x);
      if (m_OnIntensity.IsValid())
        m_OnIntensity(m_DragGuid, m_fDragValue);
      m_bDrawDirty = true;
      return;

    case Type::Release:
    case Type::Drop:
    case Type::DragCancel:
      // the intensity is an audition, not part of the document: it stays where it was let go
      m_bDragging = false;
      m_bDrawDirty = true;
      return;

    default:
      return;
  }
}

//////////////////////////////////////////////////////////////////////////
// plEditorUiAudioMixerInsertEditor

plEditorUiAudioMixerInsertEditor::plEditorUiAudioMixerInsertEditor(plDocument* pDocument, const plAudioMixerConsoleState* pState)
  : plEditorUiContent("AudioMixerInsert")
  , m_pDocument(pDocument)
  , m_pState(pState)
{
  if (IsReady())
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioMixerInsertEditor::OnPointer, this));
}

plEditorUiAudioMixerInsertEditor::~plEditorUiAudioMixerInsertEditor()
{
  if (IsReady())
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
}

void plEditorUiAudioMixerInsertEditor::Refresh()
{
  // the bus name field is part of the document, so switching between a bus and an insert rebuilds it
  const plUuid bus = m_pState != nullptr ? m_pState->m_SelectedBus : plUuid();
  const plInt32 iEffect = m_pState != nullptr ? m_pState->m_iSelectedEffect : -1;

  if (bus != m_BuiltBus || (iEffect < 0) != (m_iBuiltEffect < 0))
    m_bRebuild = true;

  m_bDrawDirty = true;
}

const plDocumentObject* plEditorUiAudioMixerInsertEditor::GetBus() const
{
  if (m_pState == nullptr || !m_pState->m_SelectedBus.IsValid())
    return nullptr;

  return m_pDocument->GetObjectManager()->GetObject(m_pState->m_SelectedBus);
}

const plDocumentObject* plEditorUiAudioMixerInsertEditor::GetEffect() const
{
  const plDocumentObject* pBus = GetBus();
  if (pBus == nullptr || m_pState->m_iSelectedEffect < 0)
    return nullptr;

  plHybridArray<const plDocumentObject*, 4> effects;
  GatherChildren(pBus, "Effects", effects);

  return static_cast<plUInt32>(m_pState->m_iSelectedEffect) < effects.GetCount() ? effects[m_pState->m_iSelectedEffect] : nullptr;
}

plUInt32 plEditorUiAudioMixerInsertEditor::GetBusIndex() const
{
  plHybridArray<const plDocumentObject*, 12> buses;
  GatherChildren(GetMixerProperties(m_pDocument), "Buses", buses);

  const plDocumentObject* pBus = GetBus();
  for (plUInt32 i = 0; i < buses.GetCount(); ++i)
  {
    if (buses[i] == pBus)
      return i;
  }

  return plInvalidIndex;
}

void plEditorUiAudioMixerInsertEditor::OnUiUpdate()
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

void plEditorUiAudioMixerInsertEditor::BuildDocument()
{
  using namespace plEditorUi;

  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();
  m_BuiltBus = m_pState != nullptr ? m_pState->m_SelectedBus : plUuid();
  m_iBuiltEffect = m_pState != nullptr ? m_pState->m_iSelectedEffect : -1;

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = Surface();
  DrawList(pRoot, "Mixer.Insert", s_sInsertEntry.GetView());

  // a bus gets its name field; Enter renames the bus and everything that refers to it
  if (const plDocumentObject* pBus = GetBus(); pBus != nullptr && m_iBuiltEffect < 0)
  {
    TextField(pRoot, plRectFloat(s_fPad + 60.0f, 104.0f, plMath::Max(m_vBuiltSize.x - 2.0f * s_fPad - 60.0f, 60.0f), ControlHeight), "Insert.Name", "Insert.Name.Text", "Bus name", "Insert.Name.Submit");
    GetSurface()->GetBlackboard().SetEntryValue("Insert.Name.Text", pBus->GetTypeAccessor().GetValue("Name").ConvertTo<plString>());
  }

  SetDocument(Finish(pRoot));
  m_bDrawDirty = true;
}

void plEditorUiAudioMixerInsertEditor::OnUiEvent(const plMsgUiEvent& event)
{
  if (event.m_sIdentifier.GetView() != "Insert.Name.Submit")
    return;

  const plDocumentObject* pBus = GetBus();
  if (pBus == nullptr)
    return;

  const plVariant text = GetSurface()->GetBlackboard().GetEntryValue(plTempHashedString("Insert.Name.Text"));
  if (!text.IsA<plString>() || text.Get<plString>().IsEmpty())
    return;

  if (m_OnRename.IsValid())
    m_OnRename(pBus->GetGuid(), text.Get<plString>());
}

float plEditorUiAudioMixerInsertEditor::GetParam(const plDocumentObject* pEffect, plUInt32 uiParam) const
{
  const auto& accessor = pEffect->GetTypeAccessor();
  if (static_cast<plInt32>(uiParam) < accessor.GetCount("Params"))
    return accessor.GetValue("Params", uiParam).ConvertTo<float>();

  // parameters the document does not store yet use their defaults
  const auto eType = static_cast<plAudioEffectType::Enum>(accessor.GetValue("Type").ConvertTo<plInt64>());
  const plArrayPtr<const plAudioEffectParameterInfo> parameters = plAudioEffectRegistry::GetTypeInfo(eType).m_Parameters;
  return uiParam < parameters.GetCount() ? parameters[uiParam].m_fDefault : 0.0f;
}

void plEditorUiAudioMixerInsertEditor::CommitParam(const plDocumentObject* pEffect, plUInt32 uiParam, float fValue)
{
  const auto& typeAccessor = pEffect->GetTypeAccessor();
  const auto eType = static_cast<plAudioEffectType::Enum>(typeAccessor.GetValue("Type").ConvertTo<plInt64>());
  const plAudioEffectParameterInfo& info = plAudioEffectRegistry::GetTypeInfo(eType).m_Parameters[uiParam];

  plStringBuilder sTransaction("Set ", info.m_szName);

  plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
  accessor.StartTransaction(sTransaction);

  // the array holds every parameter up to the edited one, so the slots stay in registry order
  for (plInt32 i = typeAccessor.GetCount("Params"); i <= static_cast<plInt32>(uiParam); ++i)
  {
    accessor.InsertValueByName(pEffect, "Params", GetParam(pEffect, i), i).IgnoreResult();
  }

  accessor.SetValueByName(pEffect, "Params", fValue, uiParam).IgnoreResult();
  accessor.FinishTransaction();
}

void plEditorUiAudioMixerInsertEditor::Draw()
{
  using namespace plEditorUi;

  m_bDrawDirty = false;
  m_Writer.Clear();
  m_ParamRows.Clear();
  m_BypassButton = plRectFloat(0, 0, 0, 0);
  m_RemoveButton = plRectFloat(0, 0, 0, 0);
  m_SidechainField = plRectFloat(0, 0, 0, 0);

  const plVec2 size = GetSurface()->GetLogicalSize();

  const plDocumentObject* pBus = GetBus();
  const plDocumentObject* pEffect = GetEffect();

  if (pBus == nullptr)
  {
    m_Writer.Text(plRectFloat(s_fPad, 10.0f, size.x - 2.0f * s_fPad, 20.0f), "Select a bus or one of its inserts.", Dim(), FontSize - 1);
  }
  else if (pEffect == nullptr)
  {
    DrawBus(pBus, size.x);
  }
  else
  {
    DrawEffect(pBus, pEffect, size.x);
  }

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sInsertEntry);
}

void plEditorUiAudioMixerInsertEditor::DrawBus(const plDocumentObject* pBus, float fWidth)
{
  using namespace plEditorUi;

  const auto& accessor = pBus->GetTypeAccessor();
  const plString sName = accessor.GetValue("Name").ConvertTo<plString>();
  const bool bMaster = GetBusIndex() == 0;

  plAudioEngineUi::DotText(m_Writer, plRectFloat(s_fPad, 6.0f, fWidth - 2.0f * s_fPad, 22.0f), sName, plAudioEngineUi::AudioColor(), Foreground(), FontSize);

  plStringBuilder sOutput;
  if (bMaster)
    sOutput = "Output \xE2\x86\x92 device";
  else
    sOutput.SetFormat("Output \xE2\x86\x92 {}", accessor.GetValue("Parent").ConvertTo<plString>().IsEmpty() ? plStringView("Master") : accessor.GetValue("Parent").ConvertTo<plString>().GetView());
  m_Writer.Text(plRectFloat(s_fPad, 28.0f, fWidth - 2.0f * s_fPad, 18.0f), sOutput, Muted(), FontSize - 2);

  m_Writer.Text(plRectFloat(s_fPad, 52.0f, fWidth - 2.0f * s_fPad, 16.0f), "Select an insert to edit it. Renaming the bus updates", Dim(), FontSize - 2);
  m_Writer.Text(plRectFloat(s_fPad, 68.0f, fWidth - 2.0f * s_fPad, 16.0f), "its children, sends, sidechains, VCAs, snapshots and events.", Dim(), FontSize - 2);

  m_Writer.Text(plRectFloat(s_fPad, 104.0f, 56.0f, ControlHeight), "Name", Muted(), FontSize - 1);
}

void plEditorUiAudioMixerInsertEditor::DrawCurve(const plRectFloat& rect, plAudioEffectType::Enum type, const float* pParams)
{
  using namespace plEditorUi;

  m_Writer.Rect(rect, Field(), 3.0f);
  m_Writer.Line(plVec2(rect.x, rect.y + rect.height * 0.5f + 0.5f), plVec2(rect.x + rect.width, rect.y + rect.height * 0.5f + 0.5f), Line());

  // 20 Hz to 20 kHz on a log axis, +-12 dB
  plHybridArray<plVec2, 128> points;
  const plUInt32 uiSteps = 96;
  for (plUInt32 i = 0; i <= uiSteps; ++i)
  {
    const float fT = static_cast<float>(i) / uiSteps;
    const float fFrequency = 20.0f * plMath::Pow(1000.0f, fT);
    const float fGain = plMath::Clamp(CurveGainAt(type, pParams, fFrequency), -12.0f, 12.0f);
    points.PushBack(plVec2(rect.x + fT * rect.width, rect.y + rect.height * 0.5f - fGain / 12.0f * (rect.height * 0.5f - 6.0f)));
  }

  m_Writer.Polyline(points, plAudioEngineUi::AudioColor(), 1.6f);
  m_Writer.Text(plRectFloat(rect.x + 4.0f, rect.y + rect.height - 14.0f, 60.0f, 12.0f), "20 Hz", Dim(), MinFontSize);
  m_Writer.Text(plRectFloat(rect.x + rect.width - 64.0f, rect.y + rect.height - 14.0f, 60.0f, 12.0f), "20 kHz", Dim(), MinFontSize, plUiTextHAlign::Right);
  m_Writer.Text(plRectFloat(rect.x + 4.0f, rect.y + 2.0f, 40.0f, 12.0f), "+12", Dim(), MinFontSize);
}

void plEditorUiAudioMixerInsertEditor::DrawEffect(const plDocumentObject* pBus, const plDocumentObject* pEffect, float fWidth)
{
  using namespace plEditorUi;

  const auto& effectAccessor = pEffect->GetTypeAccessor();
  const auto eType = static_cast<plAudioEffectType::Enum>(effectAccessor.GetValue("Type").ConvertTo<plInt64>());
  const plAudioEffectTypeInfo& info = plAudioEffectRegistry::GetTypeInfo(eType);
  const bool bBypass = effectAccessor.GetValue("Bypass").ConvertTo<bool>();

  // header: "Music · Compressor", Bypass, Remove
  {
    plStringBuilder sTitle;
    sTitle.SetFormat("{} \xC2\xB7 {}", pBus->GetTypeAccessor().GetValue("Name").ConvertTo<plString>(), info.m_szName);

    const float fRemoveWidth = plAudioEngineUi::TextButtonWidth("Remove", false);
    const float fBypassWidth = plAudioEngineUi::TextButtonWidth("Bypass", false);
    m_RemoveButton = plRectFloat(fWidth - s_fPad - fRemoveWidth, 6.0f, fRemoveWidth, ControlHeight);
    m_BypassButton = plRectFloat(m_RemoveButton.x - 6.0f - fBypassWidth, 6.0f, fBypassWidth, ControlHeight);

    plAudioEngineUi::DotText(m_Writer, plRectFloat(s_fPad, 6.0f, m_BypassButton.x - s_fPad - 6.0f, ControlHeight), sTitle, plAudioEngineUi::AudioColor(), Foreground(), FontSize - 1);

    if (bBypass)
    {
      m_Writer.Rect(m_BypassButton, plAudioEngineUi::WithAlpha(Warning(), 60), 4.0f);
      m_Writer.RectOutline(m_BypassButton, Warning(), 1.0f, 4.0f);
      m_Writer.Text(m_BypassButton, "Bypass", Foreground(), FontSize - 1, plUiTextHAlign::Center);
    }
    else
    {
      plAudioEngineUi::TextButton(m_Writer, m_BypassButton, "Bypass");
    }

    plAudioEngineUi::TextButton(m_Writer, m_RemoveButton, "Remove");
  }

  float y = 6.0f + ControlHeight + 10.0f;

  // the current parameters, with the dragged one
  float params[plAudioEffectMaxParams] = {};
  for (plUInt32 p = 0; p < info.m_Parameters.GetCount() && p < plAudioEffectMaxParams; ++p)
  {
    params[p] = (m_iDragParam == static_cast<plInt32>(p)) ? m_fDragValue : GetParam(pEffect, p);
  }

  if (eType == plAudioEffectType::Eq3 || eType == plAudioEffectType::LowPass || eType == plAudioEffectType::HighPass)
  {
    DrawCurve(plRectFloat(s_fPad, y, fWidth - 2.0f * s_fPad, 90.0f), eType, params);
    y += 98.0f;
  }

  if (info.m_bUsesSidechain)
  {
    const plString sKey = effectAccessor.GetValue("SidechainBus").ConvertTo<plString>();

    m_Writer.Text(plRectFloat(s_fPad, y, 92.0f, ControlHeight), "Sidechain key", Muted(), FontSize - 2);
    m_SidechainField = plRectFloat(s_fPad + 96.0f, y, fWidth - 2.0f * s_fPad - 96.0f, ControlHeight);
    plAudioEngineUi::Dropdown(m_Writer, m_SidechainField, {}, sKey.IsEmpty() ? plStringView("self") : sKey.GetView());
    y += ControlHeight + 6.0f;

    // gain reduction of the effect in the engine process
    float fReduction = 0.0f;
    const plUInt32 uiBus = GetBusIndex();
    if (m_pState != nullptr && uiBus < m_pState->m_EffectGainReduction.GetCount() && static_cast<plUInt32>(m_pState->m_iSelectedEffect) < m_pState->m_EffectGainReduction[uiBus].GetCount())
      fReduction = m_pState->m_EffectGainReduction[uiBus][m_pState->m_iSelectedEffect];

    m_Writer.Text(plRectFloat(s_fPad, y, 92.0f, ControlHeight), "Gain reduction", Muted(), FontSize - 2);
    const plRectFloat meter(s_fPad + 96.0f, y + 7.0f, fWidth - 2.0f * s_fPad - 96.0f - 60.0f, 8.0f);
    plAudioEngineUi::ValueBar(m_Writer, meter, plMath::Clamp(-fReduction / 12.0f, 0.0f, 1.0f), Warning());

    plStringBuilder sReduction;
    sReduction.SetFormat("{} dB", plArgF(fReduction, 1));
    m_Writer.Text(plRectFloat(meter.x + meter.width + 6.0f, y, 54.0f, ControlHeight), sReduction, Muted(), FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
    y += ControlHeight + 6.0f;

    if (info.m_bNeedsSidechain && sKey.IsEmpty())
    {
      m_Writer.Text(plRectFloat(s_fPad, y, fWidth - 2.0f * s_fPad, 16.0f), "Choose the key bus; without one the ducker does nothing.", Warning(), FontSize - 2);
      y += 20.0f;
    }
  }

  // the named parameters: label, slider, value with unit
  for (plUInt32 p = 0; p < info.m_Parameters.GetCount() && p < plAudioEffectMaxParams; ++p)
  {
    const plAudioEffectParameterInfo& param = info.m_Parameters[p];
    const bool bDraggingThis = m_iDragParam == static_cast<plInt32>(p);

    m_Writer.Text(plRectFloat(s_fPad, y, 84.0f, s_fParamRow), param.m_szName, Muted(), FontSize - 2);

    const plRectFloat slider(s_fPad + 88.0f, y + (s_fParamRow - 14.0f) * 0.5f, plMath::Max(fWidth - 2.0f * s_fPad - 88.0f - 70.0f, 40.0f), 14.0f);
    plAudioEngineUi::Slider(m_Writer, slider, ParamToFraction(param, params[p]), !bBypass, bDraggingThis);

    plStringBuilder sValue;
    FormatParam(param, params[p], sValue);
    m_Writer.Text(plRectFloat(slider.x + slider.width + 6.0f, y, 64.0f, s_fParamRow), sValue, Foreground(), FontSize - 2, plUiTextHAlign::Right, plUiTextVAlign::Center, true);

    ParamRow& row = m_ParamRows.ExpandAndGetRef();
    row.m_uiParam = p;
    row.m_Slider = slider;

    y += s_fParamRow;
  }

  y += 8.0f;
  m_Writer.Text(plRectFloat(s_fPad, y, fWidth - 2.0f * s_fPad, 16.0f), "Named, typed parameters from the effect registry.", Dim(), FontSize - 2);
  m_Writer.Text(plRectFloat(s_fPad, y + 16.0f, fWidth - 2.0f * s_fPad, 16.0f), "Snapshots address them by name, so inserts can move.", Dim(), FontSize - 2);
}

void plEditorUiAudioMixerInsertEditor::ShowSidechainMenu(const plVec2& vPosition)
{
  const plDocumentObject* pEffect = GetEffect();
  const plDocumentObject* pBus = GetBus();
  if (pEffect == nullptr || pBus == nullptr)
    return;

  plHybridArray<const plDocumentObject*, 12> buses;
  GatherChildren(GetMixerProperties(m_pDocument), "Buses", buses);

  const plString sCurrent = pEffect->GetTypeAccessor().GetValue("SidechainBus").ConvertTo<plString>();

  plEditorUiMenu menu;
  plHybridArray<plString, 12> names;

  plEditorUiMenuItem& self = menu.AddAction("self (own input)", 0);
  self.m_bCheckable = true;
  self.m_bChecked = sCurrent.IsEmpty();
  names.PushBack(plString());

  for (const plDocumentObject* pOther : buses)
  {
    if (pOther == pBus)
      continue;

    const plString sName = pOther->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
    plEditorUiMenuItem& item = menu.AddAction(sName, names.GetCount());
    item.m_bCheckable = true;
    item.m_bChecked = sCurrent.IsEqual_NoCase(sName);
    names.PushBack(sName);
  }

  const plUuid effectGuid = pEffect->GetGuid();
  plEditorUiMenuView::Show(*GetSurface(), vPosition, menu, [this, names, effectGuid](plUInt32 uiId)
    {
      const plDocumentObject* pTarget = m_pDocument->GetObjectManager()->GetObject(effectGuid);
      if (pTarget == nullptr || uiId >= names.GetCount())
        return;

      plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
      accessor.StartTransaction("Set Sidechain Key");
      accessor.SetValueByName(pTarget, "SidechainBus", names[uiId]).IgnoreResult();
      accessor.FinishTransaction(); });
}

void plEditorUiAudioMixerInsertEditor::OnPointer(const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;

  const plDocumentObject* pEffect = GetEffect();

  auto DragTo = [&](float x)
  {
    const auto eType = static_cast<plAudioEffectType::Enum>(pEffect->GetTypeAccessor().GetValue("Type").ConvertTo<plInt64>());
    m_fDragValue = FractionToParam(plAudioEffectRegistry::GetTypeInfo(eType).m_Parameters[m_iDragParam], plAudioEngineUi::SliderFractionAt(m_DragSlider, x));
    m_bDrawDirty = true;

    if (m_OnLiveEdit.IsValid())
    {
      plAudioMixerLiveEdit edit;
      edit.m_Type = plAudioMixerLiveEdit::Type::EffectParam;
      edit.m_uiBus = GetBusIndex();
      edit.m_uiIndex = static_cast<plUInt32>(m_pState->m_iSelectedEffect);
      edit.m_uiParam = static_cast<plUInt32>(m_iDragParam);
      edit.m_fValue = m_fDragValue;
      m_OnLiveEdit(edit);
    }
  };

  switch (e.m_Type)
  {
    case Type::Press:
    {
      if (e.m_Button != plEditorUiPointerEvent::Button::Left || pEffect == nullptr)
        return;

      if (m_BypassButton.Contains(e.m_vPosition))
      {
        plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
        accessor.StartTransaction("Toggle Bypass");
        accessor.SetValueByName(pEffect, "Bypass", !pEffect->GetTypeAccessor().GetValue("Bypass").ConvertTo<bool>()).IgnoreResult();
        accessor.FinishTransaction();
        return;
      }

      if (m_RemoveButton.Contains(e.m_vPosition))
      {
        plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
        accessor.StartTransaction("Remove Insert");
        if (accessor.RemoveObject(pEffect).Failed())
        {
          accessor.CancelTransaction();
          return;
        }
        accessor.FinishTransaction();
        return;
      }

      if (m_SidechainField.Contains(e.m_vPosition))
      {
        ShowSidechainMenu(plVec2(m_SidechainField.x, m_SidechainField.y + m_SidechainField.height));
        return;
      }

      for (const ParamRow& row : m_ParamRows)
      {
        const plRectFloat grab(row.m_Slider.x - 4.0f, row.m_Slider.y - 6.0f, row.m_Slider.width + 8.0f, row.m_Slider.height + 12.0f);
        if (!grab.Contains(e.m_vPosition))
          continue;

        m_iDragParam = static_cast<plInt32>(row.m_uiParam);
        m_DragSlider = row.m_Slider;
        DragTo(e.m_vPosition.x);
        return;
      }
      return;
    }

    case Type::Move:
    case Type::DragStart:
    case Type::DragMove:
      if (m_iDragParam >= 0 && pEffect != nullptr)
        DragTo(e.m_vPosition.x);
      return;

    case Type::Release:
    case Type::Drop:
    {
      if (m_iDragParam < 0)
        return;

      const plUInt32 uiParam = static_cast<plUInt32>(m_iDragParam);
      m_iDragParam = -1;
      m_bDrawDirty = true;

      // one step per drag
      if (pEffect != nullptr && plMath::Abs(GetParam(pEffect, uiParam) - m_fDragValue) > 0.00001f)
        CommitParam(pEffect, uiParam, m_fDragValue);
      return;
    }

    case Type::DragCancel:
      m_iDragParam = -1;
      m_bDrawDirty = true;
      return;

    default:
      return;
  }
}
