#pragma once

#include <Foundation/IO/Stream.h>
#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Strings/String.h>
#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

/// \brief Where on the beat grid a music change or a stinger may happen.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioMusicQuantize
{
  using StorageType = plUInt8;

  enum Enum : StorageType
  {
    Immediate = 0,    ///< as soon as it can be scheduled (one look-ahead span)
    NextBeat = 1,
    NextBar = 2,
    EndOfSegment = 3,
    ExitCue = 4,      ///< the next exit cue of the playing segment, or its end without one ahead

    Default = NextBar
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PLASMAAUDIOPLUGIN_DLL, plAudioMusicQuantize);

/// \brief Where the target of a music change starts.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioMusicStartAt
{
  using StorageType = plUInt8;

  enum Enum : StorageType
  {
    SegmentStart = 0, ///< at the beginning of the target state's first segment
    SamePosition = 1, ///< at the position the playing segment is at, so related stems stay in sync

    Default = SegmentStart
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_PLASMAAUDIOPLUGIN_DLL, plAudioMusicStartAt);

/// \brief A piece of music on a beat grid: bars of a tempo and meter, played by stems (instruments of the event).
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioMusicSegmentDesc
{
  plString m_sName;
  float m_fTempo = 120.0f;     ///< beats per minute
  plUInt8 m_uiBeatsPerBar = 4;
  plUInt16 m_uiBars = 4;

  /// \brief Bars after which a change set to plAudioMusicQuantize::ExitCue may leave (1 = after the first bar).
  plHybridArray<plUInt16, 4> m_ExitCues;

  /// \brief Indices into plAudioEventDesc::m_Instruments: the stems that play the segment, all from its start.
  plHybridArray<plUInt8, 4> m_Stems;

  PL_NODISCARD double GetBeatSeconds() const { return 60.0 / plMath::Max(static_cast<double>(m_fTempo), 1.0); }
  PL_NODISCARD plUInt32 GetBeatCount() const { return static_cast<plUInt32>(plMath::Max<plUInt8>(m_uiBeatsPerBar, 1)) * plMath::Max<plUInt16>(m_uiBars, 1); }
  PL_NODISCARD double GetLengthSeconds() const { return GetBeatSeconds() * GetBeatCount(); }
};

/// \brief A state of the music: the segments that play while the game asks for it, in order, then again from the first.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioMusicStateDesc
{
  plString m_sName;
  plHybridArray<plUInt16, 4> m_Segments; ///< indices into plAudioMusicDesc::m_Segments
};

/// \brief How the music changes from one state to another.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioMusicTransitionDesc
{
  static constexpr plUInt16 AnyState = 0xFFFF;
  static constexpr plUInt16 NoSegment = 0xFFFF;

  plUInt16 m_uiFrom = AnyState; ///< index into plAudioMusicDesc::m_States, or AnyState
  plUInt16 m_uiTo = 0;
  plAudioMusicQuantize::Enum m_eChangeAt = plAudioMusicQuantize::Default;

  /// \brief The playing segment fades out over this many beats from the change on; 0 cuts it at the change.
  float m_fFadeBeats = 0.0f;

  /// \brief A segment that plays between the two states (an intro or an outro), or NoSegment.
  plUInt16 m_uiVia = NoSegment;

  plAudioMusicStartAt::Enum m_eStartAt = plAudioMusicStartAt::Default;
};

/// \brief A short phrase played over the music, on the beat grid.
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioMusicStingerDesc
{
  plString m_sName;
  plUInt8 m_uiInstrument = 0; ///< index into plAudioEventDesc::m_Instruments
  plAudioMusicQuantize::Enum m_eAt = plAudioMusicQuantize::NextBeat;
};

/// \brief The music section of an audio event: an event with states plays them as interactive music.
///
/// The event's instruments are the stems of the segments and the stingers; the music decides when each starts and ends
/// instead of the instruments' start delays. The event parameter named StateParameter selects the state (its value is
/// the state's index); the game can also set a state or play a stinger by name (see the reserved parameters of the
/// audio system).
struct PL_PLASMAAUDIOPLUGIN_DLL plAudioMusicDesc
{
  static constexpr const char* StateParameter = "State";

  plDynamicArray<plAudioMusicSegmentDesc> m_Segments;
  plDynamicArray<plAudioMusicStateDesc> m_States;
  plDynamicArray<plAudioMusicTransitionDesc> m_Transitions;
  plDynamicArray<plAudioMusicStingerDesc> m_Stingers;

  /// \brief Whether the game object of the entity gets a message on every beat.
  bool m_bBeatMessages = false;

  PL_NODISCARD bool IsMusic() const { return !m_States.IsEmpty() && !m_Segments.IsEmpty(); }

  /// \brief The transition from one state to another: the one set for the pair, else one from any state, else a change
  /// on the next bar that cuts the playing segment.
  PL_NODISCARD plAudioMusicTransitionDesc FindTransition(plUInt16 uiFrom, plUInt16 uiTo) const;

  PL_NODISCARD plUInt32 FindState(plStringView sName) const;
  PL_NODISCARD plUInt32 FindStinger(plStringView sName) const;

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);
};
