#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/AttenuationAsset/AudioAttenuationAsset.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioAttenuationAssetProperties, 1, plRTTIDefaultAllocator<plAudioAttenuationAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Curve", plAudioAttenuationCurve, m_eCurve),
    PL_MEMBER_PROPERTY("MinDistance", m_fMinDistance)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.01f, 10000.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("MaxDistance", m_fMaxDistance)->AddAttributes(new plDefaultValueAttribute(100.0f), new plClampValueAttribute(0.01f, 10000.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Rolloff", m_fRolloff)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.001f, 1000.0f)),
    PL_ARRAY_MEMBER_PROPERTY("CurvePoints", m_CurvePoints),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioAttenuationAssetMetaInfo, 1, plRTTIDefaultAllocator<plAudioAttenuationAssetMetaInfo>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Curve", plAudioAttenuationCurve, m_eCurve),
    PL_MEMBER_PROPERTY("MinDistance", m_fMinDistance)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("MaxDistance", m_fMaxDistance)->AddAttributes(new plDefaultValueAttribute(100.0f)),
    PL_MEMBER_PROPERTY("Rolloff", m_fRolloff)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_ARRAY_MEMBER_PROPERTY("Points", m_Points),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioAttenuationAssetDocument, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

void plAudioAttenuationAssetProperties::PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  if (e.m_pObject->GetTypeAccessor().GetType() != plGetStaticRTTI<plAudioAttenuationAssetProperties>())
    return;

  auto& props = *e.m_pPropertyStates;

  // the points are dragged in the curve panel
  const plInt64 iCurve = e.m_pObject->GetTypeAccessor().GetValue("Curve").ConvertTo<plInt64>();
  props["Rolloff"].m_Visibility = iCurve == plAudioAttenuationCurve::Inverse ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  props["CurvePoints"].m_Visibility = plPropertyUiState::Invisible;
}

void plAudioAttenuationAssetMetaInfo::ApplyTo(plAudioEventDesc& ref_desc) const
{
  ref_desc.m_eAttenuationCurve = m_eCurve;
  ref_desc.m_fMinDistance = plMath::Min(m_fMinDistance, m_fMaxDistance);
  ref_desc.m_fMaxDistance = plMath::Max(m_fMinDistance, m_fMaxDistance);
  ref_desc.m_fRolloff = m_fRolloff;

  ref_desc.m_AttenuationPoints.Clear();
  for (const plVec2& point : m_Points)
  {
    ref_desc.m_AttenuationPoints.PushBack(point);
  }
}

bool plAudioAttenuationAssetMetaInfo::Find(plStringView sAssetGuid, plAudioAttenuationAssetMetaInfo& out_info, plString* out_pDisplayName)
{
  if (sAssetGuid.IsEmpty())
    return false;

  const auto pSubAsset = plAssetCurator::GetSingleton()->FindSubAsset(sAssetGuid);
  if (!pSubAsset.isValid() || pSubAsset->m_pAssetInfo == nullptr || pSubAsset->m_pAssetInfo->m_Info == nullptr)
    return false;

  const plAudioAttenuationAssetMetaInfo* pMeta = pSubAsset->m_pAssetInfo->m_Info->GetMetaInfo<plAudioAttenuationAssetMetaInfo>();
  if (pMeta == nullptr)
    return false;

  if (out_pDisplayName != nullptr)
    *out_pDisplayName = pSubAsset->GetName();

  out_info.m_eCurve = pMeta->m_eCurve;
  out_info.m_fMinDistance = pMeta->m_fMinDistance;
  out_info.m_fMaxDistance = pMeta->m_fMaxDistance;
  out_info.m_fRolloff = pMeta->m_fRolloff;
  out_info.m_Points = pMeta->m_Points;
  return true;
}

plAudioAttenuationAssetDocument::plAudioAttenuationAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plAudioAttenuationAssetProperties>(sDocumentPath, plAssetDocEngineConnection::None)
{
}

void plAudioAttenuationAssetDocument::UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const
{
  SUPER::UpdateAssetDocumentInfo(pInfo);

  const plAudioAttenuationAssetProperties* pProp = GetProperties();

  plAudioAttenuationAssetMetaInfo* pMeta = PL_DEFAULT_NEW(plAudioAttenuationAssetMetaInfo);
  pMeta->m_eCurve = pProp->m_eCurve;
  pMeta->m_fMinDistance = pProp->m_fMinDistance;
  pMeta->m_fMaxDistance = pProp->m_fMaxDistance;
  pMeta->m_fRolloff = pProp->m_fRolloff;

  for (const plAudioEventCurvePoint& point : pProp->m_CurvePoints)
  {
    pMeta->m_Points.PushBack(plVec2(plMath::Clamp(point.m_fX, 0.0f, 1.0f), plMath::Clamp(point.m_fY, 0.0f, 1.0f)));
  }

  pMeta->m_Points.Sort([](const plVec2& a, const plVec2& b)
    { return a.x < b.x; });

  pInfo->m_MetaInfo.PushBack(pMeta);
}

plTransformStatus plAudioAttenuationAssetDocument::InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  // the type disables the transform: the events that use the attenuation bake it
  return plStatus(PL_SUCCESS);
}
