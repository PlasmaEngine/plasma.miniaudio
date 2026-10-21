#pragma once

#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAsset.h>
#include <Foundation/Basics.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Types/UniquePtr.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.moc.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

class QTimer;
class QVBoxLayout;
class plEditorUiAudioEventAutomation;
class plEditorUiAudioEventDeck;
class plEditorUiAudioEventTimeline;
class plQtEngineUiContentWidget;
class plEditorUiAudioMeter;
class plEditorUiAudioStatusRow;
class plQtEngineControlStrip;
class plQtPropertyGridWidget;
struct plDocumentObjectPropertyEvent;
struct plDocumentObjectStructureEvent;
struct plSelectionManagerEvent;

/// \brief FMOD-style audio event editor: multi-track timeline on top, mockup deck below
/// (instrument playlist with weights, playback controls, automation curve), macros dock right.
///
/// The toolbar row with the OUT meter, the parameter tabs and timeline, the deck and the status row are
/// engine-drawn.
class plAudioEventAssetDocumentWindow : public plQtDocumentWindow
{
  Q_OBJECT

public:
  plAudioEventAssetDocumentWindow(plDocument* pDocument);
  ~plAudioEventAssetDocumentWindow();

  virtual const char* GetGroupName() const { return "AudioEventAsset"; }

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

  void CreateEngineCentralWidget(QWidget* pCentralWidget, QVBoxLayout* pCentralLayout);

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

  /// \brief Asset browser for Audio Clips, preselecting sCurrentClip; returns the picked clip's GUID string.
  bool PickClip(plStringView sCurrentClip, plString& out_sClipGuid);
  void OnAddAutomationFor(plUInt8 uiTarget, const plString& sParameter);

  /// \brief Multi-select sound file dialog; returns data-directory-relative paths.
  bool PickSoundFiles(plHybridArray<plString, 8>& out_files);

  void QueueRefresh();

  plAudioEventAssetDocument* m_pAssetDoc = nullptr;

  plUuid m_ActiveInstrumentGuid;
  bool m_bRefreshQueued = false;

  // audition in the engine process; the seed is a session value that makes the random picks repeat
  bool m_bAuditioning = false;
  bool m_bMixerNoteShown = false;
  plUInt32 m_uiSeed = 1;
  plEventSubscriptionID m_AuditionSubscription = 0;

  QTimer* m_pMeterTimer = nullptr;

  // engine-drawn views
  plQtEngineControlStrip* m_pToolStrip = nullptr;
  plUInt32 m_uiPlay = 0;
  plUInt32 m_uiStop = 0;
  plUInt32 m_uiSeedField = 0;
  plUInt32 m_uiNewSeed = 0;
  plUInt32 m_uiFit = 0;
  plUInt32 m_uiAddInstrument = 0;
  plUInt32 m_uiRemoveSelected = 0;
  plEditorUiAudioMeter* m_pMeterView = nullptr;
  plEditorUiAudioEventTimeline* m_pTimelineView = nullptr;
  plEditorUiAudioEventDeck* m_pDeckView = nullptr;
  plQtEngineUiContentWidget* m_pDeckWidget = nullptr; ///< takes the height the deck's playlist needs
  plEditorUiAudioEventAutomation* m_pAutomationView = nullptr;
  plEditorUiAudioStatusRow* m_pStatusView = nullptr;
};
