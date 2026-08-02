using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginMiniAudioModule : ModuleRules
{
    public EditorPluginMiniAudioModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EditorPluginMiniAudio", "EditorPluginMiniAudioPCH.h",
            "BUILDSYSTEM_BUILDING_EDITORPLUGINMINIAUDIO_LIB");

        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);
        PublicDependencies.Add("MiniAudioModule");
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/MiniAudioPlugin", "plMiniAudioPlugin");

        PlasmaPackageQt.ConfigureQtModule(this, context, "EditorPluginMiniAudioModule",
            "Source/EditorPluginMiniAudio", "Core", "Gui", "Widgets");
    }
}
