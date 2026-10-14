#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerAsset.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerEffect, 1, plRTTIDefaultAllocator<plAudioMixerEffect>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_MEMBER_PROPERTY("Type", plAudioEffectType, m_eType),
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

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMixerSnapshotOverride, 1, plRTTIDefaultAllocator<plAudioMixerSnapshotOverride>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Bus", m_sBus),
    PL_ENUM_MEMBER_PROPERTY("Target", plAudioSnapshotTarget, m_eTarget),
    PL_MEMBER_PROPERTY("IndexA", m_uiIndexA),
    PL_MEMBER_PROPERTY("IndexB", m_uiIndexB),
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
}

plAudioMixerAssetDocument::plAudioMixerAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plAudioMixerAssetProperties>(sDocumentPath, plAssetDocEngineConnection::None)
{
}

plStatus plAudioMixerAssetProperties::BuildRuntimeDesc(const plAudioMixerAssetProperties& properties, plAudioMixerDesc& desc)
{
  const plAudioMixerAssetProperties* pProp = &properties;

  desc = plAudioMixerDesc();

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

    for (plUInt32 i = 0; i < pProp->m_Buses.GetCount(); ++i)
    {
      const auto& bus = pProp->m_Buses[i];

      if (bus.m_sName.IsEmpty())
        return plStatus(plFmt("Bus {0} has no name.", i));

      auto& busDesc = desc.m_Buses.ExpandAndGetRef();
      busDesc.m_sName = bus.m_sName;
      busDesc.m_fVolumeDb = bus.m_fVolumeDb;
      busDesc.m_fPitch = bus.m_fPitch;
      busDesc.m_bIsAux = bus.m_bIsAux;
      busDesc.m_sEnvironmentName = bus.m_sEnvironmentName;

      if (i == 0)
      {
        busDesc.m_uiParent = 0xFF; // master
      }
      else if (bus.m_sParent.IsEmpty())
      {
        busDesc.m_uiParent = 0; // default to master
      }
      else
      {
        const plUInt32 uiParent = findBus(bus.m_sParent);
        if (uiParent == plInvalidIndex)
          return plStatus(plFmt("Bus '{0}' references unknown parent bus '{1}'.", bus.m_sName, bus.m_sParent));
        if (uiParent == i)
          return plStatus(plFmt("Bus '{0}' cannot be its own parent.", bus.m_sName));

        busDesc.m_uiParent = static_cast<plUInt8>(uiParent);
      }

      for (const auto& effect : bus.m_Effects)
      {
        auto& effectDesc = busDesc.m_Effects.ExpandAndGetRef();
        effectDesc.m_eType = static_cast<plAudioEffectType::Enum>(effect.m_eType.GetValue());
        effectDesc.InitDefaults();

        for (plUInt32 p = 0; p < plMath::Min<plUInt32>(effect.m_Params.GetCount(), plAudioEffectMaxParams); ++p)
        {
          effectDesc.m_fParams[p] = effect.m_Params[p];
        }
      }

      for (const auto& send : bus.m_Sends)
      {
        const plUInt32 uiTarget = findBus(send.m_sTargetBus);
        if (uiTarget == plInvalidIndex)
          return plStatus(plFmt("Bus '{0}' sends to unknown bus '{1}'.", bus.m_sName, send.m_sTargetBus));

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
          return plStatus(plFmt("VCA '{0}' references unknown bus '{1}'.", vca.m_sName, sBus));

        vcaDesc.m_Buses.PushBack(static_cast<plUInt8>(uiBus));
      }
    }

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
          return plStatus(plFmt("Snapshot '{0}' overrides unknown bus '{1}'.", snapshot.m_sName, ovr.m_sBus));

        auto& ovrDesc = snapshotDesc.m_Overrides.ExpandAndGetRef();
        ovrDesc.m_uiBus = static_cast<plUInt8>(uiBus);
        ovrDesc.m_eTarget = static_cast<plAudioSnapshotTarget::Enum>(ovr.m_eTarget.GetValue());
        ovrDesc.m_uiIndexA = ovr.m_uiIndexA;
        ovrDesc.m_uiIndexB = ovr.m_uiIndexB;
        ovrDesc.m_fValue = ovr.m_fValue;
      }
    }
  }

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
