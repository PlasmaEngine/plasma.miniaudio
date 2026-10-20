#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <AudioSystem/Mixer/AudioMixerData.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerViews.h>
#include <EditorPluginPlasmaAudio/Preview/AudioPreviewService.h>
#include <EditorUi/Drawings.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

namespace
{
  constexpr float s_fStripWidth = 128.0f;
  constexpr float s_fStripGap = 8.0f;
  constexpr float s_fMargin = 10.0f;
  constexpr float s_fStripHeader = 26.0f;
  constexpr float s_fSectionHeight = 58.0f;
  constexpr float s_fFaderHeight = 160.0f;
  constexpr float s_fWheelStep = 40.0f;
  constexpr float s_fSnapshotRow = 40.0f;

  const plHashedString s_sStripsEntry = plMakeHashedString("Mixer.Strips");
  const plHashedString s_sSnapshotsEntry = plMakeHashedString("Mixer.Snapshots");

  template <typename ARRAY>
  void GatherChildrenOfProperty(const plDocumentObject* pParent, const char* szProperty, ARRAY& out_children)
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

  const char* EffectTypeName(plInt64 iType)
  {
    switch (iType)
    {
      case plAudioEffectType::LowPass:
        return "LowPass";
      case plAudioEffectType::HighPass:
        return "HighPass";
      case plAudioEffectType::Eq3:
        return "EQ3";
      case plAudioEffectType::Delay:
        return "Delay";
      case plAudioEffectType::Reverb:
        return "Reverb";
      case plAudioEffectType::Compressor:
        return "Compressor";
      case plAudioEffectType::Limiter:
        return "Limiter";
      default:
        return "?";
    }
  }

  /// "+1.5 dB", "-∞ dB" at the bottom of the fader.
  void FormatVolume(float fDb, plStringBuilder& out_sText)
  {
    if (fDb <= plEditorUiDraw::MeterMinDb)
      out_sText = "-\xE2\x88\x9E dB";
    else
      out_sText.SetFormat("{}{} dB", fDb >= 0.0f ? "+" : "", plArgF(fDb, 1));
  }

  /// The fader's 0.5 dB steps with a detent at 0 dB.
  float SnapVolume(float fDb)
  {
    if (plMath::Abs(fDb) < 0.75f)
      return 0.0f;

    return plMath::Clamp(plMath::RoundToMultiple(fDb, 0.5f), -60.0f, 12.0f);
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// plQtAudioMixerStripsView

plQtAudioMixerStripsView::plQtAudioMixerStripsView(QWidget* pParent, plDocument* pDocument)
  : plQtEngineUiWidget(pParent, "AudioMixerStrips")
  , m_pDocument(pDocument)
{
  setMinimumSize(0, 0);

  if (IsReady())
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plQtAudioMixerStripsView::OnPointer, this));
}

plQtAudioMixerStripsView::~plQtAudioMixerStripsView()
{
  if (IsReady())
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
}

void plQtAudioMixerStripsView::Refresh()
{
  m_bDrawDirty = true;
}

void plQtAudioMixerStripsView::SetLiveMeters(bool bLive)
{
  m_bLiveMeters = bLive;
  m_bDrawDirty = true;
}

void plQtAudioMixerStripsView::UpdateMeters()
{
  m_bDrawDirty = true;
}

const plDocumentObject* plQtAudioMixerStripsView::GetPropertiesObject() const
{
  return GetMixerProperties(m_pDocument);
}

void plQtAudioMixerStripsView::GatherStrips(plHybridArray<const plDocumentObject*, 12>& out_buses, plHybridArray<const plDocumentObject*, 4>& out_vcas) const
{
  GatherChildrenOfProperty(GetPropertiesObject(), "Buses", out_buses);
  GatherChildrenOfProperty(GetPropertiesObject(), "VCAs", out_vcas);
}

float plQtAudioMixerStripsView::GetContentWidth() const
{
  plHybridArray<const plDocumentObject*, 12> buses;
  plHybridArray<const plDocumentObject*, 4> vcas;
  GatherStrips(buses, vcas);

  return 2.0f * s_fMargin + (buses.GetCount() + vcas.GetCount()) * (s_fStripWidth + s_fStripGap) + (vcas.IsEmpty() ? 0.0f : 9.0f);
}

void plQtAudioMixerStripsView::OnUiUpdate()
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

void plQtAudioMixerStripsView::BuildDocument()
{
  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();

  plUiPanel* pRoot = plEditorUi::Panel();
  pRoot->m_Color = plEditorUi::Surface();
  plEditorUi::DrawList(pRoot, "Mixer.Strips", s_sStripsEntry.GetView());

  SetDocument(plEditorUi::Finish(pRoot));
  m_bDrawDirty = true;
}

void plQtAudioMixerStripsView::Draw()
{
  using namespace plEditorUi;

  m_bDrawDirty = false;
  m_Writer.Clear();
  m_Strips.Clear();

  const plVec2 size = GetSurface()->GetLogicalSize();
  m_fScrollX = plMath::Clamp(m_fScrollX, 0.0f, plMath::Max(GetContentWidth() - size.x, 0.0f));

  plHybridArray<const plDocumentObject*, 12> buses;
  plHybridArray<const plDocumentObject*, 4> vcas;
  GatherStrips(buses, vcas);

  if (buses.IsEmpty() && vcas.IsEmpty())
  {
    const float cy = size.y * 0.5f;
    plAudioEngineUi::Icon(m_Writer, plRectFloat(size.x * 0.5f - 11.0f, cy - 46.0f, 22.0f, 22.0f), plAudioEngineUi::IconMixer, Dim());
    m_Writer.Text(plRectFloat(0.0f, cy - 18.0f, size.x, 18.0f), "No buses yet.", Muted(), FontSize, plUiTextHAlign::Center);
    m_Writer.Text(plRectFloat(0.0f, cy, size.x, 18.0f), "Use '+ Bus' to create the mixer - the first bus is the Master bus.", Dim(), FontSize - 1, plUiTextHAlign::Center);
    m_Writer.Publish(GetSurface()->GetBlackboard(), s_sStripsEntry);
    return;
  }

  const plUuid selected = m_pDocument->GetSelectionManager()->GetCurrentObject() != nullptr ? m_pDocument->GetSelectionManager()->GetCurrentObject()->GetGuid() : plUuid();
  const float fStripHeight = s_fStripHeader + 6.0f + 2.0f * s_fSectionHeight + 6.0f + s_fFaderHeight + 6.0f + 18.0f + 16.0f + 8.0f;

  float x = s_fMargin - m_fScrollX;
  for (plUInt32 i = 0; i < buses.GetCount(); ++i)
  {
    DrawStrip(plRectFloat(x, s_fMargin, s_fStripWidth, fStripHeight), buses[i], i, false, i == 0, buses[i]->GetGuid() == selected);
    x += s_fStripWidth + s_fStripGap;
  }

  if (!vcas.IsEmpty())
  {
    // a dashed divider between the buses and the VCAs
    m_Writer.Line(plVec2(x + 0.5f, s_fMargin), plVec2(x + 0.5f, s_fMargin + fStripHeight), Border(), 1.0f, 3.0f, 3.0f);
    x += 9.0f;
  }

  for (plUInt32 i = 0; i < vcas.GetCount(); ++i)
  {
    DrawStrip(plRectFloat(x, s_fMargin, s_fStripWidth, fStripHeight), vcas[i], plInvalidIndex, true, false, vcas[i]->GetGuid() == selected);
    x += s_fStripWidth + s_fStripGap;
  }

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sStripsEntry);
}

void plQtAudioMixerStripsView::DrawStrip(const plRectFloat& rect, const plDocumentObject* pObject, plUInt32 uiBusIndex, bool bVca, bool bMaster, bool bSelected)
{
  using namespace plEditorUi;

  const auto& accessor = pObject->GetTypeAccessor();
  const bool bAux = !bVca && accessor.GetValue("IsAux").ConvertTo<bool>();
  const plColorGammaUB system = bVca ? plAudioEngineUi::CategoryColor("Utilities") : (bAux ? plAudioEngineUi::CategoryColor("Scripting") : plAudioEngineUi::AudioColor());

  // body: the strip on the raised surface, selection in the accent, VCAs dashed
  m_Writer.Rect(rect, plAudioEngineUi::Mix(Surface(), Raised(), 0.4f), 6.0f);
  if (bVca)
    plAudioEngineUi::DashedRect(m_Writer, rect, bSelected ? Accent() : Border());
  else
    m_Writer.RectOutline(rect, bSelected ? plAudioEngineUi::Mix(Surface(), Accent(), 0.55f) : Line(), 1.0f, 6.0f);

  // header with the system colour as the left edge
  const plRectFloat header(rect.x + 1.0f, rect.y + 1.0f, rect.width - 2.0f, s_fStripHeader - 1.0f);
  m_Writer.Rect(header, Surface(), 5.0f);
  m_Writer.Rect(plRectFloat(header.x, header.y + header.height - 5.0f, header.width, 5.0f), Surface());
  m_Writer.Rect(plRectFloat(header.x, header.y + 2.0f, 3.0f, header.height - 2.0f), system);
  m_Writer.Rect(plRectFloat(rect.x, rect.y + s_fStripHeader, rect.width, 1.0f), Line());

  plStringBuilder sName = accessor.GetValue("Name").ConvertTo<plString>();
  if (sName.IsEmpty())
    sName = bVca ? "VCA" : "Bus";
  if (bVca)
    sName.Prepend("VCA \xC2\xB7 ");

  const float fNameWidth = plMath::Min(MeasureText(sName, FontSize - 1) + 2.0f, rect.width - 20.0f - (bAux ? 18.0f : 0.0f));
  m_Writer.Text(plRectFloat(rect.x + 10.0f, rect.y, fNameWidth, s_fStripHeader), sName, Foreground(), FontSize - 1);
  if (bAux)
    plAudioEngineUi::Icon(m_Writer, plRectFloat(rect.x + 12.0f + fNameWidth, rect.y + 7.0f, 12.0f, 12.0f), IconPath(Icon::Repeat), Muted());

  // inserts (buses) or controlled buses (VCAs)
  float y = rect.y + s_fStripHeader + 6.0f;
  const float fInnerX = rect.x + 8.0f;
  const float fInnerWidth = rect.width - 16.0f;

  auto sectionLabel = [&](plStringView sLabel)
  {
    m_Writer.Text(plRectFloat(fInnerX, y, fInnerWidth, 12.0f), sLabel, Dim(), MinFontSize);
  };

  auto chip = [&](float fY, plStringView sText, bool bOn, bool bAdd)
  {
    const plRectFloat chipRect(fInnerX, fY, fInnerWidth, 15.0f);
    if (bAdd)
    {
      plAudioEngineUi::DashedRect(m_Writer, chipRect, Line());
      m_Writer.Text(chipRect, sText, Dim(), MinFontSize, plUiTextHAlign::Center);
      return;
    }

    m_Writer.Rect(chipRect, Field(), 3.0f);
    m_Writer.RectOutline(chipRect, Line(), 1.0f, 3.0f);
    m_Writer.Rect(plRectFloat(chipRect.x + 1.0f, chipRect.y + 1.0f, 3.0f, chipRect.height - 2.0f), bOn ? plAudioEngineUi::AudioColor() : Border());
    m_Writer.Text(plRectFloat(chipRect.x + 9.0f, chipRect.y, chipRect.width - 12.0f, chipRect.height), sText, bOn ? Foreground() : Muted(), MinFontSize);
  };

  {
    sectionLabel(bVca ? "CONTROLS" : "INSERTS");
    float fChipY = y + 14.0f;

    if (bVca)
    {
      // "Buses" is a string array value, not child objects
      const plInt32 iCount = accessor.GetCount("Buses");
      for (plInt32 i = 0; i < plMath::Min<plInt32>(iCount, 3); ++i)
      {
        chip(fChipY, accessor.GetValue("Buses", i).ConvertTo<plString>(), false, false);
        fChipY += 18.0f;
      }

      if (iCount > 3)
      {
        plStringBuilder sMore;
        sMore.SetFormat("+{} more", iCount - 3);
        m_Writer.Text(plRectFloat(fInnerX, fChipY, fInnerWidth, 12.0f), sMore, Dim(), MinFontSize);
      }
      else if (iCount == 0)
      {
        m_Writer.Text(plRectFloat(fInnerX, fChipY, fInnerWidth, 14.0f), "- none -", Dim(), MinFontSize);
      }
    }
    else
    {
      plHybridArray<const plDocumentObject*, 12> effects;
      GatherChildrenOfProperty(pObject, "Effects", effects);

      for (plUInt32 i = 0; i < plMath::Min<plUInt32>(effects.GetCount(), 3); ++i)
      {
        chip(fChipY, EffectTypeName(effects[i]->GetTypeAccessor().GetValue("Type").ConvertTo<plInt64>()), true, false);
        fChipY += 18.0f;
      }

      if (effects.GetCount() > 3)
      {
        plStringBuilder sMore;
        sMore.SetFormat("+{} more", effects.GetCount() - 3);
        m_Writer.Text(plRectFloat(fInnerX, fChipY, fInnerWidth, 12.0f), sMore, Dim(), MinFontSize);
      }
      else if (effects.IsEmpty())
      {
        chip(fChipY, "+ effect", false, true);
      }
    }
  }

  // sends (buses) or environment (aux buses)
  y += s_fSectionHeight + 6.0f;
  if (!bVca)
  {
    sectionLabel(bAux ? "ENVIRONMENT" : "SENDS");
    const float fLineY = y + 14.0f;

    if (bAux)
    {
      const plString sEnv = accessor.GetValue("Environment").ConvertTo<plString>();
      m_Writer.Text(plRectFloat(fInnerX, fLineY, fInnerWidth, 14.0f), sEnv.IsEmpty() ? plString("- none -") : sEnv, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
    }
    else
    {
      plHybridArray<const plDocumentObject*, 12> sends;
      GatherChildrenOfProperty(pObject, "Sends", sends);

      if (sends.IsEmpty())
        m_Writer.Text(plRectFloat(fInnerX, fLineY, fInnerWidth, 14.0f), "-", Dim(), MinFontSize);

      for (plUInt32 i = 0; i < plMath::Min<plUInt32>(sends.GetCount(), 2); ++i)
      {
        const plString sTarget = sends[i]->GetTypeAccessor().GetValue("TargetBus").ConvertTo<plString>();
        plStringBuilder sLevel;
        sLevel.SetFormat("{} dB", plArgF(sends[i]->GetTypeAccessor().GetValue("Level").ConvertTo<double>(), 0));

        const plRectFloat line(fInnerX, fLineY + i * 15.0f, fInnerWidth, 14.0f);
        m_Writer.Text(line, sTarget, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
        m_Writer.Text(line, sLevel, Dim(), MinFontSize, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
      }
    }
  }

  // fader zone: scale, fader, meter
  y += s_fSectionHeight + 6.0f;
  const float fFaderX = rect.x + 44.0f;
  const plRectFloat fader(fFaderX, y, 22.0f, s_fFaderHeight);

  const bool bDraggingThis = m_bDragging && pObject->GetGuid() == m_DragGuid;
  const float fVolumeDb = bDraggingThis ? m_fDragValueDb : accessor.GetValue("Volume").ConvertTo<float>();

  plEditorUiDraw::FaderScale(m_Writer, fader, fFaderX - 4.0f);
  plEditorUiDraw::Fader(m_Writer, fader, fVolumeDb, plAudioEngineUi::AudioColor(), bDraggingThis);

  if (!bVca)
  {
    // the bus meter on the -60..0 dBFS scale; VCAs are pure gain and carry no signal
    float fPeak = 0.0f;
    float fRms = 0.0f;
    if (m_bLiveMeters && uiBusIndex != plInvalidIndex)
      plAudioPreviewService::GetSingleton().GetBusMeter(uiBusIndex, fPeak, fRms);

    plEditorUiDraw::LevelMeter(m_Writer, plRectFloat(fFaderX + 30.0f, y, 10.0f, s_fFaderHeight), plEditorUiDraw::LinearToDb(fRms), plEditorUiDraw::LinearToDb(fPeak));
  }

  // readout and routing
  y += s_fFaderHeight + 6.0f;
  plStringBuilder sVolume;
  FormatVolume(fVolumeDb, sVolume);
  m_Writer.Text(plRectFloat(rect.x, y, rect.width, 18.0f), sVolume, Foreground(), FontSize - 2, plUiTextHAlign::Center, plUiTextVAlign::Center, true);

  plStringBuilder sRouting;
  if (bVca)
    sRouting = "options-menu group";
  else if (bMaster)
    sRouting = "\xE2\x86\x92 output device";
  else
  {
    const plString sParent = accessor.GetValue("Parent").ConvertTo<plString>();
    sRouting.SetFormat("\xE2\x86\x92 {}", sParent.IsEmpty() ? plStringView("Master") : sParent.GetView());
  }

  m_Writer.Text(plRectFloat(rect.x + 4.0f, y + 18.0f, rect.width - 8.0f, 16.0f), sRouting, Dim(), MinFontSize, plUiTextHAlign::Center);

  Strip& strip = m_Strips.ExpandAndGetRef();
  strip.m_Guid = pObject->GetGuid();
  strip.m_Rect = rect;
  strip.m_Fader = fader;
  strip.m_bVca = bVca;
}

void plQtAudioMixerStripsView::OnPointer(const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;

  switch (e.m_Type)
  {
    case Type::Wheel:
    {
      m_fScrollX -= e.m_fWheel * s_fWheelStep;
      m_bDrawDirty = true;
      return;
    }

    case Type::Press:
    {
      if (e.m_Button != plEditorUiPointerEvent::Button::Left)
        return;

      for (const Strip& strip : m_Strips)
      {
        if (!strip.m_Rect.Contains(e.m_vPosition))
          continue;

        if (const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(strip.m_Guid))
          m_pDocument->GetSelectionManager()->SetSelection(pObject);

        // the fader's track and some room around it grab the cap
        const plRectFloat grab(strip.m_Fader.x - 8.0f, strip.m_Fader.y - 6.0f, strip.m_Fader.width + 16.0f, strip.m_Fader.height + 12.0f);
        if (grab.Contains(e.m_vPosition))
        {
          m_bDragging = true;
          m_DragGuid = strip.m_Guid;
          m_DragFader = strip.m_Fader;
          m_fDragValueDb = SnapVolume(plEditorUiDraw::FaderDbAt(m_DragFader, e.m_vPosition.y));
        }

        m_bDrawDirty = true;
        return;
      }
      return;
    }

    case Type::Move:
    case Type::DragStart:
    case Type::DragMove:
    {
      if (!m_bDragging)
        return;

      m_fDragValueDb = SnapVolume(plEditorUiDraw::FaderDbAt(m_DragFader, e.m_vPosition.y));
      m_bDrawDirty = true;
      return;
    }

    case Type::Release:
    case Type::Drop:
    {
      if (!m_bDragging)
        return;

      m_bDragging = false;
      m_bDrawDirty = true;

      // one step per drag
      const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(m_DragGuid);
      if (pObject == nullptr || plMath::Abs(pObject->GetTypeAccessor().GetValue("Volume").ConvertTo<float>() - m_fDragValueDb) < 0.001f)
        return;

      plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
      accessor.StartTransaction("Set Volume");
      accessor.SetValueByName(pObject, "Volume", m_fDragValueDb).IgnoreResult();
      accessor.FinishTransaction();
      return;
    }

    case Type::DragCancel:
      m_bDragging = false;
      m_bDrawDirty = true;
      return;

    default:
      return;
  }
}

//////////////////////////////////////////////////////////////////////////
// plQtAudioMixerRoutingView

namespace
{
  enum : plUInt64
  {
    GroupBuses = 1,
    GroupVcas = 2,
    GroupSnapshots = 3,
    FirstBus = 100,
    FirstVca = 10000,
    FirstSnapshot = 20000,
  };
} // namespace

/// Groups, then the buses under their parents, the VCAs and the snapshots. Item ids follow the document order.
class plQtAudioMixerRoutingView::Adapter : public plEditorUiTreeAdapter
{
public:
  explicit Adapter(plDocument* pDocument)
    : m_pDocument(pDocument)
  {
  }

  /// Re-reads the buses, VCAs and snapshots and resolves the bus parents.
  void Update()
  {
    const plDocumentObject* pProps = GetMixerProperties(m_pDocument);
    GatherChildrenOfProperty(pProps, "Buses", m_Buses);
    GatherChildrenOfProperty(pProps, "VCAs", m_Vcas);
    GatherChildrenOfProperty(pProps, "Snapshots", m_Snapshots);

    auto findBus = [&](plStringView sName) -> plUInt32
    {
      for (plUInt32 i = 0; i < m_Buses.GetCount(); ++i)
      {
        if (m_Buses[i]->GetTypeAccessor().GetValue("Name").ConvertTo<plString>().IsEqual_NoCase(sName))
          return i;
      }
      return plInvalidIndex;
    };

    // parents by name; unknown or later parents put a bus under the master bus
    m_BusParents.SetCount(m_Buses.GetCount());
    for (plUInt32 i = 0; i < m_Buses.GetCount(); ++i)
    {
      if (i == 0)
      {
        m_BusParents[i] = GroupBuses;
        continue;
      }

      const plString sParent = m_Buses[i]->GetTypeAccessor().GetValue("Parent").ConvertTo<plString>();
      const plUInt32 uiParent = sParent.IsEmpty() ? 0 : findBus(sParent);
      m_BusParents[i] = FirstBus + ((uiParent != plInvalidIndex && uiParent < i) ? uiParent : 0);
    }
  }

  const plDocumentObject* GetObject(plUInt64 uiItem) const
  {
    if (uiItem >= FirstSnapshot && uiItem - FirstSnapshot < m_Snapshots.GetCount())
      return m_Snapshots[static_cast<plUInt32>(uiItem - FirstSnapshot)];
    if (uiItem >= FirstVca && uiItem < FirstSnapshot && uiItem - FirstVca < m_Vcas.GetCount())
      return m_Vcas[static_cast<plUInt32>(uiItem - FirstVca)];
    if (uiItem >= FirstBus && uiItem < FirstVca && uiItem - FirstBus < m_Buses.GetCount())
      return m_Buses[static_cast<plUInt32>(uiItem - FirstBus)];
    return nullptr;
  }

  plUInt64 FindItem(const plDocumentObject* pObject) const
  {
    for (plUInt32 i = 0; i < m_Buses.GetCount(); ++i)
      if (m_Buses[i] == pObject)
        return FirstBus + i;
    for (plUInt32 i = 0; i < m_Vcas.GetCount(); ++i)
      if (m_Vcas[i] == pObject)
        return FirstVca + i;
    for (plUInt32 i = 0; i < m_Snapshots.GetCount(); ++i)
      if (m_Snapshots[i] == pObject)
        return FirstSnapshot + i;
    return 0;
  }

  void GetChildren(plUInt64 uiParent, plDynamicArray<plUInt64>& out_children) const override
  {
    out_children.Clear();

    if (uiParent == 0)
    {
      out_children.PushBack(GroupBuses);
      out_children.PushBack(GroupVcas);
      out_children.PushBack(GroupSnapshots);
    }
    else if (uiParent == GroupVcas)
    {
      for (plUInt32 i = 0; i < m_Vcas.GetCount(); ++i)
        out_children.PushBack(FirstVca + i);
    }
    else if (uiParent == GroupSnapshots)
    {
      for (plUInt32 i = 0; i < m_Snapshots.GetCount(); ++i)
        out_children.PushBack(FirstSnapshot + i);
    }
    else if (uiParent == GroupBuses || (uiParent >= FirstBus && uiParent < FirstVca))
    {
      for (plUInt32 i = 0; i < m_BusParents.GetCount(); ++i)
        if (m_BusParents[i] == uiParent)
          out_children.PushBack(FirstBus + i);
    }
  }

  plUInt64 GetParent(plUInt64 uiItem) const override
  {
    if (uiItem <= GroupSnapshots)
      return 0;
    if (uiItem >= FirstSnapshot)
      return GroupSnapshots;
    if (uiItem >= FirstVca)
      return GroupVcas;

    const plUInt32 uiBus = static_cast<plUInt32>(uiItem - FirstBus);
    return uiBus < m_BusParents.GetCount() ? m_BusParents[uiBus] : GroupBuses;
  }

  void GetRow(plUInt64 uiItem, plEditorUiTreeRow& out_row) const override
  {
    switch (uiItem)
    {
      case GroupBuses:
        out_row.m_sText = "BUSES";
        out_row.m_TextStyle = plEditorUiTreeTextStyle::Muted;
        return;
      case GroupVcas:
        out_row.m_sText = "VCAS";
        out_row.m_TextStyle = plEditorUiTreeTextStyle::Muted;
        return;
      case GroupSnapshots:
        out_row.m_sText = "SNAPSHOTS";
        out_row.m_TextStyle = plEditorUiTreeTextStyle::Muted;
        return;
      default:
        break;
    }

    const plDocumentObject* pObject = GetObject(uiItem);
    if (pObject == nullptr)
      return;

    plString sName = pObject->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      sName = "<unnamed>";
    out_row.m_sText = sName;

    if (uiItem >= FirstSnapshot)
    {
      out_row.m_sIcon = plEditorUi::IconPath(plEditorUi::Icon::Camera);
      out_row.m_IconColor = plEditorUi::Muted();
    }
    else if (uiItem >= FirstVca)
    {
      out_row.m_sIcon = plAudioEngineUi::IconSliders;
      out_row.m_IconColor = plAudioEngineUi::CategoryColor("Utilities");
    }
    else if (pObject->GetTypeAccessor().GetValue("IsAux").ConvertTo<bool>())
    {
      out_row.m_sIcon = plEditorUi::IconPath(plEditorUi::Icon::Repeat);
      out_row.m_IconColor = plAudioEngineUi::CategoryColor("Scripting");
      out_row.m_sTag = "aux";
    }
    else
    {
      out_row.m_sIcon = plAudioEngineUi::IconMixer;
      out_row.m_IconColor = plAudioEngineUi::AudioColor();
    }
  }

private:
  plDocument* m_pDocument = nullptr;
  plHybridArray<const plDocumentObject*, 12> m_Buses;
  plHybridArray<const plDocumentObject*, 4> m_Vcas;
  plHybridArray<const plDocumentObject*, 8> m_Snapshots;
  plHybridArray<plUInt64, 12> m_BusParents;
};

plQtAudioMixerRoutingView::plQtAudioMixerRoutingView(QWidget* pParent, plDocument* pDocument)
  : plQtEngineUiWidget(pParent, "AudioMixerRouting")
  , m_pDocument(pDocument)
{
  setMinimumSize(0, 0);

  m_pAdapter = PL_DEFAULT_NEW(Adapter, pDocument);
  m_pAdapter->Update();

  if (IsReady())
  {
    m_pTree = PL_DEFAULT_NEW(plEditorUiTree, *GetSurface(), "Mixer.Routing", *m_pAdapter);
    m_TreeSubscription = m_pTree->m_Events.AddEventHandler(plMakeDelegate(&plQtAudioMixerRoutingView::OnTreeEvent, this));
  }
}

plQtAudioMixerRoutingView::~plQtAudioMixerRoutingView()
{
  if (m_pTree != nullptr)
    m_pTree->m_Events.RemoveEventHandler(m_TreeSubscription);

  m_pTree.Clear();
}

void plQtAudioMixerRoutingView::Refresh()
{
  m_pAdapter->Update();

  if (m_pTree == nullptr || m_bRebuild)
    return;

  m_pTree->Refresh();
  m_pTree->ExpandAll();

  // show the document's selection
  m_bSyncingSelection = true;
  const plUInt64 uiItem = m_pAdapter->FindItem(m_pDocument->GetSelectionManager()->GetCurrentObject());
  plHybridArray<plUInt64, 1> selection;
  if (uiItem != 0)
    selection.PushBack(uiItem);
  m_pTree->SetSelection(selection);
  m_bSyncingSelection = false;
}

void plQtAudioMixerRoutingView::OnUiUpdate()
{
  if (IsReady() && m_bRebuild)
    BuildDocument();
}

void plQtAudioMixerRoutingView::BuildDocument()
{
  m_bRebuild = false;

  plUiPanel* pRoot = plEditorUi::Panel();
  pRoot->m_Color = plEditorUi::Surface();

  plUiPanel* pList = plEditorUi::Panel(pRoot);
  plEditorUi::Fill(pList, plVec4(0, 2, 0, 0));
  pList->m_Color = plEditorUi::Surface();
  m_pTree->Build(pList);

  SetDocument(plEditorUi::Finish(pRoot));
  Refresh();
}

void plQtAudioMixerRoutingView::OnTreeEvent(const plEditorUiTreeEvent& e)
{
  if (e.m_Type != plEditorUiTreeEvent::Type::SelectionChanged || m_bSyncingSelection)
    return;

  plHybridArray<plUInt64, 4> selection;
  m_pTree->GetSelection(selection);
  if (selection.IsEmpty())
    return;

  if (const plDocumentObject* pObject = m_pAdapter->GetObject(selection[0]))
    m_pDocument->GetSelectionManager()->SetSelection(pObject);
}

//////////////////////////////////////////////////////////////////////////
// plQtAudioMixerSnapshotsView

plQtAudioMixerSnapshotsView::plQtAudioMixerSnapshotsView(QWidget* pParent, plDocument* pDocument)
  : plQtEngineUiWidget(pParent, "AudioMixerSnapshots")
  , m_pDocument(pDocument)
{
  setMinimumSize(0, 0);

  if (IsReady())
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plQtAudioMixerSnapshotsView::OnPointer, this));
}

plQtAudioMixerSnapshotsView::~plQtAudioMixerSnapshotsView()
{
  if (IsReady())
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
}

void plQtAudioMixerSnapshotsView::Refresh()
{
  m_bDrawDirty = true;
}

void plQtAudioMixerSnapshotsView::OnUiUpdate()
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

void plQtAudioMixerSnapshotsView::BuildDocument()
{
  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();

  plUiPanel* pRoot = plEditorUi::Panel();
  pRoot->m_Color = plEditorUi::Surface();
  plEditorUi::DrawList(pRoot, "Mixer.Snapshots", s_sSnapshotsEntry.GetView());

  SetDocument(plEditorUi::Finish(pRoot));
  m_bDrawDirty = true;
}

void plQtAudioMixerSnapshotsView::Draw()
{
  using namespace plEditorUi;

  m_bDrawDirty = false;
  m_Writer.Clear();
  m_Rows.Clear();

  const plVec2 size = GetSurface()->GetLogicalSize();

  plHybridArray<const plDocumentObject*, 8> snapshots;
  GatherChildrenOfProperty(GetMixerProperties(m_pDocument), "Snapshots", snapshots);

  if (snapshots.IsEmpty())
    m_Writer.Text(plRectFloat(10.0f, 6.0f, size.x - 20.0f, 20.0f), "No snapshots - '+ Snapshot' adds one.", Dim(), FontSize - 1);

  const plDocumentObject* pSelected = m_pDocument->GetSelectionManager()->GetCurrentObject();

  float y = 2.0f;
  for (const plDocumentObject* pSnapshot : snapshots)
  {
    const auto& accessor = pSnapshot->GetTypeAccessor();

    plString sName = accessor.GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      sName = "<unnamed>";

    plHybridArray<const plDocumentObject*, 8> overrides;
    GatherChildrenOfProperty(pSnapshot, "Overrides", overrides);

    plStringBuilder sLine;
    sLine.SetFormat("{} overrides \xC2\xB7 in {}s / out {}s \xC2\xB7 prio {}", overrides.GetCount(), plArgF(accessor.GetValue("FadeIn").ConvertTo<double>(), 1), plArgF(accessor.GetValue("FadeOut").ConvertTo<double>(), 1), accessor.GetValue("Priority").ConvertTo<plInt64>());

    if (pSnapshot == pSelected)
      m_Writer.Rect(plRectFloat(0.0f, y, size.x, s_fSnapshotRow), Selection());

    m_Writer.Text(plRectFloat(10.0f, y + 3.0f, size.x - 20.0f, 18.0f), sName, Foreground(), FontSize);
    m_Writer.Text(plRectFloat(10.0f, y + 20.0f, size.x - 20.0f, 16.0f), sLine, Dim(), FontSize - 2);

    m_Rows.PushBack(pSnapshot->GetGuid());
    y += s_fSnapshotRow;
  }

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sSnapshotsEntry);
}

void plQtAudioMixerSnapshotsView::OnPointer(const plEditorUiPointerEvent& e)
{
  if (e.m_Type != plEditorUiPointerEvent::Type::Press || e.m_Button != plEditorUiPointerEvent::Button::Left)
    return;

  const plInt32 iRow = static_cast<plInt32>((e.m_vPosition.y - 2.0f) / s_fSnapshotRow);
  if (e.m_vPosition.y < 2.0f || iRow < 0 || iRow >= static_cast<plInt32>(m_Rows.GetCount()))
    return;

  if (const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(m_Rows[iRow]))
    m_pDocument->GetSelectionManager()->SetSelection(pObject);
}
