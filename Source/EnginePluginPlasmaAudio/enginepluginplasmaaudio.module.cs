using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EnginePluginPlasmaAudioModule : ModuleRules
{
    public EnginePluginPlasmaAudioModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EnginePluginPlasmaAudio", "EnginePluginPlasmaAudioPCH.h",
            "BUILDSYSTEM_BUILDING_ENGINEPLUGINPLASMAAUDIO_LIB");

        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/PlasmaAudioPlugin", "plPlasmaAudioPlugin");
    }
}
