using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaAudioPluginModule : ModuleRules
{
    public PlasmaAudioPluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/PlasmaAudioPlugin", "PlasmaAudioPluginPCH.h",
            "BUILDSYSTEM_BUILDING_PLASMAAUDIOPLUGIN_LIB");

        PublicDependencies.Add("MiniAudioModule");

        // libopus is compiled into the plugin; no public header of the plugin includes it
        PrivateDependencies.Add("OpusModule");
    }
}
