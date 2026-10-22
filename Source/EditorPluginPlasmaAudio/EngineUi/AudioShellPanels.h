#pragma once

#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <EditorUi/Content.h>

/// \brief Part of a panel of an audio window in the engine shell: contents placed one after the other, top to bottom
/// or left to right. A part with an extent keeps it; the others share the rest by weight.
///
/// The window makes the stack a panel (plDocumentWindow::AddPanel) and fills it with the views it drives
/// (AddOwnedPart), or with nested stacks.
class plEditorUiAudioShellStack : public plEditorUiContent
{
public:
  explicit plEditorUiAudioShellStack(bool bHorizontal = false);

  /// fExtent > 0: logical units along the stack; else fWeight shares what the fixed parts leave.
  void AddPart(plEditorUiContent* pPart, float fExtent, float fWeight = 1.0f);

  /// A content the stack owns and places.
  plEditorUiContent* AddOwnedPart(plUniquePtr<plEditorUiContent>&& pPart, float fExtent, float fWeight = 1.0f);

  /// The part's extent follows its content (a deck that grows with its playlist), asked every frame with the stack's
  /// length.
  void SetExtentSource(const plEditorUiContent* pPart, plDelegate<float(float)> extent);

  /// A line between the parts, in the theme's line colour.
  void SetSeparators(bool bSeparators) { m_bSeparators = bSeparators; }

protected:
  void OnUiUpdate() override;

private:
  struct Part
  {
    plEditorUiContent* m_pContent = nullptr;
    float m_fExtent = 0.0f;
    float m_fWeight = 1.0f;
    plDelegate<float(float)> m_Extent;
  };

  bool IsChild(const plEditorUiContent* pContent) const;
  void PlaceParts();

  plHybridArray<Part, 6> m_Parts;
  bool m_bHorizontal = false;
  bool m_bSeparators = false;
  bool m_bBuilt = false;
};
