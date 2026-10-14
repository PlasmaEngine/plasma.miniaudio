using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaAudioTestTarget : TargetRules
{
    public PlasmaAudioTestTarget(BuildContext context) : base(context)
    {
        Type = TargetType.Executable;
        OutputName = "PlasmaAudioTest";
        // Kept out of the package binary directory: that directory is what a local install copies into a
        // project and what the publisher draws the platform components from. The test resolves the plugin
        // and engine DLLs through PATH.
        OutputDirectory = Path.Combine(context.ProjectRoot, "Bin", "Tests", $"{context.Platform}_x64_{context.Configuration}");
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        GenerateDebugInfo = true;

        // miniaudio is compiled into (and exported from) plPlasmaAudioPlugin, so the test binds to that copy
        // instead of building its own: the offline ma_engine it creates is driven by the plugin's code.
        TargetDependencies.Add("PlasmaAudioPlugin");
        ExtraModules.Add("PlasmaAudioTestModule");
    }
}
