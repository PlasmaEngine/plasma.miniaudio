// #pl-version 1

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaAudioWorkspace : WorkspaceRules
{
    public PlasmaAudioWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("PlasmaAudioPlugin");
        TargetNames.Add("EditorPluginPlasmaAudio");
        TargetNames.Add("PlasmaAudioTest");
    }
}
