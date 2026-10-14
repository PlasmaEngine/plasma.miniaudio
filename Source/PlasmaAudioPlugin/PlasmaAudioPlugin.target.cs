using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaAudioPluginTarget : TargetRules
{
    public PlasmaAudioPluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plPlasmaAudioPlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        ExtraModules.Add("PlasmaAudioPluginModule");
        ExtraModules.Add("MiniAudioModule");
    }
}
