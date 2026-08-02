using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class MiniAudioPluginModule : ModuleRules
{
    public MiniAudioPluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/MiniAudioPlugin", "MiniAudioPluginPCH.h",
            "BUILDSYSTEM_BUILDING_MINIAUDIOPLUGIN_LIB");

        PublicDependencies.Add("MiniAudioModule");
    }
}
