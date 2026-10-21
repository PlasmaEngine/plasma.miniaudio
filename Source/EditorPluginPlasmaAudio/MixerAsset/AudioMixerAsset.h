#pragma once

#include <AudioSystem/Mixer/AudioMixerData.h>
#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <EnginePluginPlasmaAudio/Mixer/AudioMixerConsoleMessages.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>

/// \brief One insert effect on a mixer bus. The parameters are in the slot order of the effect registry
/// (plAudioEffectRegistry); missing ones use the registry defaults.
class plAudioMixerEffect : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerEffect, plReflectedClass);

public:
  plEnum<plAudioEffectType> m_eType;

  /// \brief How snapshot overrides address the effect on its bus. Empty uses the type's name (see GetEffectNames).
  plString m_sName;

  bool m_bBypass = false;

  /// \brief Compressor and ducker: the bus whose level drives the effect. Empty keys on the effect's own input.
  plString m_sSidechainBus;

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

/// \brief One overridden property inside a snapshot. Sends, effects and parameters are addressed by name, so the
/// override keeps its target when sends or effects are reordered.
///
/// Version 1 addressed them by index; a graph patch converts those overrides to names.
class plAudioMixerSnapshotOverride : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerSnapshotOverride, plReflectedClass);

public:
  plString m_sBus;
  plEnum<plAudioSnapshotTarget> m_eTarget;
  plString m_sSend;      ///< SendLevel: the aux bus the send goes to
  plString m_sEffect;    ///< EffectParam: the effect's name on the bus (see plAudioMixerEffect::m_sName)
  plString m_sParameter; ///< EffectParam: the parameter's registry name, e.g. "Threshold"
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
  /// name references to indices. Shared by the asset transform and the mixer console.
  ///
  /// Every problem (an unknown name, then the checks of plAudioMixerDesc::Validate) is reported; references that do
  /// not resolve are left out of the desc. out_pIssues receives each problem with the buses it concerns.
  static plStatus BuildRuntimeDesc(const plAudioMixerAssetProperties& props, plAudioMixerDesc& out_desc, plDynamicArray<plAudioMixerIssue>* out_pIssues = nullptr);

  /// \brief The names snapshot overrides address the effects of a bus by: the authored name, or the type's name, with
  /// " 2", " 3" ... for the second and later effects that share a name.
  static void GetEffectNames(plArrayPtr<const plUInt8> types, plArrayPtr<const plString> authoredNames, plDynamicArray<plString>& out_names);

  /// \brief GetEffectNames for the effects of a bus.
  static void GetEffectNames(const plAudioMixerBus& bus, plDynamicArray<plString>& out_names);

  plDynamicArray<plAudioMixerBus> m_Buses;
  plDynamicArray<plAudioMixerVca> m_Vcas;
  plDynamicArray<plAudioMixerSnapshot> m_Snapshots;
};

class plAudioMixerAssetDocument : public plSimpleAssetDocument<plAudioMixerAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerAssetDocument, plSimpleAssetDocument<plAudioMixerAssetProperties>);

public:
  plAudioMixerAssetDocument(plStringView sDocumentPath);

  /// \brief Writes the mixer as its resource file holds it (its path, the asset file header, then the desc), for the
  /// mixer console to apply in the engine process.
  plStatus WriteConsoleData(plStreamWriter& inout_stream, const plAudioMixerDesc& desc) const;

  /// \brief The last status the engine process sent for the mixer console.
  const plAudioMixerConsoleStatusMsgToEditor& GetConsoleStatus() const { return m_ConsoleStatus; }

  /// \brief Fires for every status the engine process sends.
  plEvent<const plAudioMixerConsoleStatusMsgToEditor&> m_ConsoleEvents;

protected:
  virtual void HandleEngineMessage(const plEditorEngineDocumentMsg* pMsg) override;
  virtual void UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const override;
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile,
    const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;

private:
  plAudioMixerConsoleStatusMsgToEditor m_ConsoleStatus;
};
