using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginPlasmaAudioTarget : TargetRules
{
    public EditorPluginPlasmaAudioTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plEditorPluginPlasmaAudio";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        TargetDependencies.Add("PlasmaAudioPlugin");
        ExtraModules.Add("EditorPluginPlasmaAudioModule");
        ExtraModules.Add("MiniAudioModule");
    }
}
