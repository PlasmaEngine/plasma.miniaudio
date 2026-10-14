#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/Preview/AudioEnumRegistry.h>

#include <GuiFoundation/UIServices/DynamicStringEnum.h>

namespace
{
  /// \brief Adds a value if it is not present yet. Uses GetDynamicEnum (not CreateDynamicEnum) so
  /// the editor's on-demand loader keeps the enum user-editable and backed by its project file.
  void AddValue(plStringView sEnumName, plStringView sValue)
  {
    if (sValue.IsEmpty())
      return;

    auto& dynEnum = plDynamicStringEnum::GetDynamicEnum(sEnumName);

    if (dynEnum.IsValueValid(sValue))
      return;

    dynEnum.AddValidValue(sValue, true);
  }
} // namespace

void plAudioEnumRegistry::RegisterMixerNames(const plAudioMixerDesc& desc)
{
  for (const auto& bus : desc.m_Buses)
  {
    AddValue("AudioSystem.Bus", bus.m_sName);

    // only aux buses can be fed by a reverb zone
    if (bus.m_bIsAux)
    {
      AddValue("AudioSystem.Environment", bus.m_sEnvironmentName);
    }
    else
    {
      // ... and conversely, only non-aux buses are valid event output targets
      AddValue("AudioSystem.OutputBus", bus.m_sName);
    }
  }

  for (const auto& snapshot : desc.m_Snapshots)
  {
    AddValue("AudioSystem.Snapshot", snapshot.m_sName);
  }
}

void plAudioEnumRegistry::RegisterBusName(plStringView sName)
{
  AddValue("AudioSystem.Bus", sName);
}

void plAudioEnumRegistry::RegisterOutputBusName(plStringView sName)
{
  AddValue("AudioSystem.OutputBus", sName);
}

void plAudioEnumRegistry::RegisterEnvironmentName(plStringView sName)
{
  AddValue("AudioSystem.Environment", sName);
}

void plAudioEnumRegistry::RegisterSnapshotName(plStringView sName)
{
  AddValue("AudioSystem.Snapshot", sName);
}

void plAudioEnumRegistry::RegisterRtpcName(plStringView sName)
{
  AddValue("AudioSystem.Rtpc", sName);
}

void plAudioEnumRegistry::RegisterSwitchStateName(plStringView sName)
{
  AddValue("AudioSystem.SwitchState", sName);
}
