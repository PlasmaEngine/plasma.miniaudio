#pragma once

#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAsset.h>
#include <EditorUi/Runtime.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.h>

class plEditorUiAudioBankDashboard;
class plEditorUiDockManager;

/// \brief Audio Bank editor: the bank dashboard (every bank of the project against a platform's budget) and the
/// bank's properties.
///
/// The panels: "AudioBankDashboardDockWidget" (the dashboard) and "AudioBankAssetDockWidget" (the bank's status and
/// properties). While open, the window asks the engine process for the loaded banks four times a second.
class plAudioBankAssetDocumentWindow : public plDocumentWindow
{
public:
  plAudioBankAssetDocumentWindow(plDocument* pDocument);
  ~plAudioBankAssetDocumentWindow();

  /// The default layout of the panels: Bank Properties on the right of Audio Banks.
  static void ArrangePanels(plEditorUiDockManager& ref_panels);

private:
  plEditorUiAudioBankDashboard* m_pDashboard = nullptr;
  plEditorUiTimerGroup m_Timers; ///< the poll of the loaded banks
};
