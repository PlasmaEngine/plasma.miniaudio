#pragma once

#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerConsoleViews.h>
#include <EditorUi/Content.h>
#include <EditorUi/Tree.h>
#include <Foundation/Types/UniquePtr.h>
#include <Foundation/Types/Uuid.h>
#include <GameUiPlugin/Elements/UiDrawList.h>

class plDocument;
class plDocumentObject;
struct plEditorUiPointerEvent;

/// \brief The engine-drawn mixer console: one channel strip per bus and, after a dashed divider, one per VCA.
///
/// A strip has a header with the system colour as its left edge (Audio for buses, Scripting for aux buses,
/// Utilities for VCAs), mute and solo, the inserts (or the controlled buses of a VCA), the first send with its level
/// (or the environment of an aux bus), the ducking a sidechain applies, a fader from -60 to +12 dB with its scale, the
/// bus meter on the dBFS scale, the readout (with the playing snapshots blended in) and the routing.
///
/// Clicking selects the bus or VCA, or an insert. A dragged fader or send is heard right away through m_OnLiveEdit and
/// written as one undo step on release; the fader snaps to 0.5 dB with a detent at 0 dB. "+ insert" adds an effect
/// from the registry. Buses a validation issue concerns are outlined, and with the issues a banner names the first one.
/// Buses silenced by mute or solo are dimmed.
class plEditorUiAudioMixerStrips : public plEditorUiContent
{
public:
  plEditorUiAudioMixerStrips(plDocument* pDocument, const plAudioMixerConsoleState* pState);
  ~plEditorUiAudioMixerStrips() override;

  /// Re-reads the document and the console state.
  void Refresh();

  plDelegate<void(const plAudioMixerLiveEdit& edit)> m_OnLiveEdit;
  plDelegate<void(const plUuid& bus, bool bMute)> m_OnMute;
  plDelegate<void(const plUuid& bus, bool bSolo)> m_OnSolo;

  /// An insert slot (or, with -1, the strip) was clicked.
  plDelegate<void(const plUuid& bus, plInt32 iEffect)> m_OnSelectInsert;

protected:
  void OnUiUpdate() override;

private:
  struct Strip
  {
    plUuid m_Guid;
    plUInt32 m_uiBus = plInvalidIndex;
    plRectFloat m_Rect = plRectFloat(0, 0, 0, 0);
    plRectFloat m_Fader = plRectFloat(0, 0, 0, 0);
    plRectFloat m_Mute = plRectFloat(0, 0, 0, 0);      ///< empty for VCAs
    plRectFloat m_Solo = plRectFloat(0, 0, 0, 0);      ///< empty for VCAs
    plRectFloat m_AddInsert = plRectFloat(0, 0, 0, 0); ///< empty when the slots are full
    plRectFloat m_Send = plRectFloat(0, 0, 0, 0);      ///< the first send's slider; empty without sends
    plHybridArray<plRectFloat, 4> m_Inserts;
    bool m_bVca = false;
  };

  const plDocumentObject* GetPropertiesObject() const;
  void GatherStrips(plHybridArray<const plDocumentObject*, 12>& out_buses, plHybridArray<const plDocumentObject*, 4>& out_vcas) const;
  float GetContentWidth() const;

  void BuildDocument();
  void Draw();
  void DrawStrip(const plRectFloat& rect, const plDocumentObject* pObject, plUInt32 uiBusIndex, bool bVca, bool bMaster, bool bSelected);
  void OnPointer(const plEditorUiPointerEvent& e);
  void ShowAddInsertMenu(const plUuid& bus, const plVec2& vPosition);

  plDocument* m_pDocument = nullptr;
  const plAudioMixerConsoleState* m_pState = nullptr;
  plDynamicArray<Strip> m_Strips;
  float m_fScrollX = 0.0f;
  float m_fTop = 0.0f; ///< below the validation banner

  // fader or send drag: heard while dragging, written on release
  enum class Drag : plUInt8
  {
    None,
    Fader,
    Send,
  };

  Drag m_Drag = Drag::None;
  plUuid m_DragGuid;
  plUInt32 m_uiDragBus = 0;
  plRectFloat m_DragRect = plRectFloat(0, 0, 0, 0);
  float m_fDragValueDb = 0.0f;

  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
  plEventSubscriptionID m_PointerSubscription = 0;
};

/// \brief The Routing dock: the buses as a hierarchy (parents by name, unknown parents under the master bus), the
/// VCAs and the snapshots, each under its group. Clicking selects the object.
class plEditorUiAudioMixerRouting : public plEditorUiContent
{
public:
  explicit plEditorUiAudioMixerRouting(plDocument* pDocument);
  ~plEditorUiAudioMixerRouting() override;

  /// Re-reads the document and shows the document's selection.
  void Refresh();

protected:
  void OnUiUpdate() override;

private:
  class Adapter;

  void BuildDocument();
  void OnTreeEvent(const plEditorUiTreeEvent& e);

  plDocument* m_pDocument = nullptr;
  plUniquePtr<Adapter> m_pAdapter;
  plUniquePtr<plEditorUiTree> m_pTree;
  bool m_bRebuild = true;
  bool m_bSyncingSelection = false;
  plEventSubscriptionID m_TreeSubscription = 0;
};

