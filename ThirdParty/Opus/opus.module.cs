using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

// libopus 1.5.2 (BSD, see COPYING), float build. Only the sources of the upstream float build (celt_sources.mk,
// silk_sources.mk, opus_sources.mk: CELT_SOURCES, SILK_SOURCES, SILK_SOURCES_FLOAT, OPUS_SOURCES, OPUS_SOURCES_FLOAT and
// the x86 SSE/SSE2/SSE4.1/AVX2 lists) are in this directory, because every .c file below it is compiled. The x86 kernels
// compile to nothing unless OPUS_X86_MAY_HAVE_* is defined; on Windows x64 they are, with run-time CPU detection
// picking the kernels the CPU has. MSVC compiles the SSE4.1 and AVX2 intrinsics without per-file /arch flags.
//
// PlasmaAudio change to upstream: celt/x86/x86cpu.c caps the detected architecture with opus_plasma_max_arch
// (include/plasma_opus_arch.h), for the decode benchmark.
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
            // x64: run-time CPU detection over the SSE to AVX2 kernels
            PrivateIncludePaths.Add("celt/x86");
            PrivateIncludePaths.Add("silk/x86");
            PrivateDefinitions.Add("OPUS_HAVE_RTCD");
            PrivateDefinitions.Add("OPUS_X86_MAY_HAVE_SSE");
            PrivateDefinitions.Add("OPUS_X86_MAY_HAVE_SSE2");
            PrivateDefinitions.Add("OPUS_X86_MAY_HAVE_SSE4_1");
            PrivateDefinitions.Add("OPUS_X86_MAY_HAVE_AVX2");
            PublicDefinitions.Add("PL_OPUS_HAS_ARCH_CAP");

            PrivateDefinitions.Add("_CRT_SECURE_NO_WARNINGS");
            AdditionalCompilerFlags.Add("/wd4244");
            AdditionalCompilerFlags.Add("/wd4267");
            AdditionalCompilerFlags.Add("/wd4305");
            AdditionalCompilerFlags.Add("/wd4996");
        });
    }
}
