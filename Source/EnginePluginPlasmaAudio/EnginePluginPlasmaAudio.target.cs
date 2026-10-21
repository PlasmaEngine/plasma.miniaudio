using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EnginePluginPlasmaAudioTarget : TargetRules
{
    public EnginePluginPlasmaAudioTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plEnginePluginPlasmaAudio";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        TargetDependencies.Add("PlasmaAudioPlugin");
        ExtraModules.Add("EnginePluginPlasmaAudioModule");
    }
}
