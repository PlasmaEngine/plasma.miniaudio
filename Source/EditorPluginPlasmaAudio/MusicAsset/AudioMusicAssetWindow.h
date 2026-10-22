#pragma once

#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicAsset.h>
#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicView.h>
#include <EditorUi/Runtime.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.h>

class plEditorUiDockManager;
struct plDocumentObjectPropertyEvent;
struct plDocumentObjectStructureEvent;

/// \brief Audio Music editor: the music view (where the music is built and auditioned) with Music Properties beside it.
///
/// The view plays the music in the engine process's audition (the same as an event's): Play starts it in its initial
/// state, a state button asks for that state (and starts the music in it when it does not play), a stinger button plays
/// the stinger. While it plays, the window polls the audition and hands the view the music's position and the messages
/// the game object would get. Every edit builds the music again (the view shows why it does not build); the segment and
/// state names fill the property grid's dropdowns. What the view selects, the property grid shows.
///
/// The panels: "AudioMusicViewDockWidget" (the music view) and "AudioMusicAssetDockWidget" (Music Properties).
class plAudioMusicAssetDocumentWindow : public plDocumentWindow
{
public:
  plAudioMusicAssetDocumentWindow(plDocument* pDocument);
  ~plAudioMusicAssetDocumentWindow();

  /// Music Properties on the right of the music view.
  static void ArrangePanels(plEditorUiDockManager& ref_panels);

private:
  void RequestRefresh();
  void RefreshMusic();

  void OnPropertyEvent(const plDocumentObjectPropertyEvent& e);
  void OnStructureEvent(const plDocumentObjectStructureEvent& e);
  void OnAuditionStatus(const plAudioAuditionStatusMsgToEditor& status);

  void Play(plUInt32 uiState);
  void Stop();
  void ChooseState(plUInt32 uiState);
  void PlayStinger(plUInt32 uiStinger);
  void Select(plEditorUiAudioMusicView::Selection selection, plUInt32 uiIndex);

  void SendCommand(plAudioAuditionCommand::Enum command);
  void SendParameter(plStringView sName, float fValue);

  plAudioMusicAssetDocument* m_pAssetDoc = nullptr;
  plEditorUiAudioMusicView* m_pView = nullptr;

  plAudioMusicDesc m_Music; ///< as last built, for names
  plUInt32 m_uiInitialState = 0;
  bool m_bAuditioning = false;
  bool m_bRefreshPending = false;
  bool m_bMixerNoteShown = false;

  plEventSubscriptionID m_PropertySubscription = 0;
  plEventSubscriptionID m_StructureSubscription = 0;
  plEventSubscriptionID m_AuditionSubscription = 0;

  plEditorUiTimerGroup m_Timers; ///< the audition poll and the queued refresh
};
