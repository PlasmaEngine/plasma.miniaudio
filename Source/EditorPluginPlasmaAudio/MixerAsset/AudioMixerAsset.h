#pragma once

#include <PlasmaAudioPlugin/Mixer/AudioMixerData.h>
#include <EditorFramework/Assets/SimpleAssetDocument.h>

/// \brief One insert effect on a mixer bus. Parameter slot meaning depends on the effect type
/// (see plAudioEffectType documentation).
class plAudioMixerEffect : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerEffect, plReflectedClass);

public:
  plEnum<plAudioEffectType> m_eType;
  plDynamicArray<float> m_Params;
};

/// \brief An aux send from a bus to an aux/return bus (by name).
class plAudioMixerSend : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerSend, plReflectedClass);

public:
  plString m_sTargetBus;
  float m_fLevelDb = 0.0f;
};

/// \brief One bus of the mixer. The first bus in the list is always the master bus.
class plAudioMixerBus : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerBus, plReflectedClass);

public:
  plString m_sName;
  plString m_sParent; // empty = master (first bus)
  float m_fVolumeDb = 0.0f;
  float m_fPitch = 1.0f;
  bool m_bIsAux = false;
  plString m_sEnvironmentName; // aux only
  plDynamicArray<plAudioMixerEffect> m_Effects;
  plDynamicArray<plAudioMixerSend> m_Sends;
};

/// \brief A VCA: gain applied on top of a set of buses (options-menu volume sliders).
class plAudioMixerVca : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerVca, plReflectedClass);

public:
  plString m_sName;
  float m_fVolumeDb = 0.0f;
  plDynamicArray<plString> m_Buses;
};

/// \brief One overridden property inside a snapshot.
class plAudioMixerSnapshotOverride : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerSnapshotOverride, plReflectedClass);

public:
  plString m_sBus;
  plEnum<plAudioSnapshotTarget> m_eTarget;
  plUInt8 m_uiIndexA = 0; // send index or effect index
  plUInt8 m_uiIndexB = 0; // effect param index
  float m_fValue = 0.0f;
};

/// \brief A blendable mixer state with fade times.
class plAudioMixerSnapshot : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerSnapshot, plReflectedClass);

public:
  plString m_sName;
  float m_fFadeIn = 0.5f;  // seconds
  float m_fFadeOut = 0.5f; // seconds
  plUInt8 m_uiPriority = 0;
  plDynamicArray<plAudioMixerSnapshotOverride> m_Overrides;
};

/// \brief Names this mixer defines, stored in the asset's document info.
///
/// The asset curator caches this, so the audio components can offer bus / environment / snapshot
/// dropdowns without the mixer document being open.
class plAudioMixerAssetMetaInfo : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerAssetMetaInfo, plReflectedClass);

public:
  plDynamicArray<plString> m_Buses;       ///< every bus, aux included (snapshot overrides may target any of them)
  plDynamicArray<plString> m_OutputBuses; ///< non-aux buses only — the ones an event may output to
  plDynamicArray<plString> m_Environments;
  plDynamicArray<plString> m_Snapshots;
};

class plAudioMixerAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerAssetProperties, plReflectedClass);

public:
  static void PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e);

  /// \brief Converts the authored properties into the runtime description, resolving all
  /// name references to indices. Shared by the asset transform and the editor's live preview.
  static plStatus BuildRuntimeDesc(const plAudioMixerAssetProperties& props, plAudioMixerDesc& out_desc);

  plDynamicArray<plAudioMixerBus> m_Buses;
  plDynamicArray<plAudioMixerVca> m_Vcas;
  plDynamicArray<plAudioMixerSnapshot> m_Snapshots;
};

class plAudioMixerAssetDocument : public plSimpleAssetDocument<plAudioMixerAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerAssetDocument, plSimpleAssetDocument<plAudioMixerAssetProperties>);

public:
  plAudioMixerAssetDocument(plStringView sDocumentPath);

protected:
  virtual void UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const override;
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile,
    const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
};
