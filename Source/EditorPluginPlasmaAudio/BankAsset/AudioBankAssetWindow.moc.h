#pragma once

#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAsset.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.moc.h>

class plEditorUiAudioBankDashboard;
class QTimer;

/// \brief Audio Bank editor: the bank dashboard (every bank of the project against a platform's budget) and the
/// bank's properties.
///
/// While open, the window asks the engine process for the loaded banks four times a second.
class plAudioBankAssetDocumentWindow : public plQtDocumentWindow
{
  Q_OBJECT

public:
  plAudioBankAssetDocumentWindow(plDocument* pDocument);

  virtual const char* GetGroupName() const { return "AudioBankAsset"; }

private:
  plEditorUiAudioBankDashboard* m_pDashboard = nullptr;
  QTimer* m_pPollTimer = nullptr;
};
