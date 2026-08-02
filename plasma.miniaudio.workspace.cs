// #pl-version 1

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaMiniAudioWorkspace : WorkspaceRules
{
    public PlasmaMiniAudioWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("MiniAudioPlugin");
        TargetNames.Add("EditorPluginMiniAudio");
    }
}
