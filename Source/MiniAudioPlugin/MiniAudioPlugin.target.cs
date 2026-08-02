using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class MiniAudioPluginTarget : TargetRules
{
    public MiniAudioPluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plMiniAudioPlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        ExtraModules.Add("MiniAudioPluginModule");
        ExtraModules.Add("MiniAudioModule");
    }
}
