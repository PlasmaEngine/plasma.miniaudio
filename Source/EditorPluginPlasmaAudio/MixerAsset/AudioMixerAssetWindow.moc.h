#pragma once

#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerAsset.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerConsoleViews.h>
#include <Foundation/Basics.h>
#include <Foundation/Containers/HashTable.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.moc.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

class QTimer;
class plEditorUiAudioMixerRouting;
class plEditorUiAudioMixerStrips;
class plEditorUiAudioStatusRow;
class plQtDocumentPanel;
class plQtEngineControlStrip;
struct plDocumentEvent;
struct plDocumentObjectPropertyEvent;
struct plDocumentObjectStructureEvent;
struct plSelectionManagerEvent;

/// \brief Audio mixer editor, the live mixer console: the strips with the snapshot rail in the centre, the routing tree
/// on the left, the insert editor and the properties of the selection on the right.
///
/// The console runs against the editor's engine process. Every edit, and every value while it is dragged, reaches the
/// mixer resource there (plAudioMixerContext), and a mixer the engine process plays applies it without stopping what
/// plays. The source picks what is heard: Preview loads this mixer and can play a test signal into it; Engine process
/// shows whichever mixer plays (a running scene's), live only when that is this one. Meters, gain reduction, mute,
/// solo and snapshot intensities come back from the engine process. Invalid routing is shown as a banner and outlined
/// strips and is not applied.
class plAudioMixerAssetDocumentWindow : public plQtDocumentWindow
{
  Q_OBJECT

public:
  plAudioMixerAssetDocumentWindow(plDocument* pDocument);
  ~plAudioMixerAssetDocumentWindow();

  virtual const char* GetGroupName() const { return "AudioMixerAsset"; }

private:
  enum class Source : plUInt8
  {
    Preview,
    EngineProcess,
  };

  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void SelectionEventHandler(const plSelectionManagerEvent& e);
  void DocumentEventHandler(const plDocumentEvent& e);
  void OnConsoleStatus(const plAudioMixerConsoleStatusMsgToEditor& status);

  const plDocumentObject* GetPropertiesObject() const;

  void CreateViews();
  void CreatePropertyDock();

  void OnAddBus(bool bAux);
  void OnAddVca();
  void OnAddSnapshot();
  void OnRemoveSelected();
  void SetSource(Source source);

  void QueueRefresh();

  /// \brief Builds the runtime desc and the validation issues from the document.
  void RebuildDesc();

  /// \brief Sends the document's mixer, with the value being dragged, to the engine process. Invalid mixers are not
  /// sent. Throttled while dragging.
  void ApplyToEngine(bool bForce);

  /// \brief Sends a request to the mixer context; returns false without an engine process connection.
  bool SendConsoleCommand(plAudioMixerConsoleCommand::Enum command, plStringView sTarget = {}, float fValue = 0.0f);

  /// \brief Fills the console state from the last status and the document.
  void UpdateConsoleState();

  void OnLiveEdit(const plAudioMixerLiveEdit& edit);
  void OnMute(const plUuid& bus, bool bMute);
  void OnSolo(const plUuid& bus, bool bSolo);
  void OnSelectInsert(const plUuid& bus, plInt32 iEffect);
  void OnSnapshotIntensity(const plUuid& snapshot, float fIntensity);

  /// \brief Stores the faders that differ from the authored mix into the snapshot and resets them, in one step.
  void OnCapture(const plUuid& snapshot);

  /// \brief Renames a bus and updates every reference to it, in this mixer and in the audio events, in one step.
  void RenameBus(const plUuid& bus, plStringView sNewName);

  /// \brief Updates the references to a bus that was renamed in the property grid, as part of that step.
  void UpdateReferencesAfterRename(plStringView sOldName, plStringView sNewName);

  /// \brief Changes the Output Bus of every audio event that outputs to sOldName. Returns how many changed.
  plUInt32 RenameEventOutputBuses(plStringView sOldName, plStringView sNewName);

  /// \brief Remembers the current bus volumes as the authored mix Capture compares against.
  void TakeBaseline();

  void ShowToast(plStringView sText);
  void UpdateStatusRow();

  plAudioMixerAssetDocument* m_pAssetDoc = nullptr;
  bool m_bRefreshQueued = false;

  plQtEngineControlStrip* m_pToolStrip = nullptr;
  plUInt32 m_uiAddBus = 0;
  plUInt32 m_uiAddAuxBus = 0;
  plUInt32 m_uiAddVca = 0;
  plUInt32 m_uiAddSnapshot = 0;
  plUInt32 m_uiRemoveSelected = 0;
  plUInt32 m_uiSourcePreview = 0;
  plUInt32 m_uiSourceEngine = 0;
  plUInt32 m_uiTestSignal = 0;
  plUInt32 m_uiValidation = 0;
  plEditorUiAudioMixerStrips* m_pStripsView = nullptr;
  plEditorUiAudioMixerSnapshotRail* m_pRailView = nullptr;
  plEditorUiAudioMixerRouting* m_pRoutingView = nullptr;
  plEditorUiAudioMixerInsertEditor* m_pInsertView = nullptr;
  plQtDocumentPanel* m_pInsertPanel = nullptr;
  plEditorUiAudioStatusRow* m_pStatusView = nullptr;

  // the console
  plAudioMixerConsoleState m_State;
  plAudioMixerDesc m_Desc;
  bool m_bDescValid = false;
  Source m_Source = Source::Preview;
  bool m_bTestSignal = false;
  plAudioMixerLiveEdit m_LiveEdit;
  plTime m_LastApply;
  bool m_bApplyPending = false;
  QTimer* m_pPollTimer = nullptr;
  plEventSubscriptionID m_ConsoleSubscription = 0;

  /// The bus volumes Capture compares the faders against: when the window opened, saved or captured.
  plHashTable<plUuid, float> m_Baseline;

  /// Renames in the property grid whose references are updated once the rename's step is done.
  plString m_sPendingRenameFrom;
  plString m_sPendingRenameTo;
  bool m_bUpdatingReferences = false;

  plString m_sToast;
  plTime m_ToastTime;
};
