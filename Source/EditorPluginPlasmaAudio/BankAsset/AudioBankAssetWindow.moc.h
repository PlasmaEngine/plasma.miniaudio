#pragma once

#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAsset.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.moc.h>

/// \brief Audio Bank editor: the bank's event and clip lists in a property grid.
class plAudioBankAssetDocumentWindow : public plQtDocumentWindow
{
  Q_OBJECT

public:
  plAudioBankAssetDocumentWindow(plDocument* pDocument);

  virtual const char* GetGroupName() const { return "AudioBankAsset"; }
};
