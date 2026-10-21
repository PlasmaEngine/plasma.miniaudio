#pragma once

#include <EditorFramework/EngineShell/DocumentContent.h>
#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <EditorUi/Content.h>

class plEditorUiDocumentFrame;
class plQtEngineUiContentWidget;
class QWidget;

/// \brief Part of a panel of an audio window in the engine shell: contents placed one after the other, top to bottom
/// or left to right. A part with an extent keeps it; the others share the rest by weight.
///
/// The contents are the window's views, lent by the hidden Qt window (plEditorUiDocumentFrame::AddLentChild), and
/// stacks or shell contents of its own (AddOwnedPart). A lent view that goes back leaves its place empty.
class plEditorUiAudioShellStack : public plEditorUiContent
{
public:
  explicit plEditorUiAudioShellStack(bool bHorizontal = false);

  /// fExtent > 0: logical units along the stack; else fWeight shares what the fixed parts leave.
  void AddPart(plEditorUiContent* pPart, float fExtent, float fWeight = 1.0f);

  /// A content the stack owns and places.
  plEditorUiContent* AddOwnedPart(plUniquePtr<plEditorUiContent>&& pPart, float fExtent, float fWeight = 1.0f);

  /// Lends the window's view of this object name to the stack. Null when the window has no such view.
  plEditorUiContent* AddLentPart(plEditorUiDocumentFrame& ref_frame, QWidget* pWindow, const char* szName, float fExtent, float fWeight = 1.0f);

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

/// \brief The audio documents' frames in the engine shell: the Qt windows' views, lent and laid out as the windows lay
/// them out (the shell shows only the first engine content of a dock otherwise).
class plAudioShellPanels : public plEditorUiDocumentContentProvider
{
public:
  bool Matches(const plDocument* pDocument) const override;
  void CreatePanels(plEditorUiDocumentFrame& ref_frame) const override;

  static void Register();
  static void Unregister();

  /// The window's engine view of this object name, or nullptr.
  static plQtEngineUiContentWidget* FindView(QWidget* pWindow, const char* szName);

private:
  static void CreateClipPanels(plEditorUiDocumentFrame& ref_frame);
  static void CreateEventPanels(plEditorUiDocumentFrame& ref_frame);
  static void CreateMixerPanels(plEditorUiDocumentFrame& ref_frame);
  static void CreateAttenuationPanels(plEditorUiDocumentFrame& ref_frame);
};
