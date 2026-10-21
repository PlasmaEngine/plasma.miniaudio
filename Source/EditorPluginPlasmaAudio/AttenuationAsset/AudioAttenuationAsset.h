#pragma once

#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAsset.h>
#include <PlasmaAudioPlugin/Events/AudioEventResource.h>

/// \brief A distance attenuation that several events share: the curve and the min and max distance.
///
/// Uses the property names of the event's inline attenuation (Curve, MinDistance, MaxDistance, Rolloff, CurvePoints),
/// so the attenuation panel edits both.
class plAudioAttenuationAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioAttenuationAssetProperties, plReflectedClass);

public:
  static void PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);

  plEnum<plAudioAttenuationCurve> m_eCurve;
  float m_fMinDistance = 1.0f;
  float m_fMaxDistance = 100.0f;
  float m_fRolloff = 1.0f;
  plDynamicArray<plAudioEventCurvePoint> m_CurvePoints; ///< Custom curve: x = 0..1 between min and max, y = gain
};

/// \brief The attenuation as the asset curator keeps it in the asset's document info. Events bake it into their
/// description when they transform; the asset has no output of its own.
class plAudioAttenuationAssetMetaInfo : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioAttenuationAssetMetaInfo, plReflectedClass);

public:
  plEnum<plAudioAttenuationCurve> m_eCurve;
  float m_fMinDistance = 1.0f;
  float m_fMaxDistance = 100.0f;
  float m_fRolloff = 1.0f;
  plDynamicArray<plVec2> m_Points;

  /// \brief Writes the curve and the distances into an event description.
  void ApplyTo(plAudioEventDesc& ref_desc) const;

  /// \brief The saved attenuation of the asset sAssetGuid. False when the asset is unknown.
  static bool Find(plStringView sAssetGuid, plAudioAttenuationAssetMetaInfo& out_info, plString* out_pDisplayName = nullptr);
};

class plAudioAttenuationAssetDocument : public plSimpleAssetDocument<plAudioAttenuationAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioAttenuationAssetDocument, plSimpleAssetDocument<plAudioAttenuationAssetProperties>);

public:
  plAudioAttenuationAssetDocument(plStringView sDocumentPath);

protected:
  virtual void UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const override;
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile,
    const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
};
