#pragma once

#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <Foundation/Strings/String.h>

class plAudioMusicAssetDocument;
class plDocumentObject;
class plVariant;

/// \brief The edits the Audio Music window makes to the document, each one undo step.
///
/// Segments, states and stingers are found by their index in the document's arrays. States, transitions and the initial
/// state refer to segments and states by name: a rename renames those references in the same step, a delete removes
/// them. New elements get a name no other element of their kind has ("Explore A 2").
struct PL_EDITORPLUGINPLASMAAUDIO_DLL plAudioMusicEdit
{
  /// \brief The document object of the asset's properties.
  static const plDocumentObject* GetProperties(const plAudioMusicAssetDocument* pDocument);

  /// \brief The document object at uiIndex of one of the properties' arrays ("Segments", "States", "Transitions",
  /// "Stingers"), or of an element's array (a segment's "Stems"). Null when there is none.
  static const plDocumentObject* GetElement(const plAudioMusicAssetDocument* pDocument, const plDocumentObject* pParent, const char* szArray, plUInt32 uiIndex);
  static const plDocumentObject* GetElement(const plAudioMusicAssetDocument* pDocument, const char* szArray, plUInt32 uiIndex);

  /// \brief How long a clip plays, from its published asset info; 0 when the clip is unknown.
  static double GetClipSeconds(plStringView sClip);

  /// \brief The clip's asset name, or "missing clip".
  static plString GetClipName(plStringView sClip);

  /// \brief How many whole bars of fTempo (4 beats each) are closest to fSeconds, at least 1.
  static plUInt32 GetBarsForSeconds(double fSeconds, float fTempo, plUInt32 uiBeatsPerBar);

  /// \brief A new segment named after the clip with the clip as its stem, fTempo and the bars closest to the clip's
  /// length. It is inserted into the state uiState at uiPosition (its end with plInvalidIndex); plInvalidIndex as the
  /// state leaves it in no state. Returns the new segment's index.
  static plUInt32 AddSegmentFromClip(plAudioMusicAssetDocument* pDocument, plStringView sClip, float fTempo, plUInt32 uiState, plUInt32 uiPosition);

  /// \brief A new segment without stems ("Segment 3", 4 bars of fTempo), added to the end of the state uiState
  /// (plInvalidIndex: none). Returns its index.
  static plUInt32 AddEmptySegment(plAudioMusicAssetDocument* pDocument, float fTempo, plUInt32 uiState);

  /// \brief Removes the segment, from every state that plays it and every transition that plays it in between.
  static void DeleteSegment(plAudioMusicAssetDocument* pDocument, plUInt32 uiSegment);

  /// \brief Renames the segment and the states and transitions that refer to it. Fails silently on an empty or taken
  /// name.
  static void RenameSegment(plAudioMusicAssetDocument* pDocument, plUInt32 uiSegment, plStringView sName);

  /// \brief Toggles the exit cue after bar uiBar (1 = after the first bar).
  static void ToggleExitCue(plAudioMusicAssetDocument* pDocument, plUInt32 uiSegment, plUInt32 uiBar);

  /// \brief Sets Bars and drops the exit cues that no longer fall inside the segment.
  static void SetBars(plAudioMusicAssetDocument* pDocument, plUInt32 uiSegment, plUInt32 uiBars);

  /// \brief Adds the clip as a stem to the end of the segment's stems.
  static void AddStem(plAudioMusicAssetDocument* pDocument, plUInt32 uiSegment, plStringView sClip);
  static void RemoveStem(plAudioMusicAssetDocument* pDocument, plUInt32 uiSegment, plUInt32 uiStem);

  /// \brief A new state "State N" without segments. Returns its index.
  static plUInt32 AddState(plAudioMusicAssetDocument* pDocument);

  /// \brief Removes the state, the transitions from and to it, and clears the initial state when it was this one.
  static void DeleteState(plAudioMusicAssetDocument* pDocument, plUInt32 uiState);

  /// \brief Renames the state and the transitions and the initial state that refer to it. Fails silently on an empty or
  /// taken name.
  static void RenameState(plAudioMusicAssetDocument* pDocument, plUInt32 uiState, plStringView sName);

  /// \brief The state the music starts in.
  static void SetInitialState(plAudioMusicAssetDocument* pDocument, plUInt32 uiState);

  /// \brief Inserts the segment into the state's play order at uiPosition (its end with plInvalidIndex).
  static void InsertStateSegment(plAudioMusicAssetDocument* pDocument, plUInt32 uiState, plUInt32 uiPosition, plUInt32 uiSegment);

  /// \brief Removes the entry at uiPosition of the state's play order (the segment stays in the music).
  static void RemoveStateSegment(plAudioMusicAssetDocument* pDocument, plUInt32 uiState, plUInt32 uiPosition);

  /// \brief Moves the entry at uiFrom of the state's play order so it ends up at uiTo.
  static void MoveStateSegment(plAudioMusicAssetDocument* pDocument, plUInt32 uiState, plUInt32 uiFrom, plUInt32 uiTo);

  /// \brief The transition from uiFrom (plInvalidIndex: any state) to uiTo, or plInvalidIndex when the music has none.
  static plUInt32 FindTransition(const plAudioMusicAssetDocument* pDocument, plUInt32 uiFrom, plUInt32 uiTo);

  /// \brief A rule for the change from uiFrom (plInvalidIndex: any state) to uiTo: next bar, cut. Returns its index.
  static plUInt32 AddTransition(plAudioMusicAssetDocument* pDocument, plUInt32 uiFrom, plUInt32 uiTo);
  static void RemoveTransition(plAudioMusicAssetDocument* pDocument, plUInt32 uiTransition);

  /// \brief A new stinger "Stinger N" playing sClip (may be empty) on the next beat. Returns its index.
  static plUInt32 AddStinger(plAudioMusicAssetDocument* pDocument, plStringView sClip);
  static void DeleteStinger(plAudioMusicAssetDocument* pDocument, plUInt32 uiStinger);
  static void RenameStinger(plAudioMusicAssetDocument* pDocument, plUInt32 uiStinger, plStringView sName);

  /// \brief Sets one property of an element as its own undo step, when it differs.
  static void SetValue(plAudioMusicAssetDocument* pDocument, const plDocumentObject* pObject, const char* szProperty, const plVariant& value, const char* szTransaction);
};
