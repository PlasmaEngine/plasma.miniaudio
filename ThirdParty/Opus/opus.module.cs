using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

// libopus 1.5.2 (BSD, see COPYING), float build without run-time CPU detection or SIMD kernels.
// Only the sources of the upstream float build (celt_sources.mk, silk_sources.mk, opus_sources.mk: CELT_SOURCES,
// SILK_SOURCES, SILK_SOURCES_FLOAT, OPUS_SOURCES, OPUS_SOURCES_FLOAT) are in this directory, because every .c file
// below it is compiled.
public class OpusModule : ModuleRules
{
    public OpusModule(BuildContext context) : base(context)
    {
        Type = ModuleType.ThirdParty;
        SourceDirectory = "ThirdParty/Opus";

        // libopus has no export annotations unless DLL_EXPORT is defined, so it is compiled into the consuming binary
        // (plPlasmaAudioPlugin) instead of being imported from one. Consumers include <opus.h>.
        ExportsSymbols = false;
        PublicIncludePaths.Add("include");

        PrivateIncludePaths.Add(".");
        PrivateIncludePaths.Add("celt");
        PrivateIncludePaths.Add("silk");
        PrivateIncludePaths.Add("silk/float");
        PrivateIncludePaths.Add("src");

        PrivateDefinitions.Add("OPUS_BUILD");
        PrivateDefinitions.Add("USE_ALLOCA");

        // many translation units define file-local helpers with the same names
        UseUnityBuild = false;

        CStandard = CStandard.C17;
        TreatWarningsAsErrors = false;

        ConfigureForPlatform(TargetPlatform.Windows, () =>
        {
            PrivateDefinitions.Add("_CRT_SECURE_NO_WARNINGS");
            AdditionalCompilerFlags.Add("/wd4244");
            AdditionalCompilerFlags.Add("/wd4267");
            AdditionalCompilerFlags.Add("/wd4305");
            AdditionalCompilerFlags.Add("/wd4996");
        });
    }
}
