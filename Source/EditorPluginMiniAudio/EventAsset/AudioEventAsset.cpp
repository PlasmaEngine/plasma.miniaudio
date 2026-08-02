#include <EditorPluginMiniAudio/EditorPluginMiniAudioPCH.h>

#include <EditorPluginMiniAudio/EventAsset/AudioEventAsset.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioEventPlaylistEntry, 1, plRTTIDefaultAllocator<plAudioEventPlaylistEntry>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("File", m_sFile)->AddAttributes(new plFileBrowserAttribute("Select Sound", "*.wav;*.mp3")),
    PL_MEMBER_PROPERTY("Weight", m_fWeight)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 100.0f)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioEventInstrument, 1, plRTTIDefaultAllocator<plAudioEventInstrument>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_MEMBER_PROPERTY("StartDelay", m_fStartDelay)->AddAttributes(new plClampValueAttribute(0.0f, 300.0f), new plSuffixAttribute(" sec")),
    PL_ENUM_MEMBER_PROPERTY("PlaylistMode", plAudioEventPlaylistMode, m_ePlaylistMode),
    PL_MEMBER_PROPERTY("Loop", m_bLoop),
    PL_MEMBER_PROPERTY("Mute", m_bMute),
    PL_MEMBER_PROPERTY("SwitchState", m_sSwitchState)->AddAttributes(new plDynamicStringEnumAttribute("AudioSystem.SwitchState")),
    PL_MEMBER_PROPERTY("MinRetriggerDelay", m_fMinRetriggerDelay)->AddAttributes(new plClampValueAttribute(0.0f, 300.0f), new plSuffixAttribute(" sec")),
    PL_MEMBER_PROPERTY("MaxRetriggerDelay", m_fMaxRetriggerDelay)->AddAttributes(new plClampValueAttribute(0.0f, 300.0f), new plSuffixAttribute(" sec")),
    PL_MEMBER_PROPERTY("Volume", m_fVolumeDb)->AddAttributes(new plClampValueAttribute(-60.0f, 12.0f), new plSuffixAttribute(" dB")),
    PL_MEMBER_PROPERTY("VolumeRandomization", m_fVolumeRandDb)->AddAttributes(new plClampValueAttribute(0.0f, 24.0f), new plSuffixAttribute(" dB")),
    PL_MEMBER_PROPERTY("Pitch", m_fPitchSemitones)->AddAttributes(new plClampValueAttribute(-24.0f, 24.0f), new plSuffixAttribute(" st")),
    PL_MEMBER_PROPERTY("PitchRandomization", m_fPitchRandSemitones)->AddAttributes(new plClampValueAttribute(0.0f, 24.0f), new plSuffixAttribute(" st")),
    PL_ARRAY_MEMBER_PROPERTY("Playlist", m_Playlist),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioEventParameter, 1, plRTTIDefaultAllocator<plAudioEventParameter>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Name", m_sName),
    PL_MEMBER_PROPERTY("Min", m_fMin),
    PL_MEMBER_PROPERTY("Max", m_fMax)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_MEMBER_PROPERTY("Default", m_fDefault),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioEventCurvePoint, 1, plRTTIDefaultAllocator<plAudioEventCurvePoint>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("X", m_fX),
    PL_MEMBER_PROPERTY("Y", m_fY)->AddAttributes(new plDefaultValueAttribute(1.0f)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioEventAutomation, 1, plRTTIDefaultAllocator<plAudioEventAutomation>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Parameter", m_sParameter),
    PL_ENUM_MEMBER_PROPERTY("Target", plAudioEventAutomationTarget, m_eTarget),
    PL_ARRAY_MEMBER_PROPERTY("Points", m_Points),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioEventAssetProperties, 1, plRTTIDefaultAllocator<plAudioEventAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("OutputBus", m_sOutputBus)->AddAttributes(new plGroupAttribute("Routing"), new plDynamicStringEnumAttribute("AudioSystem.OutputBus")),
    PL_MEMBER_PROPERTY("MaxInstances", m_uiMaxInstances)->AddAttributes(new plGroupAttribute("Polyphony"), new plDefaultValueAttribute(8), new plClampValueAttribute(0, 64)),
    PL_ENUM_MEMBER_PROPERTY("Stealing", plAudioEventStealingMode, m_eStealing),
    PL_MEMBER_PROPERTY("Cooldown", m_fCooldown)->AddAttributes(new plGroupAttribute("Lifetime"), new plClampValueAttribute(0.0f, 60.0f), new plSuffixAttribute(" sec")),
    PL_MEMBER_PROPERTY("FadeIn", m_fFadeIn)->AddAttributes(new plClampValueAttribute(0.0f, 60.0f), new plSuffixAttribute(" sec")),
    PL_MEMBER_PROPERTY("StopFadeOut", m_fStopFadeOut)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.0f, 60.0f), new plSuffixAttribute(" sec")),
    PL_MEMBER_PROPERTY("LoopRegion", m_bLoopRegion),
    PL_MEMBER_PROPERTY("LoopStart", m_fLoopStart)->AddAttributes(new plClampValueAttribute(0.0f, 600.0f), new plSuffixAttribute(" sec")),
    PL_MEMBER_PROPERTY("LoopEnd", m_fLoopEnd)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 600.0f), new plSuffixAttribute(" sec")),
    PL_MEMBER_PROPERTY("Spatialize", m_bSpatialize)->AddAttributes(new plGroupAttribute("Spatializer"), new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("MinDistance", m_fMinDistance)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.01f, 10000.0f)),
    PL_MEMBER_PROPERTY("MaxDistance", m_fMaxDistance)->AddAttributes(new plDefaultValueAttribute(100.0f), new plClampValueAttribute(0.01f, 10000.0f)),
    PL_MEMBER_PROPERTY("Rolloff", m_fRolloff)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.001f, 1000.0f), new plAudioRolloffCurveWidgetAttribute()),
    PL_MEMBER_PROPERTY("DopplerFactor", m_fDopplerFactor)->AddAttributes(new plClampValueAttribute(0.0f, 10.0f)),
    // the arrays are edited through the Event Timeline window (timeline, tab strip, deck);
    // they are hidden in property grids via PropertyMetaStateEventHandler
    PL_ARRAY_MEMBER_PROPERTY("Instruments", m_Instruments),
    PL_ARRAY_MEMBER_PROPERTY("Parameters", m_Parameters),
    PL_ARRAY_MEMBER_PROPERTY("Automations", m_Automations),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioEventAssetMetaInfo, 1, plRTTIDefaultAllocator<plAudioEventAssetMetaInfo>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ARRAY_MEMBER_PROPERTY("Parameters", m_Parameters),
    PL_ARRAY_MEMBER_PROPERTY("SwitchStates", m_SwitchStates),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioEventAssetDocument, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

void plAudioEventAssetProperties::PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plAudioEventAssetProperties>())
  {
    auto& props = *e.m_pPropertyStates;

    const bool bSpatialize = e.m_pObject->GetTypeAccessor().GetValue("Spatialize").ConvertTo<bool>();

    props["MinDistance"].m_Visibility = bSpatialize ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["MaxDistance"].m_Visibility = bSpatialize ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["Rolloff"].m_Visibility = bSpatialize ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["DopplerFactor"].m_Visibility = bSpatialize ? plPropertyUiState::Default : plPropertyUiState::Invisible;

    // these arrays are edited through the Audio Event window (instrument dropdown, tab strip,
    // playlist panel) — showing them in the grid duplicates the whole document into the macros dock
    props["Instruments"].m_Visibility = plPropertyUiState::Invisible;
    props["Parameters"].m_Visibility = plPropertyUiState::Invisible;
    props["Automations"].m_Visibility = plPropertyUiState::Invisible;

    // the loop window only matters when the region is enabled (drag it on the timeline's logic lane)
    const bool bLoopRegion = e.m_pObject->GetTypeAccessor().GetValue("LoopRegion").ConvertTo<bool>();
    props["LoopStart"].m_Visibility = bLoopRegion ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["LoopEnd"].m_Visibility = bLoopRegion ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  }

  if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plAudioEventInstrument>())
  {
    auto& props = *e.m_pPropertyStates;

    // the playlist is edited through the window's playlist panel (rows with weights + add row)
    props["Playlist"].m_Visibility = plPropertyUiState::Invisible;
  }
}

plAudioEventAssetDocument::plAudioEventAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plAudioEventAssetProperties>(sDocumentPath, plAssetDocEngineConnection::None)
{
}

void plAudioEventAssetDocument::UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const
{
  SUPER::UpdateAssetDocumentInfo(pInfo);

  const plAudioEventAssetProperties* pProp = GetProperties();

  for (const auto& instrument : pProp->m_Instruments)
  {
    for (const auto& entry : instrument.m_Playlist)
    {
      if (!entry.m_sFile.IsEmpty())
      {
        pInfo->m_TransformDependencies.Insert(entry.m_sFile);
      }
    }
  }

  // publish the names to the asset curator, so component dropdowns can list them
  // without this document being open
  plAudioEventAssetMetaInfo* pMeta = PL_DEFAULT_NEW(plAudioEventAssetMetaInfo);

  for (const auto& parameter : pProp->m_Parameters)
  {
    if (!parameter.m_sName.IsEmpty())
      pMeta->m_Parameters.PushBack(parameter.m_sName);
  }

  for (const auto& instrument : pProp->m_Instruments)
  {
    if (!instrument.m_sSwitchState.IsEmpty())
      pMeta->m_SwitchStates.PushBack(instrument.m_sSwitchState);
  }

  pInfo->m_MetaInfo.PushBack(pMeta);
}

plTransformStatus plAudioEventAssetDocument::InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  const plAudioEventAssetProperties* pProp = GetProperties();

  if (pProp->m_Instruments.IsEmpty())
    return plStatus("The audio event has no instruments.");

  plAudioEventDesc desc;
  desc.m_sOutputBus = pProp->m_sOutputBus;
  desc.m_uiMaxInstances = pProp->m_uiMaxInstances;
  desc.m_eStealing = static_cast<plAudioEventStealingMode::Enum>(pProp->m_eStealing.GetValue());
  desc.m_Cooldown = plTime::MakeFromSeconds(pProp->m_fCooldown);
  desc.m_FadeIn = plTime::MakeFromSeconds(pProp->m_fFadeIn);
  desc.m_StopFadeOut = plTime::MakeFromSeconds(pProp->m_fStopFadeOut);
  desc.m_bLoopRegion = pProp->m_bLoopRegion;
  desc.m_LoopStart = plTime::MakeFromSeconds(plMath::Min(pProp->m_fLoopStart, pProp->m_fLoopEnd));
  desc.m_LoopEnd = plTime::MakeFromSeconds(plMath::Max(pProp->m_fLoopStart, pProp->m_fLoopEnd));
  desc.m_bSpatialize = pProp->m_bSpatialize;
  desc.m_fMinDistance = plMath::Min(pProp->m_fMinDistance, pProp->m_fMaxDistance);
  desc.m_fMaxDistance = plMath::Max(pProp->m_fMinDistance, pProp->m_fMaxDistance);
  desc.m_fRolloff = pProp->m_fRolloff;
  desc.m_fDopplerFactor = pProp->m_fDopplerFactor;

  // parameters
  for (const auto& parameter : pProp->m_Parameters)
  {
    auto& paramDesc = desc.m_Parameters.ExpandAndGetRef();
    paramDesc.m_sName = parameter.m_sName;
    paramDesc.m_fMin = plMath::Min(parameter.m_fMin, parameter.m_fMax);
    paramDesc.m_fMax = plMath::Max(parameter.m_fMin, parameter.m_fMax);
    paramDesc.m_fDefault = plMath::Clamp(parameter.m_fDefault, paramDesc.m_fMin, paramDesc.m_fMax);
  }

  // instruments + deduplicated sample table
  plMap<plString, plUInt16> sampleIndices;

  for (const auto& instrument : pProp->m_Instruments)
  {
    auto& instrumentDesc = desc.m_Instruments.ExpandAndGetRef();
    instrumentDesc.m_sName = instrument.m_sName;
    instrumentDesc.m_StartDelay = plTime::MakeFromSeconds(instrument.m_fStartDelay);
    instrumentDesc.m_ePlaylistMode = static_cast<plAudioEventPlaylistMode::Enum>(instrument.m_ePlaylistMode.GetValue());
    instrumentDesc.m_bLoop = instrument.m_bLoop;
    instrumentDesc.m_bMute = instrument.m_bMute;
    instrumentDesc.m_sSwitchState = instrument.m_sSwitchState;
    instrumentDesc.m_MinRetriggerDelay = plTime::MakeFromSeconds(plMath::Min(instrument.m_fMinRetriggerDelay, instrument.m_fMaxRetriggerDelay));
    instrumentDesc.m_MaxRetriggerDelay = plTime::MakeFromSeconds(plMath::Max(instrument.m_fMinRetriggerDelay, instrument.m_fMaxRetriggerDelay));
    instrumentDesc.m_fVolumeDb = instrument.m_fVolumeDb;
    instrumentDesc.m_fVolumeRandDb = instrument.m_fVolumeRandDb;
    instrumentDesc.m_fPitchSemitones = instrument.m_fPitchSemitones;
    instrumentDesc.m_fPitchRandSemitones = instrument.m_fPitchRandSemitones;

    for (const auto& entry : instrument.m_Playlist)
    {
      auto& entryDesc = instrumentDesc.m_Playlist.ExpandAndGetRef();
      entryDesc.m_fWeight = entry.m_fWeight;

      if (entry.m_sFile.IsEmpty())
      {
        entryDesc.m_uiSample = plAudioEventSilenceSample;
        continue;
      }

      if (auto it = sampleIndices.Find(entry.m_sFile); it.IsValid())
      {
        entryDesc.m_uiSample = it.Value();
      }
      else
      {
        const plUInt16 uiIndex = static_cast<plUInt16>(desc.m_SampleNames.GetCount());
        desc.m_SampleNames.PushBack(entry.m_sFile);
        sampleIndices[entry.m_sFile] = uiIndex;
        entryDesc.m_uiSample = uiIndex;
      }
    }
  }

  // automations
  for (const auto& automation : pProp->m_Automations)
  {
    const plUInt32 uiParam = desc.FindParameterByName(automation.m_sParameter);
    if (uiParam == plInvalidIndex)
      return plStatus(plFmt("Automation references unknown parameter '{0}'. Add it to the Parameters list.", automation.m_sParameter));

    if (automation.m_Points.GetCount() < 2)
      return plStatus(plFmt("Automation on parameter '{0}' needs at least two curve points.", automation.m_sParameter));

    auto& automationDesc = desc.m_Automations.ExpandAndGetRef();
    automationDesc.m_uiParameter = static_cast<plUInt8>(uiParam);
    automationDesc.m_eTarget = static_cast<plAudioEventAutomationTarget::Enum>(automation.m_eTarget.GetValue());

    for (const auto& point : automation.m_Points)
    {
      auto& cp = automationDesc.m_Curve.AddControlPoint(point.m_fX);
      cp.m_Position.y = point.m_fY;
    }

    automationDesc.m_Curve.SortControlPoints();
    automationDesc.m_Curve.CreateLinearApproximation();
  }

  PL_SUCCEED_OR_RETURN(desc.Save(stream));

  // sample payloads
  stream << desc.m_SampleNames.GetCount();

  for (const auto& sFile : desc.m_SampleNames)
  {
    plStringBuilder sAssetFile = sFile;
    if (!plQtEditorApp::GetSingleton()->MakeDataDirectoryRelativePathAbsolute(sAssetFile))
      return plStatus(plFmt("Failed to make sound file path absolute: '{0}'", sFile));

    plFileReader SoundFile;
    if (SoundFile.Open(sAssetFile).Failed())
      return plStatus(plFmt("Could not open sound file for reading: '{0}'", sAssetFile));

    plDefaultMemoryStreamStorage storage;
    {
      plMemoryStreamWriter writer(&storage);
      plUInt8 Temp[4 * 1024];

      while (true)
      {
        const plUInt64 uiRead = SoundFile.ReadBytes(Temp, PL_ARRAY_SIZE(Temp));
        if (uiRead == 0)
          break;

        writer.WriteBytes(Temp, uiRead).IgnoreResult();
      }
    }

    stream << storage.GetStorageSize32();
    PL_SUCCEED_OR_RETURN(storage.CopyToStream(stream));
  }

  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plAudioEventAssetDocumentGenerator, 1, plRTTIDefaultAllocator<plAudioEventAssetDocumentGenerator>)
PL_END_DYNAMIC_REFLECTED_TYPE;

plAudioEventAssetDocumentGenerator::plAudioEventAssetDocumentGenerator()
{
  AddSupportedFileType("wav");
  AddSupportedFileType("mp3");
}

plAudioEventAssetDocumentGenerator::~plAudioEventAssetDocumentGenerator() = default;

void plAudioEventAssetDocumentGenerator::GetImportModes(plStringView sAbsInputFile, plDynamicArray<plAssetDocumentGenerator::ImportMode>& out_modes) const
{
  {
    plAssetDocumentGenerator::ImportMode& info = out_modes.ExpandAndGetRef();
    info.m_Priority = plAssetDocGeneratorPriority::DefaultPriority;
    info.m_sName = "Audio_Event";
    info.m_sIcon = ":/AssetIcons/AudioEvent.svg";
  }
}

plStatus plAudioEventAssetDocumentGenerator::Generate(plStringView sInputFileAbs, plStringView sMode, plDynamicArray<plDocument*>& out_generatedDocuments)
{
  plStringBuilder sOutFile = sInputFileAbs;
  sOutFile.ChangeFileExtension(GetDocumentExtension());

  if (plOSFile::ExistsFile(sOutFile))
  {
    plLog::Info("Skipping audio event import, file has been imported before: '{}'", sOutFile);
    return plStatus(PL_SUCCESS);
  }

  auto pApp = plQtEditorApp::GetSingleton();

  plStringBuilder sInputFileRel = sInputFileAbs;
  pApp->MakePathDataDirectoryRelative(sInputFileRel);

  plDocument* pDoc = pApp->CreateDocument(sOutFile, plDocumentFlags::None);
  if (pDoc == nullptr)
    return plStatus("Could not create target document");

  out_generatedDocuments.PushBack(pDoc);

  plAudioEventAssetDocument* pAssetDoc = plDynamicCast<plAudioEventAssetDocument*>(pDoc);
  if (pAssetDoc == nullptr)
    return plStatus("Target document is not a valid plAudioEventAssetDocument");

  auto pPropObj = pAssetDoc->GetPropertyObject();

  plObjectCommandAccessor ca(pAssetDoc->GetCommandHistory());
  ca.StartTransaction("Init Values");

  // one instrument with one playlist entry referencing the imported file
  plUuid instrumentGuid;
  ca.AddObjectByName(pPropObj, "Instruments", 0, plGetStaticRTTI<plAudioEventInstrument>(), instrumentGuid).AssertSuccess();
  const plDocumentObject* pInstrument = pAssetDoc->GetObjectManager()->GetObject(instrumentGuid);

  plUuid entryGuid;
  ca.AddObjectByName(pInstrument, "Playlist", 0, plGetStaticRTTI<plAudioEventPlaylistEntry>(), entryGuid).AssertSuccess();
  const plDocumentObject* pEntry = pAssetDoc->GetObjectManager()->GetObject(entryGuid);

  ca.SetValueByName(pEntry, "File", sInputFileRel.GetView()).AssertSuccess();

  ca.FinishTransaction();

  plLog::Success("Imported audio event: '{}'", sOutFile);

  return plStatus(PL_SUCCESS);
}
