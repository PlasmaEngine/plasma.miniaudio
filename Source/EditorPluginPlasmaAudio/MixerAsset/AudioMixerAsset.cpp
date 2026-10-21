#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerAsset.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <Foundation/Serialization/AbstractObjectGraph.h>
#include <Foundation/Serialization/GraphPatch.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerEffect, 1, plRTTIDefaultAllocator<plAudioMixerEffect>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Type", plAudioEffectType, m_eType),
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_MEMBER_PROPERTY("Bypass", m_bBypass),
    PL_MEMBER_PROPERTY("SidechainBus", m_sSidechainBus),
    PL_ARRAY_MEMBER_PROPERTY("Params", m_Params),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerSend, 1, plRTTIDefaultAllocator<plAudioMixerSend>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("TargetBus", m_sTargetBus),
    PL_MEMBER_PROPERTY("Level", m_fLevelDb)->AddAttributes(new plClampValueAttribute(-60.0f, 12.0f), new plSuffixAttribute(" dB")),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerBus, 1, plRTTIDefaultAllocator<plAudioMixerBus>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName)->AddAttributes(new plGroupAttribute("Bus")),
    PL_MEMBER_PROPERTY("Parent", m_sParent),
    PL_MEMBER_PROPERTY("Volume", m_fVolumeDb)->AddAttributes(new plClampValueAttribute(-60.0f, 12.0f), new plSuffixAttribute(" dB")),
    PL_MEMBER_PROPERTY("Pitch", m_fPitch)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.01f, 10.0f)),
    PL_MEMBER_PROPERTY("IsAux", m_bIsAux)->AddAttributes(new plGroupAttribute("Aux Return")),
    PL_MEMBER_PROPERTY("Environment", m_sEnvironmentName),
    PL_ARRAY_MEMBER_PROPERTY("Effects", m_Effects)->AddAttributes(new plGroupAttribute("Inserts & Sends")),
    PL_ARRAY_MEMBER_PROPERTY("Sends", m_Sends),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerVca, 1, plRTTIDefaultAllocator<plAudioMixerVca>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_MEMBER_PROPERTY("Volume", m_fVolumeDb)->AddAttributes(new plClampValueAttribute(-60.0f, 12.0f), new plSuffixAttribute(" dB")),
    PL_ARRAY_MEMBER_PROPERTY("Buses", m_Buses),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerSnapshotOverride, 2, plRTTIDefaultAllocator<plAudioMixerSnapshotOverride>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Bus", m_sBus),
    PL_ENUM_MEMBER_PROPERTY("Target", plAudioSnapshotTarget, m_eTarget),
    PL_MEMBER_PROPERTY("Send", m_sSend),
    PL_MEMBER_PROPERTY("Effect", m_sEffect),
    PL_MEMBER_PROPERTY("Parameter", m_sParameter),
    PL_MEMBER_PROPERTY("Value", m_fValue),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerSnapshot, 1, plRTTIDefaultAllocator<plAudioMixerSnapshot>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_MEMBER_PROPERTY("FadeIn", m_fFadeIn)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 60.0f), new plSuffixAttribute(" sec")),
    PL_MEMBER_PROPERTY("FadeOut", m_fFadeOut)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 60.0f), new plSuffixAttribute(" sec")),
    PL_MEMBER_PROPERTY("Priority", m_uiPriority),
    PL_ARRAY_MEMBER_PROPERTY("Overrides", m_Overrides),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerAssetProperties, 1, plRTTIDefaultAllocator<plAudioMixerAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ARRAY_MEMBER_PROPERTY("Buses", m_Buses),
    PL_ARRAY_MEMBER_PROPERTY("VCAs", m_Vcas),
    PL_ARRAY_MEMBER_PROPERTY("Snapshots", m_Snapshots),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerAssetMetaInfo, 1, plRTTIDefaultAllocator<plAudioMixerAssetMetaInfo>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ARRAY_MEMBER_PROPERTY("Buses", m_Buses),
    PL_ARRAY_MEMBER_PROPERTY("OutputBuses", m_OutputBuses),
    PL_ARRAY_MEMBER_PROPERTY("Environments", m_Environments),
    PL_ARRAY_MEMBER_PROPERTY("Snapshots", m_Snapshots),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerAssetDocument, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  plString GetNodeString(const plAbstractObjectNode* pNode, plStringView sProperty)
  {
    const plAbstractObjectNode::Property* pProp = pNode->FindProperty(sProperty);
    return (pProp != nullptr && pProp->m_Value.IsA<plString>()) ? pProp->m_Value.Get<plString>() : plString();
  }

  /// The child nodes an array property of class objects refers to.
  void GetNodeArray(const plAbstractObjectGraph* pGraph, const plAbstractObjectNode* pNode, plStringView sProperty, plDynamicArray<const plAbstractObjectNode*>& out_nodes)
  {
    out_nodes.Clear();

    const plAbstractObjectNode::Property* pProp = pNode->FindProperty(sProperty);
    if (pProp == nullptr || !pProp->m_Value.IsA<plVariantArray>())
      return;

    for (const plVariant& value : pProp->m_Value.Get<plVariantArray>())
    {
      if (value.IsA<plUuid>())
      {
        if (const plAbstractObjectNode* pChild = pGraph->GetNode(value.Get<plUuid>()))
        {
          out_nodes.PushBack(pChild);
        }
      }
    }
  }
} // namespace

/// Version 1 addressed sends, effects and parameters of an override by index; version 2 addresses them by name.
class plAudioMixerSnapshotOverridePatch_1_2 : public plGraphPatch
{
public:
  plAudioMixerSnapshotOverridePatch_1_2()
    : plGraphPatch("plAudioMixerSnapshotOverride", 2)
  {
  }

  virtual void Patch(plGraphPatchContext& ref_context, plAbstractObjectGraph* pGraph, plAbstractObjectNode* pNode) const override
  {
    PL_IGNORE_UNUSED(ref_context);

    plUInt32 uiIndexA = 0;
    plUInt32 uiIndexB = 0;
    if (const auto* pProp = pNode->FindProperty("IndexA"))
      uiIndexA = pProp->m_Value.ConvertTo<plUInt32>();
    if (const auto* pProp = pNode->FindProperty("IndexB"))
      uiIndexB = pProp->m_Value.ConvertTo<plUInt32>();

    pNode->RemoveProperty("IndexA");
    pNode->RemoveProperty("IndexB");

    const plString sBus = GetNodeString(pNode, "Bus");
    const plString sTarget = GetNodeString(pNode, "Target");
    const bool bSend = sTarget.EndsWith("SendLevel");
    const bool bEffect = sTarget.EndsWith("EffectParam");
    if (!bSend && !bEffect)
      return;

    const plAbstractObjectNode* pBusNode = nullptr;
    for (auto it = pGraph->GetAllNodes().GetIterator(); it.IsValid(); ++it)
    {
      if (it.Value()->GetType() == "plAudioMixerBus" && GetNodeString(it.Value(), "Name").IsEqual_NoCase(sBus))
      {
        pBusNode = it.Value();
        break;
      }
    }

    if (pBusNode == nullptr)
      return;

    plDynamicArray<const plAbstractObjectNode*> children;

    if (bSend)
    {
      GetNodeArray(pGraph, pBusNode, "Sends", children);
      if (uiIndexA < children.GetCount())
      {
        pNode->AddProperty("Send", GetNodeString(children[uiIndexA], "TargetBus"));
      }
      return;
    }

    GetNodeArray(pGraph, pBusNode, "Effects", children);
    if (uiIndexA >= children.GetCount())
      return;

    plDynamicArray<plUInt8> types;
    plDynamicArray<plString> names;
    for (const plAbstractObjectNode* pEffect : children)
    {
      plInt64 iType = 0;
      plReflectionUtils::StringToEnumeration(plGetStaticRTTI<plAudioEffectType>(), GetNodeString(pEffect, "Type"), iType);
      types.PushBack(static_cast<plUInt8>(iType));
      names.PushBack(GetNodeString(pEffect, "Name"));
    }

    plDynamicArray<plString> effectNames;
    plAudioMixerAssetProperties::GetEffectNames(types, names, effectNames);
    pNode->AddProperty("Effect", effectNames[uiIndexA]);

    const plArrayPtr<const plAudioEffectParameterInfo> parameters = plAudioEffectRegistry::GetTypeInfo(static_cast<plAudioEffectType::Enum>(types[uiIndexA])).m_Parameters;
    if (uiIndexB < parameters.GetCount())
    {
      pNode->AddProperty("Parameter", plString(parameters[uiIndexB].m_szName));
    }
  }
};

plAudioMixerSnapshotOverridePatch_1_2 g_plAudioMixerSnapshotOverridePatch_1_2;

void plAudioMixerAssetDocument::UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const
{
  SUPER::UpdateAssetDocumentInfo(pInfo);

  // publish the names to the asset curator, so component dropdowns can list them
  // without this document being open
  plAudioMixerAssetMetaInfo* pMeta = PL_DEFAULT_NEW(plAudioMixerAssetMetaInfo);

  for (const auto& bus : GetProperties()->m_Buses)
  {
    if (!bus.m_sName.IsEmpty())
    {
      pMeta->m_Buses.PushBack(bus.m_sName);

      // aux buses are reached by sends and environment routing only — an event that output
      // straight into one would be permanently 100% wet, so keep them out of the event dropdown
      if (!bus.m_bIsAux)
        pMeta->m_OutputBuses.PushBack(bus.m_sName);
    }

    if (bus.m_bIsAux && !bus.m_sEnvironmentName.IsEmpty())
      pMeta->m_Environments.PushBack(bus.m_sEnvironmentName);
  }

  for (const auto& snapshot : GetProperties()->m_Snapshots)
  {
    if (!snapshot.m_sName.IsEmpty())
      pMeta->m_Snapshots.PushBack(snapshot.m_sName);
  }

  pInfo->m_MetaInfo.PushBack(pMeta);
}

void plAudioMixerAssetProperties::PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plAudioMixerAssetProperties>())
  {
    auto& props = *e.m_pPropertyStates;

    // the mixer is edited through the Mixer Console window (strips, routing tree, snapshot dock) —
    // showing the arrays in the grid duplicates the whole document into the Properties dock
    props["Buses"].m_Visibility = plPropertyUiState::Invisible;
    props["VCAs"].m_Visibility = plPropertyUiState::Invisible;
    props["Snapshots"].m_Visibility = plPropertyUiState::Invisible;
  }
  else if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plAudioMixerEffect>())
  {
    auto& props = *e.m_pPropertyStates;

    // the insert editor shows the parameters by name, with units and ranges
    props["Params"].m_Visibility = plPropertyUiState::Invisible;

    const auto eType = static_cast<plAudioEffectType::Enum>(e.m_pObject->GetTypeAccessor().GetValue("Type").ConvertTo<plInt64>());
    props["SidechainBus"].m_Visibility = plAudioEffectRegistry::GetTypeInfo(eType).m_bUsesSidechain ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  }
  else if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plAudioMixerSnapshotOverride>())
  {
    auto& props = *e.m_pPropertyStates;

    const plInt64 iTarget = e.m_pObject->GetTypeAccessor().GetValue("Target").ConvertTo<plInt64>();
    props["Send"].m_Visibility = iTarget == plAudioSnapshotTarget::SendLevel ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["Effect"].m_Visibility = iTarget == plAudioSnapshotTarget::EffectParam ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["Parameter"].m_Visibility = iTarget == plAudioSnapshotTarget::EffectParam ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  }
}

plAudioMixerAssetDocument::plAudioMixerAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plAudioMixerAssetProperties>(sDocumentPath, plAssetDocEngineConnection::Simple)
{
}

void plAudioMixerAssetDocument::HandleEngineMessage(const plEditorEngineDocumentMsg* pMsg)
{
  if (const auto* pStatus = plDynamicCast<const plAudioMixerConsoleStatusMsgToEditor*>(pMsg))
  {
    m_ConsoleStatus = *pStatus;
    m_ConsoleEvents.Broadcast(m_ConsoleStatus);
    return;
  }

  plSimpleAssetDocument<plAudioMixerAssetProperties>::HandleEngineMessage(pMsg);
}

plStatus plAudioMixerAssetDocument::WriteConsoleData(plStreamWriter& inout_stream, const plAudioMixerDesc& desc) const
{
  // what a mixer resource file holds: its path, the asset file header, then the mixer
  plStringBuilder sPath = GetDocumentPath();
  sPath.ChangeFileExtension("plAudioMixer");
  inout_stream << sPath;

  plAssetFileHeader header;
  header.SetFileHashAndVersion(plAssetCurator::GetSingleton()->GetAssetTransformHash(GetGuid()), GetAssetTypeVersion());
  PL_SUCCEED_OR_RETURN(header.Write(inout_stream));

  PL_SUCCEED_OR_RETURN(desc.Save(inout_stream));
  return plStatus(PL_SUCCESS);
}

void plAudioMixerAssetProperties::GetEffectNames(plArrayPtr<const plUInt8> types, plArrayPtr<const plString> authoredNames, plDynamicArray<plString>& out_names)
{
  out_names.Clear();

  auto TypeName = [&](plUInt32 i)
  {
    return plStringView(plAudioEffectRegistry::GetTypeInfo(static_cast<plAudioEffectType::Enum>(types[i])).m_szName);
  };

  for (plUInt32 i = 0; i < types.GetCount(); ++i)
  {
    plStringBuilder sBase = (i < authoredNames.GetCount() && !authoredNames[i].IsEmpty()) ? authoredNames[i].GetView() : TypeName(i);

    plUInt32 uiSame = 1;
    for (plUInt32 j = 0; j < i; ++j)
    {
      const plStringView sOther = (j < authoredNames.GetCount() && !authoredNames[j].IsEmpty()) ? authoredNames[j].GetView() : TypeName(j);
      if (sOther.IsEqual_NoCase(sBase))
        ++uiSame;
    }

    if (uiSame > 1)
    {
      sBase.AppendFormat(" {}", uiSame);
    }

    out_names.PushBack(sBase);
  }
}

void plAudioMixerAssetProperties::GetEffectNames(const plAudioMixerBus& bus, plDynamicArray<plString>& out_names)
{
  plHybridArray<plUInt8, 4> types;
  plHybridArray<plString, 4> names;

  for (const plAudioMixerEffect& effect : bus.m_Effects)
  {
    types.PushBack(static_cast<plUInt8>(effect.m_eType.GetValue()));
    names.PushBack(effect.m_sName);
  }

  GetEffectNames(types, names, out_names);
}

plStatus plAudioMixerAssetProperties::BuildRuntimeDesc(const plAudioMixerAssetProperties& properties, plAudioMixerDesc& desc, plDynamicArray<plAudioMixerIssue>* out_pIssues)
{
  const plAudioMixerAssetProperties* pProp = &properties;

  desc = plAudioMixerDesc();

  plStringBuilder sErrors;

  auto AddIssue = [&](plUInt32 uiBus, plUInt32 uiOtherBus, const plFormatString& error)
  {
    plStringBuilder sTmp;
    const plStringView sMessage = error.GetText(sTmp);

    if (!sErrors.IsEmpty())
    {
      sErrors.Append("\n");
    }
    sErrors.Append(sMessage);

    if (out_pIssues != nullptr)
    {
      plAudioMixerIssue& issue = out_pIssues->ExpandAndGetRef();
      issue.m_sMessage = sMessage;
      issue.m_uiBus = uiBus;
      issue.m_uiOtherBus = uiOtherBus;
    }
  };

  if (pProp->m_Buses.IsEmpty())
  {
    desc.MakeDefault();
  }
  else
  {
    // resolve bus names to indices; the first bus is the master bus
    auto findBus = [&](plStringView sName) -> plUInt32
    {
      for (plUInt32 i = 0; i < pProp->m_Buses.GetCount(); ++i)
      {
        if (pProp->m_Buses[i].m_sName.IsEqual_NoCase(sName))
          return i;
      }
      return plInvalidIndex;
    };

    if (pProp->m_Buses.GetCount() > 250)
      return plStatus("A mixer supports at most 250 buses.");

    // references that do not resolve are reported and left out, so the rest of the mixer is still checked
    for (plUInt32 i = 0; i < pProp->m_Buses.GetCount(); ++i)
    {
      const auto& bus = pProp->m_Buses[i];

      auto& busDesc = desc.m_Buses.ExpandAndGetRef();
      busDesc.m_sName = bus.m_sName;
      busDesc.m_fVolumeDb = bus.m_fVolumeDb;
      busDesc.m_fPitch = bus.m_fPitch;
      busDesc.m_bIsAux = bus.m_bIsAux;
      busDesc.m_sEnvironmentName = bus.m_sEnvironmentName;

      if (i == 0)
      {
        busDesc.m_uiParent = plAudioNoBus; // master
      }
      else if (bus.m_sParent.IsEmpty())
      {
        busDesc.m_uiParent = 0; // default to master
      }
      else
      {
        const plUInt32 uiParent = findBus(bus.m_sParent);
        if (uiParent == plInvalidIndex)
        {
          AddIssue(i, plInvalidIndex, plFmt("Bus '{0}' references unknown parent bus '{1}'.", bus.m_sName, bus.m_sParent));
          busDesc.m_uiParent = 0;
        }
        else
        {
          busDesc.m_uiParent = static_cast<plUInt8>(uiParent);
        }
      }

      for (const auto& effect : bus.m_Effects)
      {
        auto& effectDesc = busDesc.m_Effects.ExpandAndGetRef();
        effectDesc.m_eType = static_cast<plAudioEffectType::Enum>(effect.m_eType.GetValue());
        effectDesc.InitDefaults();
        effectDesc.m_bBypass = effect.m_bBypass;

        for (plUInt32 p = 0; p < plMath::Min<plUInt32>(effect.m_Params.GetCount(), plAudioEffectMaxParams); ++p)
        {
          effectDesc.m_fParams[p] = effect.m_Params[p];
        }

        if (!effect.m_sSidechainBus.IsEmpty() && plAudioEffectRegistry::GetTypeInfo(effectDesc.m_eType).m_bUsesSidechain)
        {
          const plUInt32 uiKey = findBus(effect.m_sSidechainBus);
          if (uiKey == plInvalidIndex)
          {
            AddIssue(i, plInvalidIndex, plFmt("The {0} on '{1}' is keyed by unknown bus '{2}'.", plAudioEffectRegistry::GetTypeInfo(effectDesc.m_eType).m_szName, bus.m_sName, effect.m_sSidechainBus));
          }
          else
          {
            effectDesc.m_uiSidechainBus = static_cast<plUInt8>(uiKey);
          }
        }
      }

      for (const auto& send : bus.m_Sends)
      {
        const plUInt32 uiTarget = findBus(send.m_sTargetBus);
        if (uiTarget == plInvalidIndex)
        {
          AddIssue(i, plInvalidIndex, plFmt("Bus '{0}' sends to unknown bus '{1}'.", bus.m_sName, send.m_sTargetBus));
          continue;
        }

        auto& sendDesc = busDesc.m_Sends.ExpandAndGetRef();
        sendDesc.m_uiTargetBus = static_cast<plUInt8>(uiTarget);
        sendDesc.m_fLevelDb = send.m_fLevelDb;
      }
    }

    for (const auto& vca : pProp->m_Vcas)
    {
      auto& vcaDesc = desc.m_Vcas.ExpandAndGetRef();
      vcaDesc.m_sName = vca.m_sName;
      vcaDesc.m_fVolumeDb = vca.m_fVolumeDb;

      for (const auto& sBus : vca.m_Buses)
      {
        const plUInt32 uiBus = findBus(sBus);
        if (uiBus == plInvalidIndex)
        {
          AddIssue(plInvalidIndex, plInvalidIndex, plFmt("VCA '{0}' references unknown bus '{1}'.", vca.m_sName, sBus));
          continue;
        }

        vcaDesc.m_Buses.PushBack(static_cast<plUInt8>(uiBus));
      }
    }

    plDynamicArray<plString> effectNames;

    for (const auto& snapshot : pProp->m_Snapshots)
    {
      auto& snapshotDesc = desc.m_Snapshots.ExpandAndGetRef();
      snapshotDesc.m_sName = snapshot.m_sName;
      snapshotDesc.m_FadeIn = plTime::MakeFromSeconds(snapshot.m_fFadeIn);
      snapshotDesc.m_FadeOut = plTime::MakeFromSeconds(snapshot.m_fFadeOut);
      snapshotDesc.m_uiPriority = snapshot.m_uiPriority;

      for (const auto& ovr : snapshot.m_Overrides)
      {
        const plUInt32 uiBus = findBus(ovr.m_sBus);
        if (uiBus == plInvalidIndex)
        {
          AddIssue(plInvalidIndex, plInvalidIndex, plFmt("Snapshot '{0}' overrides unknown bus '{1}'.", snapshot.m_sName, ovr.m_sBus));
          continue;
        }

        const plAudioMixerBus& bus = pProp->m_Buses[uiBus];
        const auto eTarget = static_cast<plAudioSnapshotTarget::Enum>(ovr.m_eTarget.GetValue());
        plUInt32 uiIndexA = 0;
        plUInt32 uiIndexB = 0;

        if (eTarget == plAudioSnapshotTarget::SendLevel)
        {
          uiIndexA = plInvalidIndex;
          for (plUInt32 s = 0; s < bus.m_Sends.GetCount(); ++s)
          {
            if (bus.m_Sends[s].m_sTargetBus.IsEqual_NoCase(ovr.m_sSend))
              uiIndexA = s;
          }

          if (uiIndexA == plInvalidIndex)
          {
            AddIssue(uiBus, plInvalidIndex, plFmt("Snapshot '{0}' overrides the send of '{1}' to '{2}', which it does not have.", snapshot.m_sName, bus.m_sName, ovr.m_sSend));
            continue;
          }
        }
        else if (eTarget == plAudioSnapshotTarget::EffectParam)
        {
          GetEffectNames(bus, effectNames);

          uiIndexA = plInvalidIndex;
          for (plUInt32 e = 0; e < effectNames.GetCount(); ++e)
          {
            if (effectNames[e].IsEqual_NoCase(ovr.m_sEffect))
              uiIndexA = e;
          }

          if (uiIndexA == plInvalidIndex)
          {
            AddIssue(uiBus, plInvalidIndex, plFmt("Snapshot '{0}' overrides effect '{1}' on '{2}', which it does not have.", snapshot.m_sName, ovr.m_sEffect, bus.m_sName));
            continue;
          }

          const auto eType = static_cast<plAudioEffectType::Enum>(bus.m_Effects[uiIndexA].m_eType.GetValue());
          uiIndexB = plAudioEffectRegistry::FindParameter(eType, ovr.m_sParameter);
          if (uiIndexB == plInvalidIndex)
          {
            AddIssue(uiBus, plInvalidIndex, plFmt("Snapshot '{0}' overrides '{1}' of '{2}' on '{3}'; a {4} has no such parameter.", snapshot.m_sName, ovr.m_sParameter, ovr.m_sEffect, bus.m_sName, plAudioEffectRegistry::GetTypeInfo(eType).m_szName));
            continue;
          }
        }

        auto& ovrDesc = snapshotDesc.m_Overrides.ExpandAndGetRef();
        ovrDesc.m_uiBus = static_cast<plUInt8>(uiBus);
        ovrDesc.m_eTarget = eTarget;
        ovrDesc.m_uiIndexA = static_cast<plUInt8>(uiIndexA);
        ovrDesc.m_uiIndexB = static_cast<plUInt8>(uiIndexB);
        ovrDesc.m_fValue = ovr.m_fValue;
      }
    }
  }

  // the routing as a whole (cycles, sidechains, sends to non-aux buses, duplicate names) is checked by the runtime
  // desc, the same check the runtime runs when it builds the mixer
  {
    plStringBuilder sValidation;
    desc.Validate(sValidation, out_pIssues).IgnoreResult();

    if (!sValidation.IsEmpty())
    {
      if (!sErrors.IsEmpty())
      {
        sErrors.Append("\n");
      }
      sErrors.Append(sValidation);
    }
  }

  if (!sErrors.IsEmpty())
    return plStatus(sErrors.GetView());

  return plStatus(PL_SUCCESS);
}

plTransformStatus plAudioMixerAssetDocument::InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  plAudioMixerDesc desc;

  const plStatus status = plAudioMixerAssetProperties::BuildRuntimeDesc(*GetProperties(), desc);
  if (status.Failed())
  {
    return plTransformStatus(status.m_sMessage.GetView());
  }

  PL_SUCCEED_OR_RETURN(desc.Save(stream));

  return plStatus(PL_SUCCESS);
}
