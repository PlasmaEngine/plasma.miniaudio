#pragma once

#include <AudioSystem/Mixer/AudioMixerResource.h>
#include <EditorEngineProcessFramework/EngineProcess/EngineProcessDocumentContext.h>
#include <EnginePluginPlasmaAudio/Mixer/AudioMixerConsoleMessages.h>

/// \brief The engine process side of an Audio Mixer document: the live mixer console.
///
/// Apply makes the window's mixer, saved or not, the content of the mixer's resource (its asset guid). When the engine
/// process plays that mixer, the mixer service applies the change in place; with the Preview source the context also
/// loads it. Mute, solo and snapshot requests go to the mixer service; the status reports the meters and states of
/// whatever mixer is loaded. Closing the document restores the resource from its file and clears mute, solo and the
/// test signal. The context has no view and no world.
class PL_ENGINEPLUGINPLASMAAUDIO_DLL plAudioMixerContext : public plEngineProcessDocumentContext
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioMixerContext, plEngineProcessDocumentContext);

public:
  plAudioMixerContext();
  ~plAudioMixerContext();

  virtual void HandleMessage(const plEditorEngineDocumentMsg* pMsg) override;

protected:
  virtual void OnInitialize() override;
  virtual void OnDeinitialize() override;

  virtual plEngineProcessViewContext* CreateViewContext() override { return nullptr; }
  virtual void DestroyViewContext(plEngineProcessViewContext* pContext) override {}

private:
  /// \brief Makes the data the content of the mixer resource. Returns an error text on failure.
  plString ApplyMixer(const plDataBuffer& data);

  /// \brief Sends a mixer control request to the audio system.
  void SendMixerControl(plUInt8 uiOp, plStringView sTarget, float fValue);

  /// \brief Loads the mixer when the Preview source wants it and the engine process plays another one.
  void LoadIfWanted(bool bIsThisMixer, bool bMonitorRefreshed);

  void SendStatus(const plString& sError);

  plString m_sResourceId;
  plAudioMixerResourceHandle m_hMixer;
  bool m_bCustomized = false;   ///< the resource holds the window's mixer instead of its file
  bool m_bWantLoad = false;     ///< the Preview source wants this mixer loaded
  bool m_bTouchedState = false; ///< mute, solo or a snapshot was set
  bool m_bTestSignal = false;
  plTime m_LastLoadRequest;
};
