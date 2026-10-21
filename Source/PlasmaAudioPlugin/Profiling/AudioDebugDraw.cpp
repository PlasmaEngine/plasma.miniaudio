#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <AudioSystem/Components/AudioBoxEnvironmentComponent.h>
#include <AudioSystem/Components/AudioSphereEnvironmentComponent.h>
#include <Core/World/World.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/Mutex.h>
#include <PlasmaAudioPlugin/Profiling/AudioDebugDraw.h>
#include <RendererCore/Debug/DebugRenderer.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

#include <atomic>

plCVarBool cvar_AudioDebugDraw("Audio.Debug.Draw", false, plCVarFlags::Default, "Draws the Audio view mode's overlay over the main view (the editor's Audio render mode draws it in its viewport).");
plCVarInt cvar_AudioDebugLayers("Audio.Debug.Layers", plAudioDebugDraw::All, plCVarFlags::Default, "Layers of the audio overlay: 1 emitters, 2 attenuation ranges, 4 occlusion rays, 8 environment zones, 16 labels.");

namespace
{
  plMutex s_SnapshotMutex;
  plAudioDebugSnapshot s_Snapshot;
  std::atomic<plInt64> s_iLastWantedMicroseconds = -1000000000;

  const plColor s_AudioColor = plColorGammaUB(200, 140, 185);
  const plColor s_ListenerColor = plColorGammaUB(11, 222, 179);
  const plColor s_ClearRayColor = plColorGammaUB(105, 219, 124, 170);
  const plColor s_OccludedRayColor = plColorGammaUB(255, 107, 107, 190);
  const plColor s_VirtualColor = plColorGammaUB(138, 138, 148);

  /// "-6.0 dB", "-inf dB"
  void FormatGain(float fGain, plStringBuilder& out_sText)
  {
    if (fGain <= 0.001f)
      out_sText = "-inf dB";
    else
      out_sText.SetFormat("{} dB", plArgF(20.0f * plMath::Log10(fGain), 1));
  }

  template <typename MANAGER, typename DRAW>
  void ForEachZone(const plWorld* pWorld, DRAW draw)
  {
    const MANAGER* pManager = pWorld->GetComponentManager<MANAGER>();
    if (pManager == nullptr)
      return;

    for (auto it = pManager->GetComponents(); it.IsValid(); ++it)
    {
      if (it->IsActiveAndInitialized())
        draw(*it);
    }
  }
} // namespace

void plAudioDebugDraw::Startup()
{
  plRenderWorld::GetExtractionEvent().AddEventHandler(&plAudioDebugDraw::OnExtractionEvent);
}

void plAudioDebugDraw::Shutdown()
{
  plRenderWorld::GetExtractionEvent().RemoveEventHandler(&plAudioDebugDraw::OnExtractionEvent);

  PL_LOCK(s_SnapshotMutex);
  s_Snapshot = plAudioDebugSnapshot();
}

bool plAudioDebugDraw::IsWanted()
{
  return plTime::Now().GetMicroseconds() - static_cast<double>(s_iLastWantedMicroseconds.load(std::memory_order_relaxed)) < 1000000.0;
}

void plAudioDebugDraw::Publish(plAudioDebugSnapshot&& snapshot)
{
  PL_LOCK(s_SnapshotMutex);
  s_Snapshot = std::move(snapshot);
}

void plAudioDebugDraw::OnExtractionEvent(const plRenderWorldExtractionEvent& e)
{
  if (e.m_Type != plRenderWorldExtractionEvent::Type::BeforeViewExtraction || e.m_pView == nullptr)
    return;

  const plView* pView = e.m_pView;
  const bool bAudioMode = pView->GetViewRenderMode() == plViewRenderMode::Audio;
  const bool bMainView = pView->GetCameraUsageHint() == plCameraUsageHint::MainView || pView->GetCameraUsageHint() == plCameraUsageHint::EditorView;

  if (!bAudioMode && !(cvar_AudioDebugDraw.GetValue() && bMainView))
    return;

  // the audio thread keeps publishing while views draw
  s_iLastWantedMicroseconds.store(static_cast<plInt64>(plTime::Now().GetMicroseconds()), std::memory_order_relaxed);

  plAudioDebugSnapshot snapshot;
  {
    PL_LOCK(s_SnapshotMutex);
    snapshot = s_Snapshot;
  }

  DrawView(pView, snapshot, static_cast<plUInt32>(cvar_AudioDebugLayers.GetValue()));
}

void plAudioDebugDraw::DrawView(const plView* pView, const plAudioDebugSnapshot& snapshot, plUInt32 uiLayers)
{
  const plDebugRendererContext context(pView->GetHandle());

  // environment zones: the full-amount shape and the distance where the amount reaches zero
  if ((uiLayers & Environments) != 0 && pView->GetWorld() != nullptr)
  {
    const plWorld* pWorld = pView->GetWorld();
    PL_LOCK(pWorld->GetReadMarker());

    auto zoneLabel = [&](const plAudioSystemEnvironmentComponent& zone, const plVec3& vAt, const plColor& color)
    {
      if ((uiLayers & Labels) != 0)
      {
        plDebugRenderer::Draw3DText(context, plFmt("{} ({} m falloff)", zone.GetEnvironmentName(), plArgF(zone.GetMaxDistance(), 1)), vAt, color, 14);
      }
    };

    ForEachZone<plAudioBoxEnvironmentComponentManager>(pWorld, [&](const plAudioBoxEnvironmentComponent& zone)
      {
        const plTransform transform = zone.GetOwner()->GetGlobalTransform();
        const plColor color = zone.GetShapeColor();
        plDebugRenderer::DrawLineBox(context, plBoundingBox::MakeFromCenterAndHalfExtents(plVec3::MakeZero(), zone.GetHalfExtends()), color, transform);
        plDebugRenderer::DrawLineSphere(context, plBoundingSphere::MakeFromCenterAndRadius(plVec3::MakeZero(), zone.GetMaxDistance()), color.WithAlpha(0.35f), plTransform(transform.m_vPosition));
        zoneLabel(zone, transform.m_vPosition, color); });

    ForEachZone<plAudioSphereEnvironmentComponentManager>(pWorld, [&](const plAudioSphereEnvironmentComponent& zone)
      {
        const plVec3 vCenter = zone.GetOwner()->GetGlobalPosition();
        const plColor color = zone.GetShapeColor();
        plDebugRenderer::DrawLineSphere(context, plBoundingSphere::MakeFromCenterAndRadius(vCenter, zone.GetRadius()), color);
        plDebugRenderer::DrawLineSphere(context, plBoundingSphere::MakeFromCenterAndRadius(vCenter, zone.GetMaxDistance()), color.WithAlpha(0.35f));
        zoneLabel(zone, vCenter, color); });
  }

  for (const plVec3& vListener : snapshot.m_Listeners)
  {
    plDebugRenderer::DrawCross(context, vListener, 0.6f, s_ListenerColor);
  }

  const plAudioDebugEmitter* pLoudest = nullptr;

  for (const plAudioDebugEmitter& emitter : snapshot.m_Emitters)
  {
    if (pLoudest == nullptr || emitter.m_fAudibility > pLoudest->m_fAudibility)
      pLoudest = &emitter;

    if ((uiLayers & Ranges) != 0)
    {
      plDebugRenderer::DrawLineSphere(context, plBoundingSphere::MakeFromCenterAndRadius(emitter.m_vPosition, emitter.m_fMinDistance), s_AudioColor.WithAlpha(0.6f));
      plDebugRenderer::DrawLineSphere(context, plBoundingSphere::MakeFromCenterAndRadius(emitter.m_vPosition, emitter.m_fMaxDistance), s_AudioColor.WithAlpha(0.22f));
    }

    if ((uiLayers & Occlusion) != 0 && emitter.m_fDistance < emitter.m_fMaxDistance)
    {
      const plDebugRenderer::Line ray(emitter.m_vListener, emitter.m_vPosition);
      plDebugRenderer::DrawLines(context, plArrayPtr<const plDebugRenderer::Line>(&ray, 1), emitter.m_fOcclusion > 0.05f ? s_OccludedRayColor : s_ClearRayColor);
    }

    if ((uiLayers & Emitters) != 0)
    {
      // a real voice is a filled box that is the brighter the more audible, a virtual one an outline
      const plBoundingBox box = plBoundingBox::MakeFromCenterAndHalfExtents(emitter.m_vPosition, plVec3(0.18f));
      if (emitter.m_bReal)
        plDebugRenderer::DrawSolidBox(context, box, s_AudioColor.WithAlpha(plMath::Clamp(0.35f + emitter.m_fAudibility * 1.4f, 0.35f, 1.0f)));
      else
        plDebugRenderer::DrawLineBox(context, box, s_VirtualColor);

      // the cone's direction
      if ((emitter.m_fConeInnerAngle < 360.0f || emitter.m_fConeOuterAngle < 360.0f) && !emitter.m_vForward.IsZero())
      {
        const plDebugRenderer::Line dir(emitter.m_vPosition, emitter.m_vPosition + emitter.m_vForward.GetNormalized() * 1.0f);
        plDebugRenderer::DrawLines(context, plArrayPtr<const plDebugRenderer::Line>(&dir, 1), s_AudioColor);
      }
    }

    if ((uiLayers & Labels) != 0)
    {
      plStringBuilder sGain;
      FormatGain(emitter.m_fGain, sGain);

      plStringBuilder sLabel;
      sLabel.SetFormat("{}  {}", plPathUtils::GetFileName(emitter.m_sEvent), sGain);
      if (emitter.m_fOcclusion > 0.05f)
        sLabel.AppendFormat("  occ {}", plArgF(emitter.m_fOcclusion, 2));
      if (!emitter.m_bReal)
        sLabel.Append("  virtual");

      plDebugRenderer::Draw3DText(context, sLabel.GetView(), emitter.m_vPosition + plVec3(0, 0, 0.4f), emitter.m_bReal ? plColor::White : s_VirtualColor, 14);
    }
  }

  // the summary in the corner, and the numbers of the most audible emitter
  // the summary and the legend; the view mode itself shows no text
  plDebugRenderer::DrawInfoText(context, plDebugTextPlacement::TopLeft, "Audio", plFmt("Audio  real {}/{}  virtual {}  emitters {}", snapshot.m_uiRealVoices, snapshot.m_uiMaxRealVoices, snapshot.m_uiVirtualVoices, snapshot.m_Emitters.GetCount()), s_AudioColor);
  plDebugRenderer::DrawInfoText(context, plDebugTextPlacement::TopLeft, "Audio", "filled box: real voice  outline: virtual  rays: red when occluded  layers: Audio.Debug.Layers");

  if (pLoudest != nullptr)
  {
    plStringBuilder sGain;
    FormatGain(pLoudest->m_fGain, sGain);

    plStringBuilder sEnvironments;
    for (plUInt32 i = 0; i < pLoudest->m_EnvironmentNames.GetCount(); ++i)
    {
      sEnvironments.AppendFormat("{}{} {}", i > 0 ? "  " : "", pLoudest->m_EnvironmentNames[i], plArgF(pLoudest->m_EnvironmentAmounts[i], 2));
    }

    plDebugRenderer::DrawInfoText(context, plDebugTextPlacement::TopLeft, "Audio", plFmt("Loudest: {}", plPathUtils::GetFileName(pLoudest->m_sEvent)));
    plDebugRenderer::DrawInfoText(context, plDebugTextPlacement::TopLeft, "Audio", plFmt("distance {} m  gain {}  occlusion {}  audibility {}", plArgF(pLoudest->m_fDistance, 1), sGain, plArgF(pLoudest->m_fOcclusion, 2), plArgF(pLoudest->m_fAudibility, 3)));
    plDebugRenderer::DrawInfoText(context, plDebugTextPlacement::TopLeft, "Audio", plFmt("environments: {}", sEnvironments.IsEmpty() ? plStringView("none") : sEnvironments.GetView()));
  }
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Profiling_AudioDebugDraw);
