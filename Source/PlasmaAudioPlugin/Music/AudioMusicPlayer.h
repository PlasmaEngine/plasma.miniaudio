#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HybridArray.h>
#include <PlasmaAudioPlugin/Music/AudioMusicDesc.h>

/// \brief Schedules interactive music on engine frames: which segment plays when, where a change of state lands on the
/// beat grid, and when stingers start.
///
/// The player knows nothing of voices. Every Update it places what starts before the horizon and reports it as stem
/// plays (an instrument from a frame to a frame, optionally from a position inside it) and stem cuts (an instrument
/// that is playing ends at a frame, with a fade). Segments follow each other gaplessly: a segment ends on the frame of
/// its last beat boundary and the next one starts there, each on a grid anchored at its own start, so the tempo never
/// drifts.
///
/// A change of state is decided once its landing frame is known (the quantized boundary at or after the horizon of the
/// update that asks for it) and committed by the update whose horizon reaches it: the playing segment is cut there (or
/// fades out over the transition's beats from there), the transition's segment and then the target state's first
/// segment are placed from there. Asking again before the commit replaces the change; asking for the state the music
/// already heads to drops it.
///
/// Messages (transitions, stingers and, when the desc asks for them, beats) carry their frame and are handed out once
/// the engine time reached it.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioMusicPlayer
{
public:
  static constexpr plUInt16 NoState = 0xFFFF;

  struct StemPlay
  {
    plUInt8 m_uiInstrument = 0;
    plUInt64 m_uiStartFrame = 0;
    plUInt64 m_uiOffsetFrames = 0; ///< engine frames into the stem at which it starts (a change to the same position)
    plUInt64 m_uiEndFrame = 0;     ///< the segment's end; 0 plays the whole stem (stingers)
  };

  struct StemCut
  {
    plUInt8 m_uiInstrument = 0;
    plUInt64 m_uiFrame = 0;      ///< where the stem stops sounding: the change plus the fade
    plUInt64 m_uiFadeFrames = 0; ///< the stem fades to silence over this many frames before m_uiFrame
  };

  struct Message
  {
    enum class Type : plUInt8
    {
      Beat,
      Transition,
      Stinger,
    };

    plUInt64 m_uiFrame = 0;
    Type m_Type = Type::Beat;
    plString m_sText;     ///< transition "Explore -> Combat", the stinger's name, or the beat's segment
    plUInt16 m_uiBar = 0; ///< beats: bar within the segment, from 1
    plUInt8 m_uiBeat = 0; ///< beats: beat within the bar, from 1
  };

  /// \brief What plays at a frame, for the editor and tests.
  struct Position
  {
    plUInt16 m_uiState = NoState;  ///< the state the playing segment belongs to (a transition segment: the target)
    plUInt16 m_uiSegment = NoState;
    bool m_bTransitionSegment = false;
    plUInt16 m_uiBar = 0;          ///< from 1
    plUInt8 m_uiBeat = 0;          ///< from 1
    float m_fTempo = 0.0f;
    plUInt8 m_uiBeatsPerBar = 0;
    plUInt16 m_uiPendingState = NoState; ///< a change that was asked for and has not landed yet
    plUInt64 m_uiPendingFrame = 0;       ///< where it lands; 0 while not decided
  };

  void Reset();

  /// \brief Places the first segment of uiState at uiStartFrame. pDesc must stay valid until Reset.
  void Start(const plAudioMusicDesc* pDesc, plUInt64 uiStartFrame, plUInt32 uiSampleRate, plUInt16 uiState);

  PL_NODISCARD bool IsStarted() const { return m_pDesc != nullptr; }

  /// \brief The game asks for a state; it changes at the transition's quantized point.
  void RequestState(plUInt16 uiState);

  /// \brief The game asks for a stinger; it starts at its quantized point.
  void RequestStinger(plUInt32 uiStinger);

  /// \brief The state the music plays or heads to (after a committed change), and the one the game asked for last.
  PL_NODISCARD plUInt16 GetTargetState() const { return m_uiTargetState; }
  PL_NODISCARD plUInt16 GetRequestedState() const { return m_uiRequestedState; }

  /// \brief Places what starts at or before uiHorizon and appends the stem plays and cuts it decided.
  void Update(plUInt64 uiNow, plUInt64 uiHorizon, plDynamicArray<StemPlay>& out_plays, plDynamicArray<StemCut>& out_cuts);

  /// \brief Appends the messages whose frame the engine time reached (frame <= uiNow), in frame order.
  void TakeMessages(plUInt64 uiNow, plDynamicArray<Message>& out_messages);

  PL_NODISCARD Position GetPosition(plUInt64 uiFrame) const;

private:
  /// A segment placed on the timeline.
  struct Placed
  {
    plUInt16 m_uiSegment = 0;
    plUInt16 m_uiState = 0;          ///< the state it belongs to; the target for a transition segment
    plUInt16 m_uiCycle = 0;          ///< index into the state's segment list
    bool m_bTransition = false;      ///< a transition's segment: the target state's first segment follows
    double m_fGridOrigin = 0.0;      ///< frame of beat 0 (before m_uiStart when the segment starts at a later position)
    plUInt64 m_uiStart = 0;
    plUInt64 m_uiEnd = 0;            ///< natural end: the last beat boundary
    plUInt64 m_uiMusicalEnd = 0;     ///< where the next music starts: m_uiEnd, or the frame of a change that cut it
    plUInt64 m_uiBeatsDelivered = 0; ///< beat messages: beats from the grid origin handed out so far
  };

  struct PendingChange
  {
    plUInt16 m_uiTo = NoState;
    plUInt64 m_uiFrame = 0;
    plAudioMusicTransitionDesc m_Transition;
  };

  struct PendingStinger
  {
    plUInt32 m_uiStinger = 0;
    plUInt64 m_uiFrame = 0; ///< 0 while not decided
  };

  PL_NODISCARD double GetBeatFrames(const plAudioMusicSegmentDesc& segment) const;
  PL_NODISCARD plUInt64 GetFrameOfBeat(const Placed& placed, double fBeats) const;

  /// The placed segment whose musical span holds the frame, else the last one.
  PL_NODISCARD const Placed* GetPlacedAt(plUInt64 uiFrame) const;

  /// The first frame at or after uiEarliest the quantization allows, on the grid of the segment that plays there.
  PL_NODISCARD plUInt64 GetLanding(plAudioMusicQuantize::Enum quantize, plUInt64 uiEarliest) const;

  /// Places a segment and reports its stem plays.
  void Place(plUInt16 uiSegment, plUInt16 uiState, plUInt16 uiCycle, bool bTransition, plUInt64 uiStart, plUInt64 uiOffset, plDynamicArray<StemPlay>& out_plays);

  /// Places what follows the last placed segment: the state's next segment, or after a transition segment the target's first.
  void PlaceNext(plDynamicArray<StemPlay>& out_plays);

  void CommitChange(plDynamicArray<StemPlay>& out_plays, plDynamicArray<StemCut>& out_cuts);

  void AddMessage(plUInt64 uiFrame, Message::Type type, plStringView sText, plUInt16 uiBar = 0, plUInt8 uiBeat = 0);

  /// Adds the beat messages of the placed segments up to uiFrame.
  void AddBeatMessages(plUInt64 uiFrame);

  const plAudioMusicDesc* m_pDesc = nullptr;
  plUInt32 m_uiSampleRate = 48000;
  plUInt64 m_uiStartFrame = 0; ///< where the first segment starts
  plUInt64 m_uiHorizon = 0;

  plHybridArray<Placed, 4> m_Placed; ///< in time order
  plUInt16 m_uiTargetState = NoState;
  plUInt16 m_uiRequestedState = NoState;

  bool m_bChangePending = false;
  PendingChange m_Change;

  plHybridArray<PendingStinger, 2> m_Stingers;
  plDynamicArray<Message> m_Messages; ///< in frame order
};
