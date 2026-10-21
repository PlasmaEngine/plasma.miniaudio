using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class MiniAudioModule : ModuleRules
{
    public MiniAudioModule(BuildContext context) : base(context)
    {
        Type = ModuleType.ThirdParty;
        SourceDirectory = "ThirdParty/MiniAudio";

        PublicIncludePaths.Add("..");
        PublicDefinitions.Add("BUILDSYSTEM_ENABLE_MINIAUDIO_SUPPORT");
        PublicDefinitions.Add("MA_DLL");

        PrivateDefinitions.Add("MA_NO_ENCODING");
        PrivateDefinitions.Add("MA_NO_GENERATION");

        // Custom backends stay compiled in: platform plugins bring their own output through plAudioCustomBackend. WinMM
        // and DirectSound are loaded at run time like WASAPI, so offering them costs no link dependency.

        CStandard = CStandard.C17;
        TreatWarningsAsErrors = false;

        ConfigureForPlatform(TargetPlatform.Windows, () =>
        {
            PublicSystemLibraries.Add("ole32");
            PublicSystemLibraries.Add("avrt");
            PublicSystemLibraries.Add("user32");
        });

        // The engine's Android module configuration is not available outside the engine tree,
        // and this package only ships win64 binaries.
    }
}
