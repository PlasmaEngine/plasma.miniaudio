#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/EngineUi/AudioShellPanels.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>

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

  // a part that was taken out is no longer a child
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
