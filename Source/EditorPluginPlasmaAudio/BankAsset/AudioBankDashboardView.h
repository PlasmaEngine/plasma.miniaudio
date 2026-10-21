#pragma once

#include <EditorPluginPlasmaAudio/BankAsset/AudioBankDashboard.h>
#include <EditorUi/Content.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <GameUiPlugin/Elements/UiDrawList.h>

class plAudioBankAssetDocument;
class plAudioBankStatusMsgToEditor;
struct plAssetCuratorEvent;
struct plDocumentObjectPropertyEvent;
struct plDocumentObjectStructureEvent;
struct plEditorUiPointerEvent;

/// \brief The Audio Bank window, engine-drawn: every bank of the project against the memory budget of a platform.
///
/// The bar picks the platform (the asset profiles), the language and whether the window's bank auto-includes the clips
/// of its events. Under it, the memory the loaded banks take on that platform, by category, against the platform's
/// budget. On the left the banks, with their size and load state, and the warnings; on the right the selected bank's
/// contents. Load and Unload load the selected bank in the editor's engine process. Clicking a warning selects its
/// bank and the asset it is about; double-clicking a row opens the asset.
class plEditorUiAudioBankDashboard : public plEditorUiContent
{
public:
  explicit plEditorUiAudioBankDashboard(plAudioBankAssetDocument* pDocument);
  ~plEditorUiAudioBankDashboard() override;

  /// Rebuilds the model when the curator reported changes since the last call. Main thread, called by the window's timer.
  void OnTimer();

  plString GetToolTipAt(const plVec2& vLogical) const override;

protected:
  void OnUiUpdate() override;

private:
  /// A clickable part of the last drawing.
  struct Hit
  {
    enum class Kind : plUInt8
    {
      Platform,
      Language,
      AutoInclude,
      Bank,
      Warning,
      Load,
      Open,
      Row,
    };

    Kind m_Kind;
    plRectFloat m_Rect;
    plInt32 m_iIndex = -1;
    plString m_sToolTip;
  };

  void Rebuild();
  void Draw();
  void DrawBar(const plVec2& size);
  void DrawBudget(const plVec2& size);
  void DrawBanks(const plRectFloat& area);
  void DrawWarnings(const plRectFloat& area);
  void DrawContents(const plRectFloat& area);
  void DrawStatusRow(const plVec2& size);

  void AddHit(Hit::Kind kind, const plRectFloat& rect, plInt32 iIndex = -1, plStringView sToolTip = {});
  const Hit* FindHit(const plVec2& vPos) const;

  void OnPointer(const plEditorUiPointerEvent& e);
  void OnClick(const Hit& hit);
  void ShowLanguageMenu(const plRectFloat& rect);
  void SetAutoInclude(bool bAutoInclude);
  void SelectWarning(plUInt32 uiWarning);
  void OpenAsset(plStringView sGuid);

  void OnEngineStatus(const plAudioBankStatusMsgToEditor& status);
  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void CuratorEventHandler(const plAssetCuratorEvent& e);

  const plAudioBankDashboard::Bank* GetSelectedBank() const;

  plAudioBankAssetDocument* m_pDocument = nullptr;
  plAudioBankDashboard m_Model;

  plInt32 m_iSelectedBank = 0;
  plString m_sHighlightedAsset;
  float m_fContentScroll = 0.0f; ///< rows
  float m_fWarningScroll = 0.0f; ///< rows
  plRectFloat m_ContentRows = plRectFloat(0, 0, 0, 0);
  plRectFloat m_WarningRows = plRectFloat(0, 0, 0, 0);
  plInt32 m_iHoverRow = -1;

  plDynamicArray<Hit> m_Hits;
  plAtomicInteger32 m_iCuratorChanges; ///< curator events since the last rebuild, from any thread
  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
  plEventSubscriptionID m_PointerSubscription = 0;
  plEventSubscriptionID m_StatusSubscription = 0;
  plEventSubscriptionID m_CuratorSubscription = 0;
};
