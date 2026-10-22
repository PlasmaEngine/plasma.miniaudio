#pragma once

#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <EditorUi/Runtime.h>
#include <Foundation/Threading/TaskSystem.h>
#include <Foundation/Types/RefCounted.h>
#include <Foundation/Types/SharedPtr.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.h>
#include <PlasmaAudioPlugin/Clips/AudioClipDesc.h>

#include <atomic>

class plEditorUiAudioClipUsedBy;
class plEditorUiAudioClipWaveform;
class plEditorUiControlStrip;
class plEditorUiDockManager;
class plEditorUiPropertyGrid;
struct plAssetCuratorEvent;
struct plDocumentObjectPropertyEvent;
struct plDocumentObjectStructureEvent;

/// \brief The outcome of building a clip in the editor with the settings of one platform tab.
struct plAudioClipPreview : public plRefCounted
{
  plUInt64 m_uiHash = 0;
  bool m_bSuccess = false;
  plString m_sError;

  plAudioClipBuildSettings m_Settings;
  plAudioClipHeader m_Header;
  plUInt64 m_uiFileBytes = 0;   ///< size of the built clip (default language only)
  plDynamicArray<plInt16> m_Pcm; ///< the built payload decoded again, for auditioning

  /// \brief Source time (seconds) of output frame 0: how much leading silence was trimmed.
  double m_fTrimOffset = 0.0;
};

/// \brief One preview build, run on a long running task. The window polls m_bDone.
struct plAudioClipPreviewJob : public plRefCounted
{
  plString m_sAbsSourceFile;
  plAudioClipBuildSettings m_Settings;
  plSharedPtr<plAudioClipPreview> m_pResult;
  std::atomic<bool> m_bDone = false;

  void Run();
};

/// \brief Audio Clip editor: waveform with loop markers, info and cost estimates for each platform tab, the property grid
/// of the default settings or of the selected platform override, and the assets that use the clip.
///
/// The panels: "AudioClipWaveformDockWidget" (the transport and info row over the tabs, ruler, waveform and stat row)
/// and "AudioClipAssetDockWidget" (the platform the grid edits, the asset's status and grid, and the "Used by" list).
class plAudioClipAssetDocumentWindow : public plDocumentWindow
{
public:
  plAudioClipAssetDocumentWindow(plDocument* pDocument);
  ~plAudioClipAssetDocumentWindow();

  /// The default layout of the panels: Clip Properties on the right of Audio Clip.
  static void ArrangePanels(plEditorUiDockManager& ref_panels);

private:
  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void AssetCuratorEventHandler(const plAssetCuratorEvent& e);

  const plDocumentObject* GetPropertiesObject() const;
  const plDocumentObject* GetActiveOverrideObject() const;

  void CreateClipPanel();
  void CreatePropertiesPanel();

  void RebuildTabs();
  void OnTabChanged(int iIndex);
  void UpdatePropertyGridSelection();

  /// The asset profiles an override can be added for, and whether each one already has an override.
  void GetOverrideProfiles(plDynamicArray<plString>& out_profiles, plDynamicArray<bool>& out_exists) const;
  void AddPlatformOverride(const plString& sProfile);
  void RemovePlatformOverride(const plUuid& overrideGuid);

  /// The engine-drawn "+" menu and the override tab menu.
  void ShowAddOverrideMenu(const plVec2& vPosition);
  void ShowTabMenu(plUInt32 uiTab, const plVec2& vPosition);

  /// Writes a loop dragged on the engine waveform (frames of the built clip) as source seconds.
  void SetLoopPoints(plUInt64 uiStartFrame, plUInt64 uiEndFrame);

  void RequestPreview(bool bImmediate);
  void OnTimer();
  void StartPreviewIfNeeded();
  void ShowPreview(const plSharedPtr<plAudioClipPreview>& pPreview);
  void ShowWaveformMessage(plStringView sMessage);
  void UpdateLoopLabel();
  void RefreshUsedBy();

  void SetInfoText(plStringView sText);
  void SetLoopText(plStringView sText);
  void SetPlatformText(plStringView sText);
  bool IsLoopEnabled() const;

  /// \brief Auditions the built clip in the editor's engine process, through the game's event manager and mixer.
  void OnPlay();
  void OnStop();
  void OnAuditionStatus(const plAudioAuditionStatusMsgToEditor& status);

  /// \brief The waveform frame the audition plays at, from its position in seconds; -1 while nothing plays.
  plInt64 GetAuditionCursor() const;

  plAudioClipAssetDocument* m_pAssetDoc = nullptr;
  bool m_bAuditioning = false;
  plEventSubscriptionID m_AuditionSubscription = 0;

  // engine-drawn views
  plEditorUiControlStrip* m_pToolStrip = nullptr;
  plUInt32 m_uiPlay = 0;
  plUInt32 m_uiStop = 0;
  plUInt32 m_uiLoop = 0;
  plUInt32 m_uiInfo = 0;
  plUInt32 m_uiLoopLabel = 0;
  plEditorUiControlStrip* m_pPlatformStrip = nullptr;
  plUInt32 m_uiPlatform = 0;
  plEditorUiAudioClipWaveform* m_pWaveView = nullptr;
  plEditorUiAudioClipUsedBy* m_pUsedByView = nullptr;
  plEditorUiPropertyGrid* m_pPropertyGrid = nullptr;

  plEditorUiTimerGroup m_Timers; ///< polls the tabs, the "Used by" list, the preview builds and the audition

  plUuid m_ActiveOverrideGuid;              ///< invalid: the Default tab
  plHybridArray<plUuid, 8> m_TabOverrides;  ///< override object per tab, invalid for the Default tab
  bool m_bUpdatingTabs = false;
  bool m_bTabsDirty = false;
  std::atomic<bool> m_bUsedByDirty = true;

  // preview builds
  bool m_bPreviewRequested = false;
  plTime m_PreviewDue;
  plSharedPtr<plAudioClipPreviewJob> m_pRunningJob;
  plTaskGroupID m_RunningTask;
  plHybridArray<plSharedPtr<plAudioClipPreview>, 8> m_PreviewCache; ///< most recent last
  plSharedPtr<plAudioClipPreview> m_pShownPreview;
  plUInt64 m_uiWantedHash = 0;
};
