#pragma once

#include <AudioSystem/Core/AudioSystemData.h>
#include <EditorEngineProcessFramework/EngineProcess/EngineProcessDocumentContext.h>
#include <EnginePluginPlasmaAudio/Banks/AudioBankMessages.h>

/// \brief The engine process side of an Audio Bank document: loads and unloads banks for the bank window and reports
/// the banks the engine process has loaded, their load progress and the memory of the loaded audio content.
///
/// Banks are loaded through audio system requests, as plAudioBankLoaderComponent loads them. Closing the document
/// unloads the banks it loaded. The context has no view and no world.
class PL_ENGINEPLUGINPLASMAAUDIO_DLL plAudioBankContext : public plEngineProcessDocumentContext
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioBankContext, plEngineProcessDocumentContext);

public:
  plAudioBankContext();
  ~plAudioBankContext();

  virtual void HandleMessage(const plEditorEngineDocumentMsg* pMsg) override;

protected:
  virtual void OnInitialize() override;
  virtual void OnDeinitialize() override;

  virtual plEngineProcessViewContext* CreateViewContext() override { return nullptr; }
  virtual void DestroyViewContext(plEngineProcessViewContext* pContext) override {}

private:
  /// \brief Sends a load or unload request for the bank to the audio system.
  void SendBankRequest(plStringView sBank, bool bLoad);

  void SendStatus(const plString& sError);

  plDynamicArray<plString> m_LoadedBanks; ///< the banks this context loaded
};
