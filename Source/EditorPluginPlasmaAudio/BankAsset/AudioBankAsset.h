#pragma once

#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <EnginePluginPlasmaAudio/Banks/AudioBankMessages.h>

class plPlatformProfile;
struct plAudioMemoryBudget;

/// \brief The events and clips an audio bank keeps loaded, as asset references.
class plAudioBankAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioBankAssetProperties, plReflectedClass);

public:
  plDynamicArray<plString> m_Events;
  plDynamicArray<plString> m_Clips;

  /// \brief The bank also loads every clip its events play.
  bool m_bAutoIncludeEventClips = true;

  /// \brief The bank file carries the built files of its events and clips (streamed clips excepted), so loading it
  /// reads one file.
  bool m_bPacked = false;
};

/// \brief A bank's content published in the asset document info, so the bank window shows every bank of the project
/// without opening them.
class plAudioBankAssetMetaInfo : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioBankAssetMetaInfo, plReflectedClass);

public:
  plDynamicArray<plString> m_Events; ///< asset GUID strings
  plDynamicArray<plString> m_Clips;  ///< asset GUID strings, without the auto-included ones
  bool m_bAutoIncludeEventClips = true;
  bool m_bPacked = false;
};

/// \brief Helpers shared by the bank transform and the bank window.
struct plAudioBankAssetUtils
{
  /// \brief The asset GUID string of an asset reference (a GUID string or a path), empty for an unknown asset.
  static plString ResolveAsset(plStringView sReference);

  /// \brief Adds the clips the event plays (as asset GUID strings) to inout_clips, skipping those already in it.
  static void AddEventClips(plStringView sEventGuid, plDynamicArray<plString>& inout_clips);

  /// \brief The memory budget of the profile's Audio Budgets config, in bytes.
  static plAudioMemoryBudget GetBudget(const plPlatformProfile* pProfile);
};

class plAudioBankAssetDocument : public plSimpleAssetDocument<plAudioBankAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioBankAssetDocument, plSimpleAssetDocument<plAudioBankAssetProperties>);

public:
  plAudioBankAssetDocument(plStringView sDocumentPath);

  /// \brief Asks the engine process to load or unload the bank sBankGuid, or only for the status (Poll).
  void SendBankCommand(plAudioBankCommand::Enum command, plStringView sBankGuid = {});

  /// \brief The last status the engine process sent.
  const plAudioBankStatusMsgToEditor& GetEngineStatus() const { return m_EngineStatus; }

  /// \brief Fires for every status the engine process sends.
  plEvent<const plAudioBankStatusMsgToEditor&> m_EngineStatusEvents;

protected:
  virtual void HandleEngineMessage(const plEditorEngineDocumentMsg* pMsg) override;
  virtual void UpdateAssetDocumentInfo(plAssetDocumentInfo* pInfo) const override;
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile,
    const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;

private:
  plAudioBankStatusMsgToEditor m_EngineStatus;
};
