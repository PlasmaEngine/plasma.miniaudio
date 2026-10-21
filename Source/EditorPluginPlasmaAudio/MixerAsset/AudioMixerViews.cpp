#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <AudioSystem/Mixer/AudioMixerData.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerAsset.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerViews.h>
#include <EditorUi/Drawings.h>
#include <EditorUi/Menu.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

namespace
{
  constexpr float s_fStripWidth = 128.0f;
  constexpr float s_fStripGap = 8.0f;
  constexpr float s_fMargin = 10.0f;
  constexpr float s_fStripHeader = 26.0f;
  constexpr float s_fMuteRow = 24.0f;
  constexpr float s_fInsertSection = 14.0f + 4.0f * 18.0f;
  constexpr float s_fSendSection = 40.0f;
  constexpr float s_fDuckRow = 16.0f;
  constexpr float s_fFaderHeight = 160.0f;
  constexpr float s_fWheelStep = 40.0f;
  constexpr float s_fBannerHeight = 28.0f;
  constexpr plUInt32 s_uiMaxInsertSlots = 3;

  const plHashedString s_sStripsEntry = plMakeHashedString("Mixer.Strips");

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
// plEditorUiAudioMixerStrips

plEditorUiAudioMixerStrips::plEditorUiAudioMixerStrips(plDocument* pDocument, const plAudioMixerConsoleState* pState)
  : plEditorUiContent("AudioMixerStrips")
  , m_pDocument(pDocument)
  , m_pState(pState)
{
  if (IsReady())
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioMixerStrips::OnPointer, this));
}

plEditorUiAudioMixerStrips::~plEditorUiAudioMixerStrips()
{
  if (IsReady())
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
}

void plEditorUiAudioMixerStrips::Refresh()
{
  m_bDrawDirty = true;
}

const plDocumentObject* plEditorUiAudioMixerStrips::GetPropertiesObject() const
{
  return GetMixerProperties(m_pDocument);
}

void plEditorUiAudioMixerStrips::GatherStrips(plHybridArray<const plDocumentObject*, 12>& out_buses, plHybridArray<const plDocumentObject*, 4>& out_vcas) const
{
  GatherChildrenOfProperty(GetPropertiesObject(), "Buses", out_buses);
  GatherChildrenOfProperty(GetPropertiesObject(), "VCAs", out_vcas);
}

float plEditorUiAudioMixerStrips::GetContentWidth() const
{
  plHybridArray<const plDocumentObject*, 12> buses;
  plHybridArray<const plDocumentObject*, 4> vcas;
  GatherStrips(buses, vcas);

  return 2.0f * s_fMargin + (buses.GetCount() + vcas.GetCount()) * (s_fStripWidth + s_fStripGap) + (vcas.IsEmpty() ? 0.0f : 9.0f);
}

void plEditorUiAudioMixerStrips::OnUiUpdate()
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

void plEditorUiAudioMixerStrips::BuildDocument()
{
  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();

  plUiPanel* pRoot = plEditorUi::Panel();
  pRoot->m_Color = plEditorUi::Surface();
  plEditorUi::DrawList(pRoot, "Mixer.Strips", s_sStripsEntry.GetView());

  SetDocument(plEditorUi::Finish(pRoot));
  m_bDrawDirty = true;
}

void plEditorUiAudioMixerStrips::Draw()
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

  // validation: a banner with the first issue; the buses it concerns are outlined in their strips
  m_fTop = 0.0f;
  if (m_pState != nullptr && !m_pState->m_Issues.IsEmpty())
  {
    const plRectFloat banner(s_fMargin, 6.0f, size.x - 2.0f * s_fMargin, s_fBannerHeight - 4.0f);
    m_Writer.Rect(banner, plAudioEngineUi::WithAlpha(Danger(), 40), 4.0f);
    m_Writer.RectOutline(banner, plAudioEngineUi::WithAlpha(Danger(), 140), 1.0f, 4.0f);

    plStringBuilder sBanner = m_pState->m_Issues[0].m_sMessage;
    if (m_pState->m_Issues.GetCount() > 1)
      sBanner.AppendFormat("  (+{} more)", m_pState->m_Issues.GetCount() - 1);
    sBanner.Append("  The asset does not transform until this is fixed.");

    m_Writer.Text(plRectFloat(banner.x + 10.0f, banner.y, banner.width - 20.0f, banner.height), sBanner, Foreground(), FontSize - 1);
    m_fTop = s_fBannerHeight + 4.0f;
  }

  const plUuid selected = m_pDocument->GetSelectionManager()->GetCurrentObject() != nullptr ? m_pDocument->GetSelectionManager()->GetCurrentObject()->GetGuid() : plUuid();
  const float fStripHeight = s_fStripHeader + s_fMuteRow + s_fInsertSection + 6.0f + s_fSendSection + s_fDuckRow + 6.0f + s_fFaderHeight + 6.0f + 18.0f + 16.0f + 8.0f;
  const float fTop = m_fTop + s_fMargin;

  float x = s_fMargin - m_fScrollX;
  for (plUInt32 i = 0; i < buses.GetCount(); ++i)
  {
    DrawStrip(plRectFloat(x, fTop, s_fStripWidth, fStripHeight), buses[i], i, false, i == 0, buses[i]->GetGuid() == selected);
    x += s_fStripWidth + s_fStripGap;
  }

  if (!vcas.IsEmpty())
  {
    // a dashed divider between the buses and the VCAs
    m_Writer.Line(plVec2(x + 0.5f, fTop), plVec2(x + 0.5f, fTop + fStripHeight), Border(), 1.0f, 3.0f, 3.0f);
    x += 9.0f;
  }

  for (plUInt32 i = 0; i < vcas.GetCount(); ++i)
  {
    DrawStrip(plRectFloat(x, fTop, s_fStripWidth, fStripHeight), vcas[i], plInvalidIndex, true, false, vcas[i]->GetGuid() == selected);
    x += s_fStripWidth + s_fStripGap;
  }

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sStripsEntry);
}

void plEditorUiAudioMixerStrips::DrawStrip(const plRectFloat& rect, const plDocumentObject* pObject, plUInt32 uiBusIndex, bool bVca, bool bMaster, bool bSelected)
{
  using namespace plEditorUi;

  const auto& accessor = pObject->GetTypeAccessor();
  const bool bAux = !bVca && accessor.GetValue("IsAux").ConvertTo<bool>();
  const plColorGammaUB system = bVca ? plAudioEngineUi::CategoryColor("Utilities") : (bAux ? plAudioEngineUi::CategoryColor("Scripting") : plAudioEngineUi::AudioColor());

  const bool bError = !bVca && m_pState != nullptr && uiBusIndex < m_pState->m_BusError.GetCount() && m_pState->m_BusError[uiBusIndex];

  // body: the strip on the raised surface, selection in the accent, VCAs dashed, buses with a validation issue in red
  m_Writer.Rect(rect, plAudioEngineUi::Mix(Surface(), Raised(), 0.4f), 6.0f);
  if (bVca)
    plAudioEngineUi::DashedRect(m_Writer, rect, bSelected ? Accent() : Border());
  else if (bError)
    m_Writer.RectOutline(rect, Danger(), 2.0f, 6.0f);
  else
    m_Writer.RectOutline(rect, bSelected ? plAudioEngineUi::Mix(Surface(), Accent(), 0.55f) : Line(), 1.0f, 6.0f);

  Strip& strip = m_Strips.ExpandAndGetRef();
  strip.m_Guid = pObject->GetGuid();
  strip.m_uiBus = uiBusIndex;
  strip.m_Rect = rect;
  strip.m_bVca = bVca;

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

  const float fInnerX = rect.x + 8.0f;
  const float fInnerWidth = rect.width - 16.0f;

  // mute and solo (buses only; they act on the engine process's mixer)
  float y = rect.y + s_fStripHeader + 4.0f;
  if (!bVca)
  {
    const bool bMuted = m_pState != nullptr && uiBusIndex < m_pState->m_BusMuted.GetCount() && m_pState->m_BusMuted[uiBusIndex];
    const bool bSoloed = m_pState != nullptr && uiBusIndex < m_pState->m_BusSoloed.GetCount() && m_pState->m_BusSoloed[uiBusIndex];

    auto toggle = [&](const plRectFloat& button, plStringView sText, bool bOn, const plColorGammaUB& onColor)
    {
      m_Writer.Rect(button, bOn ? onColor : Field(), 3.0f);
      m_Writer.RectOutline(button, bOn ? onColor : Line(), 1.0f, 3.0f);
      m_Writer.Text(button, sText, bOn ? plColorGammaUB(16, 16, 18) : Muted(), FontSize - 2, plUiTextHAlign::Center);
    };

    strip.m_Mute = plRectFloat(fInnerX, y, 24.0f, 18.0f);
    strip.m_Solo = plRectFloat(fInnerX + 28.0f, y, 24.0f, 18.0f);
    toggle(strip.m_Mute, "M", bMuted, Danger());
    toggle(strip.m_Solo, "S", bSoloed, Warning());
  }

  // inserts (buses) or controlled buses (VCAs)
  y += s_fMuteRow;

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

      const bool bThisBus = m_pState != nullptr && m_pState->m_SelectedBus == pObject->GetGuid();

      for (plUInt32 i = 0; i < plMath::Min<plUInt32>(effects.GetCount(), s_uiMaxInsertSlots); ++i)
      {
        const auto& effect = effects[i]->GetTypeAccessor();
        const auto eType = static_cast<plAudioEffectType::Enum>(effect.GetValue("Type").ConvertTo<plInt64>());
        const bool bBypass = effect.GetValue("Bypass").ConvertTo<bool>();
        const bool bKeyed = !effect.GetValue("SidechainBus").ConvertTo<plString>().IsEmpty() && plAudioEffectRegistry::GetTypeInfo(eType).m_bUsesSidechain;
        const bool bSlotSelected = bThisBus && m_pState->m_iSelectedEffect == static_cast<plInt32>(i);

        const plRectFloat slot(fInnerX, fChipY, fInnerWidth, 15.0f);
        chip(fChipY, plAudioEffectRegistry::GetTypeInfo(eType).m_szShortName, !bBypass, false);
        if (bSlotSelected)
          m_Writer.RectOutline(slot, Accent(), 1.0f, 3.0f);
        if (bBypass)
          m_Writer.Text(slot, "bypass", Dim(), MinFontSize, plUiTextHAlign::Right);
        else if (bKeyed)
          m_Writer.Text(plRectFloat(slot.x, slot.y, slot.width - 4.0f, slot.height), "\xE2\x86\x90", Warning(), MinFontSize, plUiTextHAlign::Right);

        strip.m_Inserts.PushBack(slot);
        fChipY += 18.0f;
      }

      if (effects.GetCount() > s_uiMaxInsertSlots)
      {
        plStringBuilder sMore;
        sMore.SetFormat("+{} more", effects.GetCount() - s_uiMaxInsertSlots);
        m_Writer.Text(plRectFloat(fInnerX, fChipY, fInnerWidth, 15.0f), sMore, Dim(), MinFontSize);
      }
      else
      {
        chip(fChipY, "+ insert", false, true);
        strip.m_AddInsert = plRectFloat(fInnerX, fChipY, fInnerWidth, 15.0f);
      }
    }
  }

  // the first send with its level (buses), or the environment (aux buses)
  y += s_fInsertSection + 6.0f;
  if (!bVca)
  {
    sectionLabel(bAux ? "ENVIRONMENT" : "SEND");
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
      {
        m_Writer.Text(plRectFloat(fInnerX, fLineY, fInnerWidth, 14.0f), "-", Dim(), MinFontSize);
      }
      else
      {
        const bool bDraggingThis = m_Drag == Drag::Send && pObject->GetGuid() == m_DragGuid;
        const float fLevel = bDraggingThis ? m_fDragValueDb : sends[0]->GetTypeAccessor().GetValue("Level").ConvertTo<float>();

        plStringBuilder sTarget;
        sTarget.SetFormat("\xE2\x86\x92 {}", sends[0]->GetTypeAccessor().GetValue("TargetBus").ConvertTo<plString>());
        if (sends.GetCount() > 1)
          sTarget.AppendFormat(" +{}", sends.GetCount() - 1);

        plStringBuilder sLevel;
        sLevel.SetFormat("{} dB", plArgF(fLevel, 0));

        const plRectFloat line(fInnerX, fLineY, fInnerWidth, 14.0f);
        m_Writer.Text(line, sTarget, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
        m_Writer.Text(line, sLevel, Muted(), MinFontSize, plUiTextHAlign::Right, plUiTextVAlign::Center, true);

        strip.m_Send = plRectFloat(fInnerX, fLineY + 16.0f, fInnerWidth, 8.0f);
        plAudioEngineUi::Slider(m_Writer, strip.m_Send, plMath::Clamp((fLevel + 60.0f) / 60.0f, 0.0f, 1.0f), true, bDraggingThis);
      }
    }
  }

  // ducking: a compressor or ducker keyed by another bus
  y += s_fSendSection;
  if (!bVca)
  {
    plHybridArray<const plDocumentObject*, 12> effects;
    GatherChildrenOfProperty(pObject, "Effects", effects);

    for (plUInt32 i = 0; i < effects.GetCount(); ++i)
    {
      const auto& effect = effects[i]->GetTypeAccessor();
      const plString sKey = effect.GetValue("SidechainBus").ConvertTo<plString>();
      const auto eType = static_cast<plAudioEffectType::Enum>(effect.GetValue("Type").ConvertTo<plInt64>());
      if (sKey.IsEmpty() || !plAudioEffectRegistry::GetTypeInfo(eType).m_bUsesSidechain || effect.GetValue("Bypass").ConvertTo<bool>())
        continue;

      float fReduction = 0.0f;
      if (m_pState != nullptr && uiBusIndex < m_pState->m_EffectGainReduction.GetCount() && i < m_pState->m_EffectGainReduction[uiBusIndex].GetCount())
        fReduction = m_pState->m_EffectGainReduction[uiBusIndex][i];

      plStringBuilder sDuck;
      if (fReduction < -0.05f)
        sDuck.SetFormat("\xE2\x86\x93 duck {} dB", plArgF(fReduction, 1));
      else
        sDuck.SetFormat("\xE2\x86\x93 {}", sKey);

      m_Writer.Text(plRectFloat(fInnerX, y, fInnerWidth, s_fDuckRow), sDuck, fReduction < -0.05f ? Warning() : Dim(), MinFontSize);
      break;
    }
  }

  // fader zone: scale, fader, meter
  y += s_fDuckRow + 6.0f;
  const float fFaderX = rect.x + 44.0f;
  const plRectFloat fader(fFaderX, y, 22.0f, s_fFaderHeight);

  const bool bDraggingThis = m_Drag == Drag::Fader && pObject->GetGuid() == m_DragGuid;
  const float fVolumeDb = bDraggingThis ? m_fDragValueDb : accessor.GetValue("Volume").ConvertTo<float>();

  plEditorUiDraw::FaderScale(m_Writer, fader, fFaderX - 4.0f);
  plEditorUiDraw::Fader(m_Writer, fader, fVolumeDb, plAudioEngineUi::AudioColor(), bDraggingThis);

  if (!bVca)
  {
    // the bus meter of the engine process on the -60..0 dBFS scale; VCAs are pure gain and carry no signal
    float fPeak = 0.0f;
    float fRms = 0.0f;
    if (m_pState != nullptr && uiBusIndex < m_pState->m_BusPeak.GetCount())
    {
      fPeak = m_pState->m_BusPeak[uiBusIndex];
      fRms = m_pState->m_BusRms[uiBusIndex];
    }

    plEditorUiDraw::LevelMeter(m_Writer, plRectFloat(fFaderX + 30.0f, y, 10.0f, s_fFaderHeight), plEditorUiDraw::LinearToDb(fRms), plEditorUiDraw::LinearToDb(fPeak));
  }

  // readout: "-4.0 → -1.0" while a playing snapshot changes the volume, and the routing
  y += s_fFaderHeight + 6.0f;
  plStringBuilder sVolume;
  FormatVolume(fVolumeDb, sVolume);

  bool bOverridden = false;
  if (!bVca && !bDraggingThis && m_pState != nullptr && uiBusIndex < m_pState->m_BusSnapshotDb.GetCount() && plMath::Abs(m_pState->m_BusSnapshotDb[uiBusIndex] - fVolumeDb) > 0.05f)
  {
    sVolume.SetFormat("{} \xE2\x86\x92 {}", plArgF(fVolumeDb, 1), plArgF(m_pState->m_BusSnapshotDb[uiBusIndex], 1));
    bOverridden = true;
  }

  m_Writer.Text(plRectFloat(rect.x, y, rect.width, 18.0f), sVolume, bOverridden ? Warning() : Foreground(), FontSize - 2, plUiTextHAlign::Center, plUiTextVAlign::Center, true);

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

  strip.m_Fader = fader;

  // a bus that mute or another bus's solo silences is dimmed
  if (!bVca && m_pState != nullptr && uiBusIndex < m_pState->m_BusSilenced.GetCount() && m_pState->m_BusSilenced[uiBusIndex])
    m_Writer.Rect(rect, plAudioEngineUi::WithAlpha(Surface(), 150), 6.0f);
}

void plEditorUiAudioMixerStrips::OnPointer(const plEditorUiPointerEvent& e)
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

        const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(strip.m_Guid);
        if (pObject == nullptr)
          return;

        m_bDrawDirty = true;

        if (strip.m_Mute.Contains(e.m_vPosition))
        {
          const bool bMuted = m_pState != nullptr && strip.m_uiBus < m_pState->m_BusMuted.GetCount() && m_pState->m_BusMuted[strip.m_uiBus];
          if (m_OnMute.IsValid())
            m_OnMute(strip.m_Guid, !bMuted);
          return;
        }

        if (strip.m_Solo.Contains(e.m_vPosition))
        {
          const bool bSoloed = m_pState != nullptr && strip.m_uiBus < m_pState->m_BusSoloed.GetCount() && m_pState->m_BusSoloed[strip.m_uiBus];
          if (m_OnSolo.IsValid())
            m_OnSolo(strip.m_Guid, !bSoloed);
          return;
        }

        if (strip.m_AddInsert.Contains(e.m_vPosition))
        {
          ShowAddInsertMenu(strip.m_Guid, plVec2(strip.m_AddInsert.x, strip.m_AddInsert.y + strip.m_AddInsert.height));
          return;
        }

        m_pDocument->GetSelectionManager()->SetSelection(pObject);

        plInt32 iInsert = -1;
        for (plUInt32 i = 0; i < strip.m_Inserts.GetCount(); ++i)
        {
          if (strip.m_Inserts[i].Contains(e.m_vPosition))
            iInsert = static_cast<plInt32>(i);
        }

        if (m_OnSelectInsert.IsValid() && !strip.m_bVca)
          m_OnSelectInsert(strip.m_Guid, iInsert);

        // the send slider, or the fader's track and some room around it, start a drag
        const plRectFloat sendGrab(strip.m_Send.x - 2.0f, strip.m_Send.y - 5.0f, strip.m_Send.width + 4.0f, strip.m_Send.height + 10.0f);
        const plRectFloat faderGrab(strip.m_Fader.x - 8.0f, strip.m_Fader.y - 6.0f, strip.m_Fader.width + 16.0f, strip.m_Fader.height + 12.0f);

        if (strip.m_Send.width > 0.0f && sendGrab.Contains(e.m_vPosition))
        {
          m_Drag = Drag::Send;
          m_DragRect = strip.m_Send;
        }
        else if (faderGrab.Contains(e.m_vPosition))
        {
          m_Drag = Drag::Fader;
          m_DragRect = strip.m_Fader;
        }
        else
        {
          return;
        }

        m_DragGuid = strip.m_Guid;
        m_uiDragBus = strip.m_uiBus;
        break;
      }

      if (m_Drag == Drag::None)
        return;

      [[fallthrough]];
    }

    case Type::Move:
    case Type::DragStart:
    case Type::DragMove:
    {
      if (m_Drag == Drag::None)
        return;

      plAudioMixerLiveEdit edit;
      edit.m_uiBus = m_uiDragBus;

      if (m_Drag == Drag::Fader)
      {
        m_fDragValueDb = SnapVolume(plEditorUiDraw::FaderDbAt(m_DragRect, e.m_vPosition.y));
        edit.m_Type = plAudioMixerLiveEdit::Type::BusVolume;
      }
      else
      {
        m_fDragValueDb = plMath::Round(plAudioEngineUi::SliderFractionAt(m_DragRect, e.m_vPosition.x) * 60.0f - 60.0f);
        edit.m_Type = plAudioMixerLiveEdit::Type::SendLevel;
      }

      edit.m_fValue = m_fDragValueDb;

      // VCAs are not part of a bus's sound on their own; only buses are heard while dragging
      if (m_OnLiveEdit.IsValid() && m_uiDragBus != plInvalidIndex)
        m_OnLiveEdit(edit);

      m_bDrawDirty = true;
      return;
    }

    case Type::Release:
    case Type::Drop:
    {
      if (m_Drag == Drag::None)
        return;

      const Drag drag = m_Drag;
      m_Drag = Drag::None;
      m_bDrawDirty = true;

      const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(m_DragGuid);
      if (pObject == nullptr)
        return;

      // one step per drag
      if (drag == Drag::Fader)
      {
        if (plMath::Abs(pObject->GetTypeAccessor().GetValue("Volume").ConvertTo<float>() - m_fDragValueDb) < 0.001f)
          return;

        plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
        accessor.StartTransaction("Set Volume");
        accessor.SetValueByName(pObject, "Volume", m_fDragValueDb).IgnoreResult();
        accessor.FinishTransaction();
      }
      else
      {
        plHybridArray<const plDocumentObject*, 4> sends;
        GatherChildrenOfProperty(pObject, "Sends", sends);
        if (sends.IsEmpty() || plMath::Abs(sends[0]->GetTypeAccessor().GetValue("Level").ConvertTo<float>() - m_fDragValueDb) < 0.001f)
          return;

        plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
        accessor.StartTransaction("Set Send Level");
        accessor.SetValueByName(sends[0], "Level", m_fDragValueDb).IgnoreResult();
        accessor.FinishTransaction();
      }
      return;
    }

    case Type::DragCancel:
      m_Drag = Drag::None;
      m_bDrawDirty = true;
      return;

    default:
      return;
  }
}

void plEditorUiAudioMixerStrips::ShowAddInsertMenu(const plUuid& bus, const plVec2& vPosition)
{
  plEditorUiMenu menu;
  for (plUInt32 i = 0; i < plAudioEffectRegistry::GetTypeCount(); ++i)
  {
    menu.AddAction(plAudioEffectRegistry::GetTypeInfo(static_cast<plAudioEffectType::Enum>(i)).m_szName, i);
  }

  plEditorUiMenuView::Show(*GetSurface(), vPosition, menu, [this, bus](plUInt32 uiType)
    {
      const plDocumentObject* pBus = m_pDocument->GetObjectManager()->GetObject(bus);
      if (pBus == nullptr || uiType >= plAudioEffectRegistry::GetTypeCount())
        return;

      const auto eType = static_cast<plAudioEffectType::Enum>(uiType);
      const plAudioEffectTypeInfo& info = plAudioEffectRegistry::GetTypeInfo(eType);

      plHybridArray<const plDocumentObject*, 4> effects;
      GatherChildrenOfProperty(pBus, "Effects", effects);

      // a name unique on the bus, so snapshot overrides keep addressing this insert when others are added or moved
      plStringBuilder sName = info.m_szName;
      for (plUInt32 uiSuffix = 2;; ++uiSuffix)
      {
        bool bTaken = false;
        for (const plDocumentObject* pEffect : effects)
        {
          bTaken |= pEffect->GetTypeAccessor().GetValue("Name").ConvertTo<plString>().IsEqual_NoCase(sName);
        }

        if (!bTaken)
          break;

        sName.SetFormat("{} {}", info.m_szName, uiSuffix);
      }

      plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
      accessor.StartTransaction("Add Insert");

      plUuid effectGuid;
      if (accessor.AddObjectByName(pBus, "Effects", -1, plGetStaticRTTI<plAudioMixerEffect>(), effectGuid).Failed())
      {
        accessor.CancelTransaction();
        return;
      }

      const plDocumentObject* pEffect = m_pDocument->GetObjectManager()->GetObject(effectGuid);
      accessor.SetValueByName(pEffect, "Type", static_cast<plInt64>(eType)).IgnoreResult();
      accessor.SetValueByName(pEffect, "Name", sName.GetView()).IgnoreResult();

      for (plUInt32 p = 0; p < info.m_Parameters.GetCount(); ++p)
      {
        accessor.InsertValueByName(pEffect, "Params", info.m_Parameters[p].m_fDefault, p).IgnoreResult();
      }

      accessor.FinishTransaction();

      if (m_OnSelectInsert.IsValid())
        m_OnSelectInsert(bus, static_cast<plInt32>(effects.GetCount()));
    });
}

//////////////////////////////////////////////////////////////////////////
// plEditorUiAudioMixerRouting

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
class plEditorUiAudioMixerRouting::Adapter : public plEditorUiTreeAdapter
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
        out_row.m_TextStyle = plEditorUiTreeTextStyle::Section;
        return;
      case GroupVcas:
        out_row.m_sText = "VCAS";
        out_row.m_TextStyle = plEditorUiTreeTextStyle::Section;
        return;
      case GroupSnapshots:
        out_row.m_sText = "SNAPSHOTS";
        out_row.m_TextStyle = plEditorUiTreeTextStyle::Section;
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

plEditorUiAudioMixerRouting::plEditorUiAudioMixerRouting(plDocument* pDocument)
  : plEditorUiContent("AudioMixerRouting")
  , m_pDocument(pDocument)
{
  m_pAdapter = PL_DEFAULT_NEW(Adapter, pDocument);
  m_pAdapter->Update();

  if (IsReady())
  {
    m_pTree = PL_DEFAULT_NEW(plEditorUiTree, *GetSurface(), "Mixer.Routing", *m_pAdapter);
    m_TreeSubscription = m_pTree->m_Events.AddEventHandler(plMakeDelegate(&plEditorUiAudioMixerRouting::OnTreeEvent, this));
  }
}

plEditorUiAudioMixerRouting::~plEditorUiAudioMixerRouting()
{
  if (m_pTree != nullptr)
    m_pTree->m_Events.RemoveEventHandler(m_TreeSubscription);

  m_pTree.Clear();
}

void plEditorUiAudioMixerRouting::Refresh()
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

void plEditorUiAudioMixerRouting::OnUiUpdate()
{
  if (IsReady() && m_bRebuild)
    BuildDocument();
}

void plEditorUiAudioMixerRouting::BuildDocument()
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

void plEditorUiAudioMixerRouting::OnTreeEvent(const plEditorUiTreeEvent& e)
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
