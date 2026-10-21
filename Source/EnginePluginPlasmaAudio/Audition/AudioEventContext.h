#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessDocumentContext.h>
#include <EnginePluginPlasmaAudio/Audition/AudioAuditionMessages.h>
#include <PlasmaAudioPlugin/Events/AudioEventResource.h>

/// \brief The engine process side of an Audio Event document: plays the window's audition through the PlasmaAudio
/// event manager and mixer (plAudioAudition).
///
/// The event the window sends is loaded as the resource "PlasmaAudio.Audition.<document guid>", so the saved event and
/// scenes that use it are not touched. The context has no view and no world.
class PL_ENGINEPLUGINPLASMAAUDIO_DLL plAudioEventContext : public plEngineProcessDocumentContext
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioEventContext, plEngineProcessDocumentContext);

public:
  plAudioEventContext();
  ~plAudioEventContext();

  virtual void HandleMessage(const plEditorEngineDocumentMsg* pMsg) override;

protected:
  virtual void OnInitialize() override;
  virtual void OnDeinitialize() override;

  virtual plEngineProcessViewContext* CreateViewContext() override { return nullptr; }
  virtual void DestroyViewContext(plEngineProcessViewContext* pContext) override {}

private:
  /// \brief Loads the event data of a Play request as the audition's event resource. Returns an error text on failure.
  plString LoadEvent(const plDataBuffer& data);

  void SendStatus(const plString& sError);

  plUInt64 m_uiKey = 0;
  plString m_sResourceId;
  plAudioEventResourceHandle m_hEvent;
};
