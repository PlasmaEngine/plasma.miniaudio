#pragma once

#include <AudioSystem/Mixer/AudioMixerData.h>
#include <EditorUi/Content.h>
#include <Foundation/Types/Delegate.h>
#include <Foundation/Types/Uuid.h>
#include <GameUiPlugin/Elements/UiDrawList.h>

class plDocument;
class plDocumentObject;
struct plEditorUiPointerEvent;

/// \brief A value the user drags in the mixer console: it is heard right away and written to the document on release.
struct plAudioMixerLiveEdit
{
  enum class Type : plUInt8
  {
    None,
    BusVolume,   ///< m_fValue in dB
    SendLevel,   ///< m_uiIndex = send, m_fValue in dB
    EffectParam, ///< m_uiIndex = effect, m_uiParam = registry slot
  };

  Type m_Type = Type::None;
  plUInt32 m_uiBus = 0; ///< document bus index
  plUInt32 m_uiIndex = 0;
  plUInt32 m_uiParam = 0;
  float m_fValue = 0.0f;
};

/// \brief What the mixer console shows besides the document: the engine process's live state, validation and the
/// window's selection. Owned by the mixer window, read by its views.
struct plAudioMixerConsoleState
{
  /// The engine process plays this document's mixer, so meters, mute, solo and snapshots are live.
  bool m_bLive = false;

  /// The bus whose insert the insert editor shows, and the insert; -1 shows the bus itself.
  plUuid m_SelectedBus;
  plInt32 m_iSelectedEffect = -1;

  /// Per bus, in document order.
  plDynamicArray<float> m_BusPeak; ///< linear
  plDynamicArray<float> m_BusRms;  ///< linear
  plDynamicArray<bool> m_BusMuted;
  plDynamicArray<bool> m_BusSoloed;
  plDynamicArray<bool> m_BusSilenced;     ///< silent because it is muted or another bus is soloed
  plDynamicArray<bool> m_BusError;        ///< a validation issue concerns the bus
  plDynamicArray<float> m_BusSnapshotDb;  ///< the volume with the playing snapshots blended in
  plDynamicArray<plHybridArray<float, 2>> m_EffectGainReduction; ///< per bus, per insert, in dB

  /// Per snapshot, in document order: the intensity that plays.
  plDynamicArray<float> m_SnapshotIntensity;

  plDynamicArray<plAudioMixerIssue> m_Issues;
};

/// \brief The snapshot rail under the mixer console's strips: per snapshot its name, priority, an intensity slider that
/// auditions it live, a Capture button and its overrides.
///
/// Clicking a name selects the snapshot. Capture stores the faders that differ from the authored mix into the snapshot
/// (the window does it, see m_OnCapture).
class plEditorUiAudioMixerSnapshotRail : public plEditorUiContent
{
public:
  plEditorUiAudioMixerSnapshotRail(plDocument* pDocument, const plAudioMixerConsoleState* pState);
  ~plEditorUiAudioMixerSnapshotRail() override;

  /// The height the window gives the rail.
  static float GetHeight();

  void Refresh();

  /// While dragging: the snapshot and its intensity.
  plDelegate<void(const plUuid& snapshot, float fIntensity)> m_OnIntensity;
  plDelegate<void(const plUuid& snapshot)> m_OnCapture;

protected:
  void OnUiUpdate() override;

private:
  struct Row
  {
    plUuid m_Guid;
    plRectFloat m_Name;
    plRectFloat m_Slider;
    plRectFloat m_Capture;
  };

  void BuildDocument();
  void Draw();
  void OnPointer(const plEditorUiPointerEvent& e);

  plDocument* m_pDocument = nullptr;
  const plAudioMixerConsoleState* m_pState = nullptr;
  plDynamicArray<Row> m_Rows;
  float m_fScrollY = 0.0f;

  plUuid m_DragGuid;
  plRectFloat m_DragSlider = plRectFloat(0, 0, 0, 0);
  float m_fDragValue = 0.0f;
  bool m_bDragging = false;

  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
  plEventSubscriptionID m_PointerSubscription = 0;
};

/// \brief The Insert panel of the mixer console: edits the selected insert, or the selected bus.
///
/// An insert shows its named parameters from the effect registry as sliders with units, Bypass and Remove, an EQ or
/// filter curve, and for a compressor or ducker its sidechain key and a gain reduction meter. A dragged slider is heard
/// right away (m_OnLiveEdit) and written as one undo step on release. A bus shows its output and its name; renaming it
/// goes through m_OnRename, which updates every reference.
class plEditorUiAudioMixerInsertEditor : public plEditorUiContent
{
public:
  plEditorUiAudioMixerInsertEditor(plDocument* pDocument, const plAudioMixerConsoleState* pState);
  ~plEditorUiAudioMixerInsertEditor() override;

  void Refresh();

  plDelegate<void(const plAudioMixerLiveEdit& edit)> m_OnLiveEdit;
  plDelegate<void(const plUuid& bus, plStringView sNewName)> m_OnRename;

protected:
  void OnUiUpdate() override;
  void OnUiEvent(const struct plMsgUiEvent& event) override;

private:
  struct ParamRow
  {
    plUInt32 m_uiParam = 0;
    plRectFloat m_Slider;
  };

  const plDocumentObject* GetBus() const;
  const plDocumentObject* GetEffect() const;
  plUInt32 GetBusIndex() const;

  void BuildDocument();
  void Draw();
  void DrawBus(const plDocumentObject* pBus, float fWidth);
  void DrawEffect(const plDocumentObject* pBus, const plDocumentObject* pEffect, float fWidth);
  void DrawCurve(const plRectFloat& rect, plAudioEffectType::Enum type, const float* pParams);
  void OnPointer(const plEditorUiPointerEvent& e);

  float GetParam(const plDocumentObject* pEffect, plUInt32 uiParam) const;
  void CommitParam(const plDocumentObject* pEffect, plUInt32 uiParam, float fValue);
  void ShowSidechainMenu(const plVec2& vPosition);

  plDocument* m_pDocument = nullptr;
  const plAudioMixerConsoleState* m_pState = nullptr;

  plDynamicArray<ParamRow> m_ParamRows;
  plRectFloat m_BypassButton = plRectFloat(0, 0, 0, 0);
  plRectFloat m_RemoveButton = plRectFloat(0, 0, 0, 0);
  plRectFloat m_SidechainField = plRectFloat(0, 0, 0, 0);

  // slider drag: heard while dragging, written on release
  plInt32 m_iDragParam = -1;
  plRectFloat m_DragSlider = plRectFloat(0, 0, 0, 0);
  float m_fDragValue = 0.0f;

  // the document is rebuilt when the panel switches between a bus (with its name field) and an insert
  plUuid m_BuiltBus;
  plInt32 m_iBuiltEffect = -2;
  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
  plEventSubscriptionID m_PointerSubscription = 0;
};
