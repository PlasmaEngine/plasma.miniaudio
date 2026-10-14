using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaAudioTestModule : ModuleRules
{
    public PlasmaAudioTestModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/PlasmaAudioTest", "PlasmaAudioTestPCH.h",
            "BUILDSYSTEM_BUILDING_PLASMAAUDIOTEST");

        // test framework headers and import library from the SDK
        var unitTestsRoot = Path.Combine(PlasmaPackageSdk.Root, "Code", "UnitTests");
        PublicIncludePaths.Add(unitTestsRoot);
        PublicIncludePaths.Add(Path.Combine(unitTestsRoot, "TestFramework"));
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Intermediate", "PlasmaBuild", "Generated", "Code", "UnitTests"));
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Intermediate", "PlasmaBuild", "Generated", "Code", "UnitTests", "TestFramework"));

        if (context.Platform == TargetPlatform.Windows)
        {
            PublicIncludePaths.Add(Path.Combine(unitTestsRoot, "TestFramework", "Platform", "Win"));
        }

        PlasmaPackageSdk.AddImportLibrary(this, context, "plTestFramework");

        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/PlasmaAudioPlugin", "plPlasmaAudioPlugin");

        // miniaudio headers only: the implementation is imported from plPlasmaAudioPlugin (built with MA_DLL)
        PublicIncludePaths.Add(Path.Combine(context.ProjectRoot, "ThirdParty"));
        PublicDefinitions.Add("BUILDSYSTEM_ENABLE_MINIAUDIO_SUPPORT");
        PublicDefinitions.Add("MA_DLL");
    }
}
