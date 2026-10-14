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

        // The clip, event and mixer windows and the Audio Profiler are engine-drawn: EditorUi and the retained UI runtime.
        PlasmaPackageSdk.AddImportLibrary(this, context, "plEditorUi");
        PlasmaPackageSdk.AddImportLibrary(this, context, "plGameUiPlugin");

        PlasmaPackageQt.ConfigureQtModule(this, context, "EditorPluginPlasmaAudioModule",
            "Source/EditorPluginPlasmaAudio", "Core", "Gui", "Widgets");
    }
}
