#pragma once

#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <Foundation/Reflection/Reflection.h>

/// \brief Marks a float property as an audio distance-attenuation exponent. The editor's
/// property grid shows the resulting rolloff curve under the value, between the sibling
/// MinDistance and MaxDistance, and dragging it vertically edits the exponent.
class plAudioRolloffCurveWidgetAttribute : public plTypeWidgetAttribute
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioRolloffCurveWidgetAttribute, plTypeWidgetAttribute);
};
