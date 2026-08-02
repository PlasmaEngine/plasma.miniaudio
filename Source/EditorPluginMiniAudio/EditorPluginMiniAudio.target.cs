using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginMiniAudioTarget : TargetRules
{
    public EditorPluginMiniAudioTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plEditorPluginMiniAudio";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        TargetDependencies.Add("MiniAudioPlugin");
        ExtraModules.Add("EditorPluginMiniAudioModule");
        ExtraModules.Add("MiniAudioModule");
    }
}
