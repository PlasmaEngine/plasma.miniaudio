#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/EventAsset/AudioEventAsset.h>
#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicAsset.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMusicStem, 1, plRTTIDefaultAllocator<plAudioMusicStem>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Clip", m_sClip)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Audio_Clip", plDependencyFlags::Transform | plDependencyFlags::Package)),
    PL_MEMBER_PROPERTY("Volume", m_fVolumeDb)->AddAttributes(new plClampValueAttribute(-60.0f, 12.0f), new plSuffixAttribute(" dB")),
    PL_MEMBER_PROPERTY("FollowIntensity", m_bFollowIntensity),
    PL_MEMBER_PROPERTY("IntensityMin", m_fIntensityMin)->AddAttributes(new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("IntensityMax", m_fIntensityMax)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("IntensityFade", m_fIntensityFade)->AddAttributes(new plDefaultValueAttribute(0.25f), new plClampValueAttribute(0.0f, 1.0f)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMusicSegment, 1, plRTTIDefaultAllocator<plAudioMusicSegment>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_MEMBER_PROPERTY("Tempo", m_fTempo)->AddAttributes(new plDefaultValueAttribute(120.0f), new plClampValueAttribute(20.0f, 400.0f), new plSuffixAttribute(" BPM")),
    PL_MEMBER_PROPERTY("BeatsPerBar", m_uiBeatsPerBar)->AddAttributes(new plDefaultValueAttribute(4), new plClampValueAttribute(1, 16)),
    PL_MEMBER_PROPERTY("Bars", m_uiBars)->AddAttributes(new plDefaultValueAttribute(4), new plClampValueAttribute(1, 512)),
    PL_ARRAY_MEMBER_PROPERTY("ExitCues", m_ExitCues),
    PL_ARRAY_MEMBER_PROPERTY("Stems", m_Stems),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMusicState, 1, plRTTIDefaultAllocator<plAudioMusicState>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_ARRAY_MEMBER_PROPERTY("Segments", m_Segments)->AddAttributes(new plDynamicStringEnumAttribute("AudioMusic.Segments")),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMusicTransition, 1, plRTTIDefaultAllocator<plAudioMusicTransition>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("From", m_sFrom)->AddAttributes(new plDynamicStringEnumAttribute("AudioMusic.States")),
    PL_MEMBER_PROPERTY("To", m_sTo)->AddAttributes(new plDynamicStringEnumAttribute("AudioMusic.States")),
    PL_ENUM_MEMBER_PROPERTY("ChangeAt", plAudioMusicQuantize, m_eChangeAt),
    PL_MEMBER_PROPERTY("FadeBeats", m_fFadeBeats)->AddAttributes(new plClampValueAttribute(0.0f, 64.0f), new plSuffixAttribute(" beats")),
    PL_MEMBER_PROPERTY("Via", m_sVia)->AddAttributes(new plDynamicStringEnumAttribute("AudioMusic.Segments")),
    PL_ENUM_MEMBER_PROPERTY("StartAt", plAudioMusicStartAt, m_eStartAt),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMusicStinger, 1, plRTTIDefaultAllocator<plAudioMusicStinger>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_MEMBER_PROPERTY("Clip", m_sClip)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Audio_Clip", plDependencyFlags::Transform | plDependencyFlags::Package)),
    PL_MEMBER_PROPERTY("Volume", m_fVolumeDb)->AddAttributes(new plClampValueAttribute(-60.0f, 12.0f), new plSuffixAttribute(" dB")),
    PL_ENUM_MEMBER_PROPERTY("At", plAudioMusicQuantize, m_eAt)->AddAttributes(new plDefaultValueAttribute(plAudioMusicQuantize::NextBeat)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMusicAssetProperties, 1, plRTTIDefaultAllocator<plAudioMusicAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("OutputBus", m_sOutputBus)->AddAttributes(new plGroupAttribute("Routing"), new plDynamicStringEnumAttribute("AudioSystem.OutputBus")),
    PL_MEMBER_PROPERTY("Priority", m_uiPriority)->AddAttributes(new plDefaultValueAttribute(200), new plClampValueAttribute(0, 255)),
    PL_MEMBER_PROPERTY("FadeIn", m_fFadeIn)->AddAttributes(new plGroupAttribute("Lifetime"), new plClampValueAttribute(0.0f, 60.0f), new plSuffixAttribute(" sec")),
    PL_MEMBER_PROPERTY("StopFadeOut", m_fStopFadeOut)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 60.0f), new plSuffixAttribute(" sec")),
    PL_MEMBER_PROPERTY("InitialState", m_sInitialState)->AddAttributes(new plGroupAttribute("Music"), new plDynamicStringEnumAttribute("AudioMusic.States")),
    PL_MEMBER_PROPERTY("BeatMessages", m_bBeatMessages),
    PL_MEMBER_PROPERTY("Intensity", m_fIntensity)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("IntensitySeek", m_fIntensitySeek)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, plVariant()), new plSuffixAttribute(" /sec")),
    PL_ARRAY_MEMBER_PROPERTY("Segments", m_Segments)->AddAttributes(new plGroupAttribute("Content")),
    PL_ARRAY_MEMBER_PROPERTY("States", m_States),
    PL_ARRAY_MEMBER_PROPERTY("Transitions", m_Transitions),
    PL_ARRAY_MEMBER_PROPERTY("Stingers", m_Stingers),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioMusicAssetDocument, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

void plAudioMusicAssetProperties::PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  if (e.m_pObject->GetTypeAccessor().GetType() != plGetStaticRTTI<plAudioMusicStem>())
    return;

  // the range only matters for a stem that follows Intensity
  const bool bFollow = e.m_pObject->GetTypeAccessor().GetValue("FollowIntensity").ConvertTo<bool>();
  auto& props = *e.m_pPropertyStates;
  props["IntensityMin"].m_Visibility = bFollow ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  props["IntensityMax"].m_Visibility = bFollow ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  props["IntensityFade"].m_Visibility = bFollow ? plPropertyUiState::Default : plPropertyUiState::Invisible;
}

plAudioMusicAssetDocument::plAudioMusicAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plAudioMusicAssetProperties>(sDocumentPath, plAssetDocEngineConnection::Simple)
{
}

void plAudioMusicAssetDocument::HandleEngineMessage(const plEditorEngineDocumentMsg* pMsg)
{
  if (const auto* pStatus = plDynamicCast<const plAudioAuditionStatusMsgToEditor*>(pMsg))
  {
    m_AuditionStatus = *pStatus;
    m_AuditionEvents.Broadcast(m_AuditionStatus);
    return;
  }

  plSimpleAssetDocument<plAudioMusicAssetProperties>::HandleEngineMessage(pMsg);
}

void plAudioMusicAssetDocument::UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const
{
  SUPER::UpdateAssetDocumentInfo(pInfo);

  // the parameters a trigger component can set, like an event's
  plAudioEventAssetMetaInfo* pMeta = PL_DEFAULT_NEW(plAudioEventAssetMetaInfo);
  pMeta->m_Parameters.PushBack(plAudioMusicDesc::StateParameter);
  pMeta->m_Parameters.PushBack(IntensityParameter);
  pMeta->m_bLoops = true;
  pInfo->m_MetaInfo.PushBack(pMeta);
}

plTransformStatus plAudioMusicAssetDocument::InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  plAudioEventDesc desc;
  PL_SUCCEED_OR_RETURN(BuildEventDesc(desc));

  // the event resource's data: the description, then its embedded samples (music has none)
  PL_SUCCEED_OR_RETURN(desc.Save(stream));
  stream << desc.m_SampleNames.GetCount();
  return plStatus(PL_SUCCESS);
}

plStatus plAudioMusicAssetDocument::WriteAuditionData(plStreamWriter& inout_stream, const plAudioEventDesc& desc) const
{
  // what an event resource file holds: its path, the asset file header, then the event
  plStringBuilder sPath = GetDocumentPath();
  sPath.ChangeFileExtension("plAudioEvent");
  inout_stream << sPath;

  plAssetFileHeader header;
  header.SetFileHashAndVersion(plAssetCurator::GetSingleton()->GetAssetTransformHash(GetGuid()), GetAssetTypeVersion());
  PL_SUCCEED_OR_RETURN(header.Write(inout_stream));

  PL_SUCCEED_OR_RETURN(desc.Save(inout_stream));
  inout_stream << desc.m_SampleNames.GetCount();
  return plStatus(PL_SUCCESS);
}

namespace
{
  plUInt32 FindByName(plArrayPtr<const plAudioMusicSegment> segments, plStringView sName)
  {
    for (plUInt32 i = 0; i < segments.GetCount(); ++i)
    {
      if (segments[i].m_sName.IsEqual_NoCase(sName))
        return i;
    }
    return plInvalidIndex;
  }

  plUInt32 FindByName(plArrayPtr<const plAudioMusicState> states, plStringView sName)
  {
    for (plUInt32 i = 0; i < states.GetCount(); ++i)
    {
      if (states[i].m_sName.IsEqual_NoCase(sName))
        return i;
    }
    return plInvalidIndex;
  }

  plAudioEventInstrumentDesc& AddClipInstrument(plAudioEventDesc& ref_desc, plStringView sName, plStringView sClip, float fVolumeDb)
  {
    plAudioEventInstrumentDesc& instrument = ref_desc.m_Instruments.ExpandAndGetRef();
    instrument.m_sName = sName;
    instrument.m_ePlaylistMode = plAudioEventPlaylistMode::Sequential;
    instrument.m_fVolumeDb = fVolumeDb;

    plAudioEventPlaylistEntryDesc& entry = instrument.m_Playlist.ExpandAndGetRef();
    entry.m_sClip = sClip;
    return instrument;
  }
} // namespace

plStatus plAudioMusicAssetDocument::BuildEventDesc(plAudioEventDesc& out_desc) const
{
  const plAudioMusicAssetProperties* pProp = GetProperties();

  if (pProp->m_Segments.IsEmpty())
    return plStatus("The music has no segments.");

  if (pProp->m_States.IsEmpty())
    return plStatus("The music has no states.");

  plAudioEventDesc& desc = out_desc;
  desc = plAudioEventDesc();
  desc.m_sOutputBus = pProp->m_sOutputBus;
  desc.m_uiMaxInstances = 1;
  desc.m_eStealing = plAudioEventStealingMode::Oldest;
  desc.m_uiPriority = pProp->m_uiPriority;
  desc.m_eVirtualMode = plAudioEventVirtualMode::Resume;
  desc.m_FadeIn = plTime::MakeFromSeconds(pProp->m_fFadeIn);
  desc.m_StopFadeOut = plTime::MakeFromSeconds(pProp->m_fStopFadeOut);
  desc.m_bSpatialize = false;

  plAudioMusicDesc& music = desc.m_Music;
  music.m_bBeatMessages = pProp->m_bBeatMessages;

  // the parameters: the state (by index) and, when a stem follows it, the intensity
  plAudioEventParameterDesc& state = desc.m_Parameters.ExpandAndGetRef();
  state.m_sName = plAudioMusicDesc::StateParameter;
  state.m_fMin = 0.0f;
  state.m_fMax = static_cast<float>(pProp->m_States.GetCount() - 1);

  if (!pProp->m_sInitialState.IsEmpty())
  {
    const plUInt32 uiInitial = FindByName(pProp->m_States, pProp->m_sInitialState);
    if (uiInitial == plInvalidIndex)
      return plStatus(plFmt("The initial state '{}' is not a state of the music.", pProp->m_sInitialState));
    state.m_fDefault = static_cast<float>(uiInitial);
  }

  bool bIntensity = false;
  for (const plAudioMusicSegment& segment : pProp->m_Segments)
  {
    for (const plAudioMusicStem& stem : segment.m_Stems)
      bIntensity |= stem.m_bFollowIntensity;
  }

  const plUInt8 uiIntensity = static_cast<plUInt8>(desc.m_Parameters.GetCount());
  if (bIntensity)
  {
    plAudioEventParameterDesc& intensity = desc.m_Parameters.ExpandAndGetRef();
    intensity.m_sName = IntensityParameter;
    intensity.m_fMin = 0.0f;
    intensity.m_fMax = 1.0f;
    intensity.m_fDefault = plMath::Clamp(pProp->m_fIntensity, 0.0f, 1.0f);
    intensity.m_fSeekSpeed = plMath::Max(pProp->m_fIntensitySeek, 0.0f);
  }

  // every stem of every segment is an instrument; the music starts and ends it
  for (const plAudioMusicSegment& segment : pProp->m_Segments)
  {
    if (segment.m_sName.IsEmpty())
      return plStatus("A segment of the music has no name.");

    plAudioMusicSegmentDesc& segmentDesc = music.m_Segments.ExpandAndGetRef();
    segmentDesc.m_sName = segment.m_sName;
    segmentDesc.m_fTempo = plMath::Clamp(segment.m_fTempo, 20.0f, 400.0f);
    segmentDesc.m_uiBeatsPerBar = static_cast<plUInt8>(plMath::Clamp<plUInt32>(segment.m_uiBeatsPerBar, 1, 16));
    segmentDesc.m_uiBars = static_cast<plUInt16>(plMath::Clamp<plUInt32>(segment.m_uiBars, 1, 512));

    for (plUInt32 uiCue : segment.m_ExitCues)
    {
      if (uiCue >= 1 && uiCue < segmentDesc.m_uiBars)
        segmentDesc.m_ExitCues.PushBack(static_cast<plUInt16>(uiCue));
    }
    segmentDesc.m_ExitCues.Sort();

    for (plUInt32 i = 0; i < segment.m_Stems.GetCount(); ++i)
    {
      const plAudioMusicStem& stem = segment.m_Stems[i];
      if (stem.m_sClip.IsEmpty())
        continue;

      if (desc.m_Instruments.GetCount() >= 255)
        return plStatus("The music has more than 255 stems and stingers.");

      plStringBuilder sName;
      sName.SetFormat("{} {}", segment.m_sName, i + 1);

      plAudioEventInstrumentDesc& instrument = AddClipInstrument(desc, sName, stem.m_sClip, stem.m_fVolumeDb);
      if (stem.m_bFollowIntensity)
      {
        instrument.m_uiRangeParameter = uiIntensity;
        instrument.m_fRangeMin = plMath::Min(stem.m_fIntensityMin, stem.m_fIntensityMax);
        instrument.m_fRangeMax = plMath::Max(stem.m_fIntensityMin, stem.m_fIntensityMax);
        instrument.m_fRangeFade = plMath::Max(stem.m_fIntensityFade, 0.0f);
      }

      segmentDesc.m_Stems.PushBack(static_cast<plUInt8>(desc.m_Instruments.GetCount() - 1));
    }
  }

  for (const plAudioMusicState& stateProp : pProp->m_States)
  {
    if (stateProp.m_sName.IsEmpty())
      return plStatus("A state of the music has no name.");

    plAudioMusicStateDesc& stateDesc = music.m_States.ExpandAndGetRef();
    stateDesc.m_sName = stateProp.m_sName;

    for (const plString& sSegment : stateProp.m_Segments)
    {
      const plUInt32 uiSegment = FindByName(pProp->m_Segments, sSegment);
      if (uiSegment == plInvalidIndex)
        return plStatus(plFmt("The state '{}' plays '{}', which is not a segment of the music.", stateProp.m_sName, sSegment));

      stateDesc.m_Segments.PushBack(static_cast<plUInt16>(uiSegment));
    }
  }

  for (const plAudioMusicTransition& transition : pProp->m_Transitions)
  {
    plAudioMusicTransitionDesc& transitionDesc = music.m_Transitions.ExpandAndGetRef();

    if (!transition.m_sFrom.IsEmpty())
    {
      const plUInt32 uiFrom = FindByName(pProp->m_States, transition.m_sFrom);
      if (uiFrom == plInvalidIndex)
        return plStatus(plFmt("A transition starts from '{}', which is not a state of the music.", transition.m_sFrom));
      transitionDesc.m_uiFrom = static_cast<plUInt16>(uiFrom);
    }

    const plUInt32 uiTo = FindByName(pProp->m_States, transition.m_sTo);
    if (uiTo == plInvalidIndex)
      return plStatus(plFmt("A transition goes to '{}', which is not a state of the music.", transition.m_sTo));
    transitionDesc.m_uiTo = static_cast<plUInt16>(uiTo);

    if (!transition.m_sVia.IsEmpty())
    {
      const plUInt32 uiVia = FindByName(pProp->m_Segments, transition.m_sVia);
      if (uiVia == plInvalidIndex)
        return plStatus(plFmt("A transition plays '{}' in between, which is not a segment of the music.", transition.m_sVia));
      transitionDesc.m_uiVia = static_cast<plUInt16>(uiVia);
    }

    transitionDesc.m_eChangeAt = static_cast<plAudioMusicQuantize::Enum>(transition.m_eChangeAt.GetValue());
    transitionDesc.m_fFadeBeats = plMath::Max(transition.m_fFadeBeats, 0.0f);
    transitionDesc.m_eStartAt = static_cast<plAudioMusicStartAt::Enum>(transition.m_eStartAt.GetValue());
  }

  for (const plAudioMusicStinger& stinger : pProp->m_Stingers)
  {
    if (stinger.m_sName.IsEmpty() || stinger.m_sClip.IsEmpty())
      continue;

    if (desc.m_Instruments.GetCount() >= 255)
      return plStatus("The music has more than 255 stems and stingers.");

    plStringBuilder sName("Stinger ", stinger.m_sName);
    AddClipInstrument(desc, sName, stinger.m_sClip, stinger.m_fVolumeDb);

    plAudioMusicStingerDesc& stingerDesc = music.m_Stingers.ExpandAndGetRef();
    stingerDesc.m_sName = stinger.m_sName;
    stingerDesc.m_uiInstrument = static_cast<plUInt8>(desc.m_Instruments.GetCount() - 1);
    stingerDesc.m_eAt = static_cast<plAudioMusicQuantize::Enum>(stinger.m_eAt.GetValue());
  }

  return plStatus(PL_SUCCESS);
}
