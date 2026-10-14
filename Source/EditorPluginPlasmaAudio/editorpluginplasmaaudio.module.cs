using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginPlasmaAudioModule : ModuleRules
{
    public EditorPluginPlasmaAudioModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EditorPluginPlasmaAudio", "EditorPluginPlasmaAudioPCH.h",
            "BUILDSYSTEM_BUILDING_EDITORPLUGINPLASMAAUDIO_LIB");

        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);
        PublicDependencies.Add("MiniAudioModule");
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/PlasmaAudioPlugin", "plPlasmaAudioPlugin");

        PlasmaPackageQt.ConfigureQtModule(this, context, "EditorPluginPlasmaAudioModule",
            "Source/EditorPluginPlasmaAudio", "Core", "Gui", "Widgets");
    }
}
