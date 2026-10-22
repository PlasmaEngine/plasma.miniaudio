#pragma once

#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAsset.h>
#include <EditorUi/Runtime.h>
#include <Foundation/Basics.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Types/UniquePtr.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

class plEditorUiAudioAttenuation;
class plEditorUiAudioEventAutomation;
class plEditorUiAudioEventDeck;
class plEditorUiAudioEventTimeline;
class plEditorUiAudioMessageList;
class plEditorUiAudioMeter;
class plEditorUiAudioShellStack;
class plEditorUiAudioStatusRow;
class plEditorUiControlStrip;
class plEditorUiDockManager;
struct plDocumentObjectPropertyEvent;
struct plDocumentObjectStructureEvent;
struct plSelectionManagerEvent;

/// \brief FMOD-style audio event editor: multi-track timeline on top, mockup deck below
/// (instrument playlist with weights, playback controls, automation curve), macros panel right.
///
/// The panels: "AudioEventTimelineDockWidget" (the toolbar row with the OUT meter, the parameter tabs and timeline,
/// the deck, AUTOMATION, MESSAGES TO GAME and DETAILS of the selection, and the status row) and "AudioEventAssetDockWidget"
/// (the asset's status and the event macros over the attenuation curve).
class plAudioEventAssetDocumentWindow : public plDocumentWindow
{
public:
  plAudioEventAssetDocumentWindow(plDocument* pDocument);
  ~plAudioEventAssetDocumentWindow();

  /// The Event Macros panel on the right of the event.
  static void ArrangePanels(plEditorUiDockManager& ref_panels);

private:
  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void SelectionEventHandler(const plSelectionManagerEvent& e);

  const plDocumentObject* GetPropertiesObject() const;
  const plDocumentObject* GetSelectedObjectOfProperty(const char* szParentProperty) const;

  /// \brief The instrument shown in the deck: the selected one (or the owner of the selected
  /// playlist entry), else the one last picked in the dropdown, else the first instrument.
  const plDocumentObject* GetActiveInstrument() const;

  /// \brief The automation shown in the deck: the selected one, else the first.
  const plDocumentObject* GetActiveAutomation() const;

  void CreateEventPanel();
  void CreateToolRow(plEditorUiAudioShellStack& ref_main);
  void CreateLowerRow(plEditorUiAudioShellStack& ref_main);
  void CreateMacrosPanel();

  /// Session solo of the timeline tracks.
  bool HasSoloTracks() const;
  bool IsSoloTrack(const plUuid& instrumentGuid) const;

  /// "..." of a playlist entry: picks its Audio Clip ("Set Playlist Clip").
  void PickEntryClip(const plUuid& entryGuid);

  void OnAddInstrument();
  void OnAddParameter();
  void OnRemoveSelected();

  /// \brief Auditions the event from the timeline cursor through the engine process (respects mute and session solo).
  void OnPlayEvent();
  void OnStopEvent();
  void OnTogglePlay();

  /// \brief Releases the audition's next sustain point.
  void OnKeyOff();

  /// \brief Opens the Audio Event asset with the given GUID (a nested event), after the current input event.
  void OpenNestedEvent(const plString& sEventGuid);

  /// \brief Sends the event as it is edited now to the engine process and plays it there. With a valid entryGuid only
  /// that playlist entry plays.
  void StartAudition(const plUuid& entryGuid, double fStartOffset);
  void SendAuditionCommand(plAudioAuditionCommand::Enum command);
  void OnAuditionParameterChanged(const plString& sName, float fValue);
  void OnAuditionStatus(const plAudioAuditionStatusMsgToEditor& status);
  void ShowAuditionLevel(float fPeak);
  void SetSeed(plUInt32 uiSeed);

  void OnAddPlaylistEntry();
  void OnAddFilesToInstrument();
  void OnAddClipToInstrument();

  /// \brief "+ Event...": picks another Audio Event and appends it to the active instrument's playlist (a nested event).
  void OnAddEventToInstrument();

  /// \brief Asset browser for Audio Clips, preselecting sCurrentClip; returns the picked clip's GUID string.
  bool PickClip(plStringView sCurrentClip, plString& out_sClipGuid);
  void OnAddAutomationFor(plUInt8 uiTarget, const plString& sParameter);

  /// \brief Multi-select sound file dialog; returns data-directory-relative paths.
  bool PickSoundFiles(plHybridArray<plString, 8>& out_files);

  void QueueRefresh();
  void Refresh();

  plAudioEventAssetDocument* m_pAssetDoc = nullptr;

  plUuid m_ActiveInstrumentGuid;
  bool m_bRefreshQueued = false;

  // audition in the engine process; the seed is a session value that makes the random picks repeat
  bool m_bAuditioning = false;
  bool m_bMixerNoteShown = false;
  bool m_bHolding = false;         ///< the audition's playhead waits at a sustain point
  double m_fAuditionPosition = 0.0; ///< timeline position of the last status, for the message times
  plUInt32 m_uiSeed = 1;
  plEventSubscriptionID m_AuditionSubscription = 0;

  // engine-drawn views
  plEditorUiControlStrip* m_pToolStrip = nullptr;
  plUInt32 m_uiPlay = 0;
  plUInt32 m_uiStop = 0;
  plUInt32 m_uiKeyOff = 0;
  plUInt32 m_uiSeedField = 0;
  plUInt32 m_uiNewSeed = 0;
  plUInt32 m_uiFit = 0;
  plUInt32 m_uiAddInstrument = 0;
  plUInt32 m_uiRemoveSelected = 0;
  plEditorUiAudioMeter* m_pMeterView = nullptr;
  plEditorUiAudioEventTimeline* m_pTimelineView = nullptr;
  plEditorUiAudioEventDeck* m_pDeckView = nullptr;
  plEditorUiAudioEventAutomation* m_pAutomationView = nullptr;
  plEditorUiAudioAttenuation* m_pAttenuationView = nullptr;
  bool m_bAttenuationShown = true; ///< the curve under the event macros; hidden for a 2D event
  plEditorUiAudioStatusRow* m_pStatusView = nullptr;
  plEditorUiAudioMessageList* m_pMessagesView = nullptr; ///< what the audition sends to the game: markers, sustain, key-off

  plEditorUiTimerGroup m_Timers; ///< the audition poll, the queued refresh and opening a nested event
};
