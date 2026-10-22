#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/SoundAsset/MiniAudioSoundAsset.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioSoundAssetDocument, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioSoundAssetProperties, 1, plRTTIDefaultAllocator<plMiniAudioSoundAssetProperties>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ARRAY_MEMBER_PROPERTY("Files", m_SoundFiles)->AddAttributes(new plFileBrowserAttribute("Select Sound", "*.wav;*.mp3")),
    PL_MEMBER_PROPERTY("Group", m_sGroup)->AddAttributes(new plDynamicStringEnumAttribute("MiniAudioSoundGroups")),
    PL_MEMBER_PROPERTY("Loop", m_bLoop),
    PL_MEMBER_PROPERTY("MinRandomVolume", m_fMinVolume)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.1f, 10.0f)),
    PL_MEMBER_PROPERTY("MaxRandomVolume", m_fMaxVolume)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.1f, 10.0f)),
    PL_MEMBER_PROPERTY("MinRandomPitch", m_fMinPitch)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.1f, 10.0f)),
    PL_MEMBER_PROPERTY("MaxRandomPitch", m_fMaxPitch)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.1f, 10.0f)),
    PL_MEMBER_PROPERTY("IsPositional", m_bSpatialize)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("DopplerFactor", m_fDopplerFactor)->AddAttributes(new plDefaultValueAttribute(0.0f), new plClampValueAttribute(0.0f, 10.0f)),
    PL_MEMBER_PROPERTY("SoundSize", m_fMinDistance)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.01f, 100.0f)),
    // PL_MEMBER_PROPERTY("MaxDistance", m_fMaxDistance)->AddAttributes(new plDefaultValueAttribute(10.0f), new plClampValueAttribute(1.0f, 1000.0f)),
    PL_MEMBER_PROPERTY("Rolloff", m_fRolloff)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.001f, 1000.0f)),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plMiniAudioSoundAssetDocument::plMiniAudioSoundAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plMiniAudioSoundAssetProperties>(sDocumentPath, plAssetDocEngineConnection::None)
{
}

void plMiniAudioSoundAssetDocument::UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const
{
  SUPER::UpdateAssetDocumentInfo(pInfo);

  const plMiniAudioSoundAssetProperties* pProp = GetProperties();

  for (const auto& str : pProp->m_SoundFiles)
  {
    pInfo->m_TransformDependencies.Insert(str);
  }
}

plTransformStatus plMiniAudioSoundAssetDocument::InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  const plMiniAudioSoundAssetProperties* pProp = GetProperties();

  if (pProp->m_SoundFiles.IsEmpty())
    return plStatus("No sound files have been specified.");

  const plUInt8 uiVersion = 2;
  stream << uiVersion;

  stream << pProp->m_bLoop;
  stream << pProp->m_fMinVolume;
  stream << pProp->m_fMaxVolume;
  stream << pProp->m_fMinPitch;
  stream << pProp->m_fMaxPitch;
  stream << pProp->m_bSpatialize;
  stream << pProp->m_fMinDistance;
  stream << pProp->m_fMaxDistance;
  stream << pProp->m_fRolloff;
  stream << pProp->m_fDopplerFactor;
  stream << pProp->m_SoundFiles.GetCount();

  for (const auto& sFile : pProp->m_SoundFiles)
  {
    plStringBuilder sAssetFile = sFile;
    if (!plEditorApp::GetSingleton()->MakeDataDirectoryRelativePathAbsolute(sAssetFile))
      return plStatus(plFmt("Failed to make sound file path absolute: '{0}'", sFile));

    plFileReader SoundFile;
    if (SoundFile.Open(sAssetFile).Failed())
      return plStatus(plFmt("Could not open sound-file for reading: '{0}'", sAssetFile));

    // we copy the entire sound into our transformed asset

    plDefaultMemoryStreamStorage storage;

    // copy the file from disk into memory
    {
      plMemoryStreamWriter writer(&storage);

      plUInt8 Temp[4 * 1024];

      while (true)
      {
        plUInt64 uiRead = SoundFile.ReadBytes(Temp, PL_ARRAY_SIZE(Temp));

        if (uiRead == 0)
          break;

        writer.WriteBytes(Temp, uiRead).IgnoreResult();
      }
    }

    // now store the entire file in our asset output
    stream << storage.GetStorageSize32();
    PL_SUCCEED_OR_RETURN(storage.CopyToStream(stream));
  }

  // version 2
  stream << pProp->m_sGroup;

  return plStatus(PL_SUCCESS);
}

void plMiniAudioSoundAssetProperties::PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
{
  if (e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plMiniAudioSoundAssetProperties>())
  {
    auto& props = *e.m_pPropertyStates;

    const bool bIsPositional = e.m_pObject->GetTypeAccessor().GetValue("IsPositional").ConvertTo<bool>();

    props["DopplerFactor"].m_Visibility = bIsPositional ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["SoundSize"].m_Visibility = bIsPositional ? plPropertyUiState::Default : plPropertyUiState::Invisible;
    props["Rolloff"].m_Visibility = bIsPositional ? plPropertyUiState::Default : plPropertyUiState::Invisible;
  }
}

//////////////////////////////////////////////////////////////////////////

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMiniAudioSoundAssetDocumentGenerator, 1, plRTTIDefaultAllocator<plMiniAudioSoundAssetDocumentGenerator>)
PL_END_DYNAMIC_REFLECTED_TYPE;

plMiniAudioSoundAssetDocumentGenerator::plMiniAudioSoundAssetDocumentGenerator()
{
  AddSupportedFileType("wav");
  AddSupportedFileType("mp3");
}

plMiniAudioSoundAssetDocumentGenerator::~plMiniAudioSoundAssetDocumentGenerator() = default;

void plMiniAudioSoundAssetDocumentGenerator::GetImportModes(plStringView sAbsInputFile, plDynamicArray<plAssetDocumentGenerator::ImportMode>& out_modes) const
{
  PL_IGNORE_UNUSED(sAbsInputFile);
  PL_IGNORE_UNUSED(out_modes);

  // Deliberately offers no import mode: this asset type is legacy and can no longer be created
  // (see plMiniAudioSoundAssetDocumentManager, m_bCanCreate = false). Importing a sound file
  // creates an Audio Clip (and optionally an Audio Event) instead - see plAudioClipAssetDocumentGenerator.
}

plStatus plMiniAudioSoundAssetDocumentGenerator::Generate(plStringView sInputFileAbs, plStringView sMode, plDynamicArray<plDocument*>& out_generatedDocuments)
{
  PL_IGNORE_UNUSED(sInputFileAbs);
  PL_IGNORE_UNUSED(sMode);
  PL_IGNORE_UNUSED(out_generatedDocuments);

  // legacy type: kept so existing assets still open, but no longer creatable
  return plStatus("MiniAudio Sound assets are legacy and can no longer be created. Import the sound file as an Audio Clip instead.");
}

