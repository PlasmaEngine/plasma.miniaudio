#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationAsset.h>
#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationView.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorUi/Drawings.h>
#include <EditorUi/Menu.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <GameUiPlugin/Components/UiCanvasComponent.h>
#include <PlasmaAudioPlugin/Events/AudioEventResource.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

namespace
{
  constexpr float s_fPad = 8.0f;
  constexpr float s_fHeader = plAudioEngineUi::SectionHeight;
  constexpr float s_fCurveRowTop = 30.0f;
  constexpr float s_fPlotTop = s_fCurveRowTop + plEditorUi::ControlHeight + 10.0f;
  constexpr float s_fPlotBottomSpace = 40.0f; ///< the distance labels and the caption under the plot
  constexpr float s_fMinimumHeight = s_fPlotTop + 120.0f + s_fPlotBottomSpace;
  constexpr float s_fGrab = 6.0f;
  constexpr float s_fFloorDb = -60.0f;

  const plHashedString s_sDrawingEntry = plMakeHashedString("Attenuation.Drawing");

  constexpr const char* s_szCurves[] = {"Inverse", "Linear", "Logarithmic", "Custom", "None"};

  const plDocumentObject* FindChild(const plDocumentObject* pParent, const plUuid& guid)
  {
    for (const plDocumentObject* pChild : pParent->GetChildren())
    {
      if (pChild->GetGuid() == guid)
        return pChild;
    }

    return nullptr;
  }

  bool HasProperty(const plDocumentObject* pObject, const char* szProperty)
  {
    return pObject->GetTypeAccessor().GetType()->FindPropertyByName(szProperty) != nullptr;
  }

  float GainToDb(float fGain)
  {
    return fGain <= 0.001f ? s_fFloorDb : plMath::Max(s_fFloorDb, 20.0f * plMath::Log10(fGain));
  }

  void FormatDistance(float fDistance, plStringBuilder& out_sText)
  {
    out_sText.SetFormat("{} m", plArgF(fDistance, fDistance < 10.0f ? 1 : 0));
  }
} // namespace

plEditorUiAudioAttenuation::plEditorUiAudioAttenuation(plDocument* pDocument)
  : plEditorUiContent("AudioAttenuation")
  , m_pDocument(pDocument)
{
  if (IsReady())
    m_PointerSubscription = GetSurface()->m_PointerEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioAttenuation::OnPointer, this));

  m_pDocument->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioAttenuation::PropertyEventHandler, this));
  m_pDocument->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plEditorUiAudioAttenuation::StructureEventHandler, this));
}

plEditorUiAudioAttenuation::~plEditorUiAudioAttenuation()
{
  m_pDocument->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plEditorUiAudioAttenuation::PropertyEventHandler, this));
  m_pDocument->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plEditorUiAudioAttenuation::StructureEventHandler, this));

  if (IsReady())
    GetSurface()->m_PointerEvents.RemoveEventHandler(m_PointerSubscription);
}

void plEditorUiAudioAttenuation::SetListenerDistance(float fDistance)
{
  if (m_Drag == Drag::Listener || m_fListenerDistance == fDistance)
    return;

  m_fListenerDistance = plMath::Max(fDistance, 0.0f);
  m_bDrawDirty = true;
  RequestRedraw();
}

float plEditorUiAudioAttenuation::GetMinimumHeight()
{
  return s_fMinimumHeight;
}

void plEditorUiAudioAttenuation::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  m_bDrawDirty = true;
  RequestRedraw();
}

void plEditorUiAudioAttenuation::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  m_bDrawDirty = true;
  RequestRedraw();
}

//////////////////////////////////////////////////////////////////////////
// The curve

const plDocumentObject* plEditorUiAudioAttenuation::GetPropertiesObject() const
{
  const auto& children = m_pDocument->GetObjectManager()->GetRootObject()->GetChildren();
  return children.IsEmpty() ? nullptr : children[0];
}

void plEditorUiAudioAttenuation::GatherPoints(plHybridArray<CurvePoint, 8>& out_points) const
{
  out_points.Clear();

  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  for (const plDocumentObject* pChild : pProps->GetChildren())
  {
    if (!pChild->GetParentProperty().IsEqual("CurvePoints"))
      continue;

    CurvePoint& point = out_points.ExpandAndGetRef();
    point.m_Guid = pChild->GetGuid();
    point.m_fX = pChild->GetTypeAccessor().GetValue("X").ConvertTo<float>();
    point.m_fY = pChild->GetTypeAccessor().GetValue("Y").ConvertTo<float>();

    if (m_Drag == Drag::Point && point.m_Guid == m_DragPointGuid)
    {
      point.m_fX = m_fDragX;
      point.m_fY = m_fDragY;
    }
  }

  out_points.Sort([](const CurvePoint& a, const CurvePoint& b)
    { return a.m_fX < b.m_fX; });
}

bool plEditorUiAudioAttenuation::GetCurve(plAudioEventDesc& out_desc, plString& out_sSharedName) const
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return false;

  const auto& accessor = pProps->GetTypeAccessor();
  out_desc.m_bSpatialize = true;
  out_sSharedName.Clear();

  // an event that uses a shared attenuation shows its saved curve
  if (HasProperty(pProps, "Attenuation"))
  {
    const plString sShared = accessor.GetValue("Attenuation").ConvertTo<plString>();
    if (!sShared.IsEmpty())
    {
      plAudioAttenuationAssetMetaInfo shared;
      plString sName;
      if (plAudioAttenuationAssetMetaInfo::Find(sShared, shared, &sName))
      {
        shared.ApplyTo(out_desc);
        out_sSharedName = sName.IsEmpty() ? sShared : sName;
      }
      else
      {
        out_sSharedName = "unknown asset";
      }

      return true;
    }
  }

  out_desc.m_eAttenuationCurve = static_cast<plAudioAttenuationCurve::Enum>(accessor.GetValue("Curve").ConvertTo<plInt64>());
  out_desc.m_fMinDistance = accessor.GetValue("MinDistance").ConvertTo<float>();
  out_desc.m_fMaxDistance = accessor.GetValue("MaxDistance").ConvertTo<float>();
  out_desc.m_fRolloff = accessor.GetValue("Rolloff").ConvertTo<float>();

  if (m_Drag == Drag::MinDistance)
    out_desc.m_fMinDistance = m_fDragDistance;
  else if (m_Drag == Drag::MaxDistance)
    out_desc.m_fMaxDistance = m_fDragDistance;

  plHybridArray<CurvePoint, 8> points;
  GatherPoints(points);
  for (const CurvePoint& point : points)
  {
    out_desc.m_AttenuationPoints.PushBack(plVec2(plMath::Clamp(point.m_fX, 0.0f, 1.0f), plMath::Clamp(point.m_fY, 0.0f, 1.0f)));
  }

  return true;
}

float plEditorUiAudioAttenuation::GetRange() const
{
  if (m_Drag != Drag::None)
    return m_fDragRange;

  plAudioEventDesc desc;
  plString sShared;
  if (!GetCurve(desc, sShared))
    return 100.0f;

  // a bit past the max distance, so its handle and the silence after it show
  const float fMax = plMath::Max(desc.m_fMinDistance, desc.m_fMaxDistance);
  return plMath::Max(fMax * 1.15f, fMax + 1.0f);
}

plRectFloat plEditorUiAudioAttenuation::GetPlotRect() const
{
  const plVec2 size = IsReady() ? GetSurface()->GetLogicalSize() : plVec2(300.0f, s_fMinimumHeight);
  return plRectFloat(s_fPad + 6.0f, s_fPlotTop, plMath::Max(size.x - 2.0f * s_fPad - 12.0f, 20.0f), plMath::Max(size.y - s_fPlotTop - s_fPlotBottomSpace, 40.0f));
}

float plEditorUiAudioAttenuation::DistanceToX(float fDistance) const
{
  const plRectFloat plot = GetPlotRect();
  return plot.x + fDistance / GetRange() * plot.width;
}

float plEditorUiAudioAttenuation::XToDistance(float x) const
{
  const plRectFloat plot = GetPlotRect();
  return plMath::Clamp((x - plot.x) / plot.width, 0.0f, 1.0f) * GetRange();
}

float plEditorUiAudioAttenuation::GainToY(float fGain) const
{
  const plRectFloat plot = GetPlotRect();
  return plot.y + GainToDb(fGain) / s_fFloorDb * plot.height;
}

float plEditorUiAudioAttenuation::YToGain(float y) const
{
  const plRectFloat plot = GetPlotRect();
  const float fDb = plMath::Clamp((y - plot.y) / plot.height, 0.0f, 1.0f) * s_fFloorDb;
  return fDb <= s_fFloorDb + 0.01f ? 0.0f : plMath::Pow(10.0f, fDb / 20.0f);
}

plEditorUiAudioAttenuation::Drag plEditorUiAudioAttenuation::PickHandle(const plVec2& vPos, plUuid& out_pointGuid) const
{
  plAudioEventDesc desc;
  plString sShared;
  if (!GetCurve(desc, sShared))
    return Drag::None;

  const plRectFloat plot = GetPlotRect();
  const plRectFloat area(plot.x - s_fGrab, plot.y - s_fGrab, plot.width + 2.0f * s_fGrab, plot.height + 2.0f * s_fGrab + 14.0f);
  if (!area.Contains(vPos))
    return Drag::None;

  const bool bListenerNear = plMath::Abs(vPos.x - DistanceToX(m_fListenerDistance)) <= s_fGrab;

  // the marker's flag at the top wins, so it can be picked up where it covers a distance handle
  if (bListenerNear && vPos.y <= plot.y + 14.0f)
    return Drag::Listener;

  if (sShared.IsEmpty())
  {
    if (desc.m_eAttenuationCurve == plAudioAttenuationCurve::Custom)
    {
      plHybridArray<CurvePoint, 8> points;
      GatherPoints(points);

      const float fMin = plMath::Min(desc.m_fMinDistance, desc.m_fMaxDistance);
      const float fMax = plMath::Max(desc.m_fMinDistance, desc.m_fMaxDistance);
      for (const CurvePoint& point : points)
      {
        const plVec2 vPoint(DistanceToX(plMath::Lerp(fMin, fMax, point.m_fX)), GainToY(point.m_fY));
        if ((vPoint - vPos).GetLength() <= s_fGrab + 1.0f)
        {
          out_pointGuid = point.m_Guid;
          return Drag::Point;
        }
      }
    }

    if (plMath::Abs(vPos.x - DistanceToX(desc.m_fMinDistance)) <= s_fGrab)
      return Drag::MinDistance;
    if (plMath::Abs(vPos.x - DistanceToX(desc.m_fMaxDistance)) <= s_fGrab)
      return Drag::MaxDistance;
  }

  return bListenerNear ? Drag::Listener : Drag::None;
}

//////////////////////////////////////////////////////////////////////////
// Document and drawing

void plEditorUiAudioAttenuation::OnUiUpdate()
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

void plEditorUiAudioAttenuation::BuildDocument()
{
  using namespace plEditorUi;

  m_bRebuild = false;
  m_vBuiltSize = GetSurface()->GetLogicalSize();

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = Surface();
  DrawList(pRoot, "Attenuation.Canvas", s_sDrawingEntry.GetView());

  plAudioEngineUi::HitButton(pRoot, plRectFloat(s_fPad, s_fCurveRowTop, 190.0f, ControlHeight), "Attenuation.Curve", "How the volume falls between the min and the max distance", &GetSurface()->GetBlackboard());

  SetDocument(Finish(pRoot));
  m_bDrawDirty = true;
}

void plEditorUiAudioAttenuation::Draw()
{
  using namespace plEditorUi;

  m_bDrawDirty = false;
  m_Writer.Clear();

  const plVec2 size = GetSurface()->GetLogicalSize();
  const plColorGammaUB audio = plAudioEngineUi::AudioColor();

  plAudioEventDesc desc;
  plString sShared;
  const bool bValid = GetCurve(desc, sShared);
  const bool bShared = !sShared.IsEmpty();

  plAudioEngineUi::SectionHeader(m_Writer, plRectFloat(0.0f, 0.0f, size.x, s_fHeader), "Attenuation", audio, bShared ? plStringView("shared") : plStringView());

  if (!bValid)
  {
    m_Writer.Publish(GetSurface()->GetBlackboard(), s_sDrawingEntry);
    return;
  }

  // the curve dropdown, or the shared asset that replaces it
  {
    const plRectFloat row(s_fPad, s_fCurveRowTop, 190.0f, ControlHeight);
    if (bShared)
    {
      plStringBuilder sText;
      sText.SetFormat("Shared: {}", sShared);
      m_Writer.Text(plRectFloat(s_fPad, s_fCurveRowTop, size.x - 2.0f * s_fPad, ControlHeight), sText, Foreground(), FontSize - 1);
    }
    else
    {
      const plUInt32 uiCurve = plMath::Min<plUInt32>(desc.m_eAttenuationCurve, PL_ARRAY_SIZE(s_szCurves) - 1);
      plAudioEngineUi::Dropdown(m_Writer, row, "Curve", s_szCurves[uiCurve]);
    }
  }

  const plRectFloat plot = GetPlotRect();
  const float fMin = plMath::Min(desc.m_fMinDistance, desc.m_fMaxDistance);
  const float fMax = plMath::Max(desc.m_fMinDistance, desc.m_fMaxDistance);
  const float fRange = GetRange();

  m_Writer.Rect(plRectFloat(plot.x - 6.0f, plot.y - 6.0f, plot.width + 12.0f, plot.height + 12.0f), Background(), 4.0f);

  // dB grid
  for (float fDb = 0.0f; fDb >= s_fFloorDb; fDb -= 20.0f)
  {
    const float y = plMath::Round(plot.y + fDb / s_fFloorDb * plot.height) + 0.5f;
    m_Writer.Line(plVec2(plot.x, y), plVec2(plot.x + plot.width, y), Line());

    plStringBuilder sDb;
    sDb.SetFormat("{} dB", static_cast<plInt32>(fDb));
    m_Writer.Text(plRectFloat(plot.x + plot.width - 48.0f, y - 12.0f, 46.0f, 11.0f), sDb, Dim(), MinFontSize, plUiTextHAlign::Right, plUiTextVAlign::Center, true);
  }

  // the attenuated range
  m_Writer.Rect(plRectFloat(DistanceToX(fMin), plot.y, DistanceToX(fMax) - DistanceToX(fMin), plot.height), plAudioEngineUi::Mix(Background(), audio, 0.07f));

  // the gain over distance, every two pixels
  {
    plHybridArray<plVec2, 256> line;
    for (float x = plot.x; x <= plot.x + plot.width + 0.5f; x += 2.0f)
    {
      line.PushBack(plVec2(x, GainToY(desc.GetDistanceGain(XToDistance(x)))));
    }

    m_Writer.Polyline(line, audio, 1.75f);
  }

  // the min and max distance handles
  for (plUInt32 i = 0; i < 2; ++i)
  {
    const float fDistance = i == 0 ? fMin : fMax;
    const float x = plMath::Round(DistanceToX(fDistance));
    const bool bActive = (i == 0 && m_Drag == Drag::MinDistance) || (i == 1 && m_Drag == Drag::MaxDistance);
    const plColorGammaUB color = bShared ? Dim() : (bActive ? Accent() : Foreground());

    m_Writer.Rect(plRectFloat(x - 0.75f, plot.y, 1.5f, plot.height), color);
    if (!bShared)
      m_Writer.Rect(plRectFloat(x - 4.0f, plot.y + plot.height - 3.0f, 8.0f, 6.0f), color, 1.0f);

    plStringBuilder sDistance, sLabel;
    FormatDistance(fDistance, sDistance);
    sLabel.SetFormat("{} {}", i == 0 ? "min" : "max", sDistance);
    const float fLabelWidth = MeasureCodeText(sLabel, MinFontSize) + 4.0f;
    const float fLabelX = plMath::Clamp(i == 0 ? x - fLabelWidth - 2.0f : x + 4.0f, plot.x - 6.0f, plot.x + plot.width + 6.0f - fLabelWidth);
    m_Writer.Text(plRectFloat(fLabelX, plot.y + plot.height + 6.0f, fLabelWidth, 12.0f), sLabel, Muted(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
  }

  // the points of a custom curve
  if (!bShared && desc.m_eAttenuationCurve == plAudioAttenuationCurve::Custom)
  {
    plHybridArray<CurvePoint, 8> points;
    GatherPoints(points);

    for (const CurvePoint& point : points)
    {
      const bool bDragged = m_Drag == Drag::Point && point.m_Guid == m_DragPointGuid;
      const plVec2 vPoint(DistanceToX(plMath::Lerp(fMin, fMax, plMath::Clamp(point.m_fX, 0.0f, 1.0f))), GainToY(point.m_fY));
      m_Writer.Circle(vPoint, bDragged ? 4.5f : 3.5f, bDragged ? Accent() : Foreground());
    }
  }

  // the listener marker
  {
    const float x = plMath::Round(DistanceToX(plMath::Min(m_fListenerDistance, fRange))) + 0.5f;
    m_Writer.Line(plVec2(x, plot.y), plVec2(x, plot.y + plot.height), Accent(), 1.25f, 3.0f, 3.0f);

    plDynamicArray<plVec2> flag;
    flag.PushBack(plVec2(x - 5.0f, plot.y - 6.0f));
    flag.PushBack(plVec2(x + 5.0f, plot.y - 6.0f));
    flag.PushBack(plVec2(x, plot.y + 2.0f));
    m_Writer.Triangles(flag, Accent());

    const plStringView sLabel = "listener";
    const float fLabelWidth = MeasureText(sLabel, MinFontSize) + 4.0f;
    const float fLabelX = x + fLabelWidth + 8.0f > plot.x + plot.width ? x - fLabelWidth - 6.0f : x + 6.0f;
    m_Writer.Text(plRectFloat(fLabelX, plot.y + 2.0f, fLabelWidth, 12.0f), sLabel, Accent(), MinFontSize);
  }

  // the caption: the gain at the listener
  {
    plStringBuilder sDistance, sCaption;
    FormatDistance(m_fListenerDistance, sDistance);

    const float fGain = desc.GetDistanceGain(m_fListenerDistance);
    if (fGain <= 0.001f)
      sCaption.SetFormat("at the listener ({}): silent", sDistance);
    else
      sCaption.SetFormat("at the listener ({}): {} dB", sDistance, plArgF(20.0f * plMath::Log10(fGain), 1));

    sCaption.Append(" \xC2\xB7 fades to silence before the max distance, no hard cut");
    m_Writer.Text(plRectFloat(s_fPad, size.y - 18.0f, size.x - 2.0f * s_fPad, 14.0f), sCaption, Muted(), MinFontSize);
  }

  m_Writer.Publish(GetSurface()->GetBlackboard(), s_sDrawingEntry);
}

plString plEditorUiAudioAttenuation::GetToolTipAt(const plVec2& vLogical) const
{
  plUuid pointGuid;
  switch (PickHandle(vLogical, pointGuid))
  {
    case Drag::MinDistance:
      return "Min distance: full volume up to here (drag)";
    case Drag::MaxDistance:
      return "Max distance: silent from here; the event becomes virtual (drag)";
    case Drag::Point:
      return "Drag the point \xC2\xB7 right-click removes it";
    case Drag::Listener:
      return "The listener distance the audition plays at (drag; the Distance tab scrubs it too)";
    default:
      break;
  }

  if (GetPlotRect().Contains(vLogical))
  {
    plAudioEventDesc desc;
    plString sShared;
    if (GetCurve(desc, sShared) && sShared.IsEmpty() && desc.m_eAttenuationCurve == plAudioAttenuationCurve::Custom)
      return "Double-click adds a point to the custom curve";
  }

  return plEditorUiContent::GetToolTipAt(vLogical);
}

//////////////////////////////////////////////////////////////////////////
// Editing

void plEditorUiAudioAttenuation::OnUiEvent(const plMsgUiEvent& event)
{
  if (event.m_sIdentifier.GetView() == "Attenuation.Curve")
    ShowCurveMenu();
}

void plEditorUiAudioAttenuation::ShowCurveMenu()
{
  plAudioEventDesc desc;
  plString sShared;
  if (!GetCurve(desc, sShared) || !sShared.IsEmpty())
    return;

  plEditorUiMenu menu;
  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_szCurves); ++i)
  {
    plEditorUiMenuItem& item = menu.AddAction(s_szCurves[i], i);
    item.m_bCheckable = true;
    item.m_bChecked = static_cast<plUInt32>(desc.m_eAttenuationCurve) == i;
  }

  plEditorUiMenuView::Show(*GetSurface(), plVec2(s_fPad, s_fCurveRowTop + plEditorUi::ControlHeight), menu, [this](plUInt32 uiId)
    { SetCurve(static_cast<plInt64>(uiId)); });
}

void plEditorUiAudioAttenuation::SetCurve(plInt64 iCurve)
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr || pProps->GetTypeAccessor().GetValue("Curve").ConvertTo<plInt64>() == iCurve)
    return;

  plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
  accessor.StartTransaction("Set Attenuation Curve");
  accessor.SetValueByName(pProps, "Curve", iCurve).IgnoreResult();

  // a new custom curve starts as a gentle bend to silence
  plHybridArray<CurvePoint, 8> points;
  GatherPoints(points);
  if (iCurve == plAudioAttenuationCurve::Custom && points.IsEmpty())
  {
    const plVec2 seed[] = {plVec2(0.25f, 0.55f), plVec2(0.6f, 0.2f), plVec2(1.0f, 0.0f)};
    for (const plVec2& point : seed)
    {
      plUuid pointGuid;
      if (accessor.AddObjectByName(pProps, "CurvePoints", -1, plGetStaticRTTI<plAudioEventCurvePoint>(), pointGuid).Succeeded())
      {
        const plDocumentObject* pPoint = m_pDocument->GetObjectManager()->GetObject(pointGuid);
        accessor.SetValueByName(pPoint, "X", point.x).IgnoreResult();
        accessor.SetValueByName(pPoint, "Y", point.y).IgnoreResult();
      }
    }
  }

  accessor.FinishTransaction();
}

void plEditorUiAudioAttenuation::CommitDrag()
{
  const Drag drag = m_Drag;
  m_Drag = Drag::None;
  m_bDrawDirty = true;

  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());

  if (drag == Drag::MinDistance || drag == Drag::MaxDistance)
  {
    const char* szProperty = drag == Drag::MinDistance ? "MinDistance" : "MaxDistance";
    if (plMath::Abs(pProps->GetTypeAccessor().GetValue(szProperty).ConvertTo<float>() - m_fDragDistance) < 0.0001f)
      return;

    accessor.StartTransaction("Edit Attenuation Distance");
    accessor.SetValueByName(pProps, szProperty, m_fDragDistance).IgnoreResult();
    accessor.FinishTransaction();
  }
  else if (drag == Drag::Point)
  {
    const plDocumentObject* pPoint = FindChild(pProps, m_DragPointGuid);
    m_DragPointGuid = plUuid();
    if (pPoint == nullptr)
      return;

    accessor.StartTransaction("Move Attenuation Point");
    accessor.SetValueByName(pPoint, "X", m_fDragX).IgnoreResult();
    accessor.SetValueByName(pPoint, "Y", m_fDragY).IgnoreResult();
    accessor.FinishTransaction();
  }
}

void plEditorUiAudioAttenuation::OnPointer(const plEditorUiPointerEvent& e)
{
  using Type = plEditorUiPointerEvent::Type;
  using Button = plEditorUiPointerEvent::Button;

  switch (e.m_Type)
  {
    case Type::Press:
    {
      if (!e.m_sElement.IsEmpty())
        return;

      plUuid pointGuid;
      const Drag drag = PickHandle(e.m_vPosition, pointGuid);
      if (drag == Drag::None)
        return;

      // right-click removes a point of the custom curve
      if (e.m_Button == Button::Right)
      {
        const plDocumentObject* pProps = GetPropertiesObject();
        const plDocumentObject* pPoint = (drag == Drag::Point && pProps != nullptr) ? FindChild(pProps, pointGuid) : nullptr;
        if (pPoint != nullptr)
        {
          plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
          accessor.StartTransaction("Remove Attenuation Point");
          if (accessor.RemoveObject(pPoint).Failed())
            accessor.CancelTransaction();
          else
            accessor.FinishTransaction();
        }
        return;
      }

      if (e.m_Button != Button::Left)
        return;

      m_fDragRange = GetRange();
      m_Drag = drag;
      m_DragPointGuid = pointGuid;

      plAudioEventDesc desc;
      plString sShared;
      GetCurve(desc, sShared);
      m_fDragDistance = drag == Drag::MinDistance ? desc.m_fMinDistance : desc.m_fMaxDistance;

      plHybridArray<CurvePoint, 8> points;
      GatherPoints(points);
      for (const CurvePoint& point : points)
      {
        if (point.m_Guid == pointGuid)
        {
          m_fDragX = point.m_fX;
          m_fDragY = point.m_fY;
        }
      }

      m_bDrawDirty = true;
      return;
    }

    case Type::Move:
    case Type::DragStart:
    case Type::DragMove:
    {
      if (m_Drag == Drag::None)
        return;

      const plDocumentObject* pProps = GetPropertiesObject();
      if (pProps == nullptr)
        return;

      const float fDistance = XToDistance(e.m_vPosition.x);
      const float fStoredMin = pProps->GetTypeAccessor().GetValue("MinDistance").ConvertTo<float>();
      const float fStoredMax = pProps->GetTypeAccessor().GetValue("MaxDistance").ConvertTo<float>();
      const float fStep = m_fDragRange >= 50.0f ? 0.5f : 0.1f;

      if (m_Drag == Drag::MinDistance)
        m_fDragDistance = plMath::Clamp(plMath::RoundToMultiple(fDistance, fStep), 0.01f, plMath::Max(fStoredMax - fStep, 0.01f));
      else if (m_Drag == Drag::MaxDistance)
        m_fDragDistance = plMath::Max(plMath::RoundToMultiple(fDistance, fStep), fStoredMin + fStep);
      else if (m_Drag == Drag::Listener)
      {
        m_fListenerDistance = plMath::RoundToMultiple(fDistance, fStep);
        if (m_OnListenerDistanceChanged.IsValid())
          m_OnListenerDistanceChanged(m_fListenerDistance);
      }
      else if (m_Drag == Drag::Point)
      {
        // x is the share of the way from min to max
        const float fMin = plMath::Min(fStoredMin, fStoredMax);
        const float fMax = plMath::Max(fStoredMin, fStoredMax);
        m_fDragX = plMath::Clamp((fDistance - fMin) / plMath::Max(fMax - fMin, 0.0001f), 0.0f, 1.0f);
        m_fDragY = YToGain(e.m_vPosition.y);
      }

      m_bDrawDirty = true;
      return;
    }

    case Type::Release:
    case Type::Drop:
    {
      if (m_Drag == Drag::Listener)
      {
        m_Drag = Drag::None;
        m_bDrawDirty = true;
      }
      else if (m_Drag != Drag::None)
      {
        CommitDrag();
      }
      return;
    }

    case Type::DragCancel:
      m_Drag = Drag::None;
      m_DragPointGuid = plUuid();
      m_bDrawDirty = true;
      return;

    case Type::DoubleClick:
    {
      if (e.m_Button != Button::Left || !e.m_sElement.IsEmpty() || !GetPlotRect().Contains(e.m_vPosition))
        return;

      plUuid pointGuid;
      if (PickHandle(e.m_vPosition, pointGuid) == Drag::Point)
        return;

      plAudioEventDesc desc;
      plString sShared;
      const plDocumentObject* pProps = GetPropertiesObject();
      if (pProps == nullptr || !GetCurve(desc, sShared) || !sShared.IsEmpty() || desc.m_eAttenuationCurve != plAudioAttenuationCurve::Custom)
        return;

      const float fMin = plMath::Min(desc.m_fMinDistance, desc.m_fMaxDistance);
      const float fMax = plMath::Max(desc.m_fMinDistance, desc.m_fMaxDistance);
      const float fX = plMath::Clamp((XToDistance(e.m_vPosition.x) - fMin) / plMath::Max(fMax - fMin, 0.0001f), 0.0f, 1.0f);

      plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
      accessor.StartTransaction("Add Attenuation Point");

      if (accessor.AddObjectByName(pProps, "CurvePoints", -1, plGetStaticRTTI<plAudioEventCurvePoint>(), pointGuid).Failed())
      {
        accessor.CancelTransaction();
        return;
      }

      const plDocumentObject* pPoint = m_pDocument->GetObjectManager()->GetObject(pointGuid);
      accessor.SetValueByName(pPoint, "X", fX).IgnoreResult();
      accessor.SetValueByName(pPoint, "Y", YToGain(e.m_vPosition.y)).IgnoreResult();
      accessor.FinishTransaction();
      return;
    }

    default:
      return;
  }
}
