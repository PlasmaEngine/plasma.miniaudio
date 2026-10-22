#pragma once

#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <EditorUi/Content.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Types/UniquePtr.h>
#include <GameUiPlugin/Elements/UiDrawList.h>

class plAudioMusicAssetDocument;
class plEditorUiSearchField;
struct plEditorUiDropEvent;
struct plEditorUiKeyEvent;
struct plEditorUiPointerEvent;
struct plEditorUiSearchFieldEvent;

/// \brief The Audio Music window's view: the music is built and auditioned here.
///
/// The Clips column lists the project's Audio Clips. Dragging one onto a state's lane makes a segment of it (its bars
/// come from the clip's length at the lane's tempo), onto a segment adds it as a stem, onto "Not in a state" makes a
/// segment that only plays between states. Segments drag along their lane to reorder, onto another lane to play there
/// too, onto "Not in a state" to leave their state. Clips from the asset browser drop the same way.
///
/// The transitions matrix has one cell per change, from its row to its column; the "any state" row holds the rules that
/// apply from every state without one of its own. The inspector on the right edits what is selected: a segment (tempo,
/// meter, bars, exit cues, stems), a state (name, initial, play order), a transition or a stinger. Double-clicking a
/// state's name renames it. Every edit is one undo step; renames keep the references by name.
///
/// The toolbar plays the music in the engine process's audition, asks for states and plays stingers; while it plays,
/// the playing segment shows the playhead, a state asked for is outlined amber and the messages the game object would
/// get show under the inspector.
class PL_EDITORPLUGINPLASMAAUDIO_DLL plEditorUiAudioMusicView : public plEditorUiContent
{
public:
  explicit plEditorUiAudioMusicView(plAudioMusicAssetDocument* pDocument);
  ~plEditorUiAudioMusicView() override;

  /// \brief The document changed: lanes, matrix and inspector are read again.
  void Refresh();

  /// \brief Why the music does not build (empty when it does). Shown in the status row.
  void SetBuildError(plStringView sError);

  /// \brief What the audition reports: whether it plays, and the music's position and pending change.
  struct Status
  {
    bool m_bPlaying = false;
    plUInt32 m_uiState = 0xFFFF;
    plUInt32 m_uiSegment = 0xFFFF;
    bool m_bTransitionSegment = false;
    plUInt32 m_uiBar = 0;
    plUInt32 m_uiBeat = 0;
    float m_fTempo = 0.0f;
    plUInt32 m_uiBeatsPerBar = 0;
    plUInt32 m_uiPendingState = 0xFFFF;
    double m_fPendingIn = -1.0;
  };
  void SetStatus(const Status& status);

  /// \brief A message the game object would get: a transition, a stinger, a beat or a note of the audition.
  enum class MessageKind : plUInt8
  {
    Note,
    Transition,
    Stinger,
    Beat,
  };
  void AddMessage(plStringView sText, MessageKind kind);
  void ClearMessages();

  /// \brief The audition started or stopped.
  void SetAuditioning(bool bAuditioning);

  /// \brief A stinger was asked for.
  void NoteStingerRequest(plUInt32 uiStinger);

  /// \brief What the view selected, for the document's selection (the Music Properties).
  enum class Selection : plUInt8
  {
    Music,
    Segment,
    State,
    Transition, ///< the index is the transition's, or plInvalidIndex for a change without a rule
    Stinger,
  };

  plDelegate<void()> m_OnPlay;
  plDelegate<void()> m_OnStop;
  plDelegate<void(plUInt32)> m_OnStateChosen;
  plDelegate<void(plUInt32)> m_OnStinger;
  plDelegate<void(Selection, plUInt32)> m_OnSelect;

protected:
  void OnUiUpdate() override;
  void OnUiEvent(const plMsgUiEvent& event) override;

private:
  struct Clip
  {
    plString m_sGuid;
    plString m_sName;
    double m_fSeconds = 0.0;
  };

  /// A segment's block on a lane.
  struct Block
  {
    plUInt32 m_uiLane = 0; ///< a state, or the loose lane after the states
    plUInt32 m_uiPosition = 0; ///< in the state's play order
    plUInt32 m_uiSegment = 0;
    plRectFloat m_Rect;
  };

  struct Lane
  {
    plRectFloat m_Header;
    plRectFloat m_Track;
  };

  /// A cell of the transitions matrix: the change from a row (plInvalidIndex: any state) to a column.
  struct Cell
  {
    plUInt32 m_uiFrom = plInvalidIndex;
    plUInt32 m_uiTo = 0;
    plRectFloat m_Rect;
  };

  /// Where everything of the columns is, computed from the document, the size and the scroll.
  struct Layout
  {
    plRectFloat m_Clips;
    plRectFloat m_Center;
    plRectFloat m_Inspector;
    plRectFloat m_Messages; ///< under the inspector while there are messages; empty otherwise
    plRectFloat m_ClipList; ///< the rows' area
    plHybridArray<Lane, 8> m_Lanes;
    plHybridArray<Block, 16> m_Blocks;
    double m_fPixelsPerSecond = 34.0;
    float m_fMatrixTop = 0.0f;
    float m_fRowLabelWidth = 0.0f;
    float m_fColumnWidth = 0.0f;
    plHybridArray<Cell, 16> m_Cells;
    float m_fCenterHeight = 0.0f; ///< unscrolled
  };

  /// What the inspector shows: controls and texts placed in its column (scrolled).
  enum class ItemType : plUInt8
  {
    Header,    ///< m_sText, m_sSub small
    Label,     ///< m_sText (a row's label), m_sSub right aligned (a value)
    Text,      ///< m_sText dim, wrapped to the width
    Field,     ///< a text input named m_sId
    Number,    ///< a number field named m_sId, m_sSub its unit
    Toggle,    ///< a check box, m_bOn
    Option,    ///< a part of a segmented choice, m_bOn
    Chip,      ///< an exit cue chip, m_bOn
    Dropdown,  ///< m_sText the value
    Slider,    ///< m_fValue 0..1
    Button,    ///< dashed accent "add" button
    Danger,    ///< a delete button
    Card,      ///< a stem's background
    Row,       ///< a state's segment row: m_sText name, m_sSub length
    Remove,    ///< a small cross
    DropZone,  ///< "drop a clip to add a stem"
  };

  enum Tone : plUInt8
  {
    ToneNormal,
    ToneWarning,
    ToneGood,
  };

  struct Item
  {
    ItemType m_Type = ItemType::Text;
    plRectFloat m_Rect;
    plString m_sId;
    plString m_sText;
    plString m_sSub;
    bool m_bOn = false;
    float m_fValue = 0.0f;
    Tone m_Tone = ToneNormal;
  };

  enum class SelectionKind : plUInt8
  {
    None,
    Segment,
    State,
    Transition,
    Stinger,
  };

  /// What is dragged inside the view.
  enum class DragKind : plUInt8
  {
    None,
    Clip,
    Segment,
    Row,
  };

  /// Where a drag would land.
  struct DropTarget
  {
    enum class Kind : plUInt8
    {
      None,
      Lane,    ///< insert into m_uiLane at m_uiPosition
      Segment, ///< a clip onto m_uiSegment: a stem
      Stems,   ///< a clip onto the inspector's drop zone: a stem of the selected segment
      Row,     ///< a row of the state's play order onto m_uiPosition
    };

    Kind m_Kind = Kind::None;
    plUInt32 m_uiLane = 0;
    plUInt32 m_uiPosition = 0;
    plUInt32 m_uiSegment = 0;
    float m_fCaretX = 0.0f;
  };

  PL_NODISCARD double Now() const;
  PL_NODISCARD plUInt32 GetLooseLane() const;
  PL_NODISCARD double GetSegmentSeconds(plUInt32 uiSegment) const;
  PL_NODISCARD float GetStateTempo(plUInt32 uiState) const;
  PL_NODISCARD plUInt32 GetInitialState() const;
  PL_NODISCARD plUInt32 FindStinger(plStringView sName) const;

  void GatherClips();
  void GetVisibleClips(plDynamicArray<const Clip*>& out_clips) const;

  void ComputeLayout(Layout& out_layout) const;
  void ComputeInspector(const Layout& layout, plDynamicArray<Item>& out_items, float& out_fHeight) const;
  void ValidateSelection();
  void Select(SelectionKind kind, plUInt32 uiIndex, plUInt32 uiTo = 0);
  void ShowToast(plStringView sText);
  void GetIssues(plStringBuilder& out_sFirst, plUInt32& out_uiCount) const;

  void BuildDocument();
  void UpdateFieldTexts();
  void CommitField(plStringView sField);
  void BeginRename(plUInt32 uiState);
  void CommitRename();

  void Draw();
  void DrawToolbar();
  void DrawClips(const Layout& layout);
  void DrawLanes(const Layout& layout);
  void DrawBlock(const Block& block, bool bSelected, bool bPlaying);
  void DrawMatrix(const Layout& layout);
  void DrawInspector(const Layout& layout);
  void DrawItem(const Item& item);
  void DrawMessages(const plRectFloat& area);
  void DrawStatus(const plRectFloat& area);
  void DrawDrag();

  void OnPointer(const plEditorUiPointerEvent& e);
  void OnKey(const plEditorUiKeyEvent& e);
  void OnDrop(const plEditorUiDropEvent& e);
  void OnSearch(const plEditorUiSearchFieldEvent& e);
  DropTarget FindDropTarget(const plVec2& vPos, DragKind kind) const;
  void ApplyDrop(const DropTarget& target, DragKind kind, plStringView sClip);
  bool FindSlider(const plVec2& vPos, Item& out_slider) const;
  void CommitSlider();

  void ShowChangeAtMenu(const plRectFloat& rect);
  void ShowViaMenu(const plRectFloat& rect);
  void ShowStingerAtMenu(const plRectFloat& rect);
  void ShowClipMenu(const plRectFloat& rect, bool bStinger);

  plAudioMusicAssetDocument* m_pDocument = nullptr;
  plString m_sBuildError;

  // the project's clips and the filter; the list scrolls by rows
  plDynamicArray<Clip> m_Clips;
  plTime m_ClipsGathered;
  plString m_sClipFilter;
  plUniquePtr<plEditorUiSearchField> m_pClipSearch;
  plUInt32 m_uiClipScroll = 0;
  float m_fCenterScroll = 0.0f;
  float m_fInspectorScroll = 0.0f;

  // selection
  SelectionKind m_Selection = SelectionKind::None;
  plUInt32 m_uiSelected = 0;
  plUInt32 m_uiSelectedTo = 0; ///< a transition's column; m_uiSelected is its row (plInvalidIndex: any state)

  // drags: from the view (pointer) or from outside (asset browser)
  DragKind m_DragKind = DragKind::None;
  plString m_sDragClip;
  plString m_sDragLabel;
  plUInt32 m_uiDragLane = 0;
  plUInt32 m_uiDragPosition = 0;
  plUInt32 m_uiDragSegment = 0;
  plVec2 m_vDragPos = plVec2::MakeZero();
  DropTarget m_DropTarget;

  // a slider being dragged: written on release
  plString m_sSlider;
  float m_fSliderValue = 0.0f;

  // fields: the one being edited commits when focus leaves; the lane rename likewise
  plString m_sFocusedField;
  plUInt32 m_uiRenameState = plInvalidIndex;
  bool m_bRenameHadFocus = false;

  // the audition
  Status m_Status;
  bool m_bAuditioning = false;
  double m_fClockOrigin = 0.0;
  double m_fSegmentAnchor = 0.0; ///< the playing segment's beat 0 on the view's clock
  plUInt32 m_uiAnchorSegment = 0xFFFF;
  bool m_bAnchorTransition = false;
  plUInt32 m_uiLastBeatIndex = 0xFFFFFFFF;

  struct Message
  {
    plString m_sText;
    plString m_sPosition; ///< "bar.beat" when it came
    MessageKind m_Kind = MessageKind::Note;
  };
  plDynamicArray<Message> m_Messages; ///< newest last

  plString m_sToast;
  double m_fToastTime = -100.0;

  plVec2 m_vBuiltSize = plVec2::MakeZero();
  bool m_bRebuild = true;
  plUiDrawListWriter m_Writer;
  plEventSubscriptionID m_PointerSubscription = 0;
  plEventSubscriptionID m_KeySubscription = 0;
  plEventSubscriptionID m_DropSubscription = 0;
  plEventSubscriptionID m_SearchSubscription = 0;
};
