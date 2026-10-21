#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Strings/StringBuilder.h>
#include <PlasmaAudioPlugin/Banks/AudioBankResource.h>
#include <PlasmaAudioPlugin/Codecs/AudioCodec.h>

class plAudioBankAssetDocument;
class plAudioBankStatusMsgToEditor;

/// \brief What the bank window shows: every bank of the project with its contents and their sizes on one platform in
/// one language, the banks the engine process has loaded, the memory they take against the platform's budget, and the
/// problems found.
///
/// Built from the curator's asset infos (the published meta infos of banks, events and clips), with the window's own
/// bank from its document, so unsaved edits show. A clip's size and languages come from the clip as built for the
/// platform; where it is not built for it, from an estimate from its build settings (see
/// plAudioClipAssetUtils::EstimateMemory). Events are listed with the size of their built file but do not count
/// against the budget, which covers sample data.
struct plAudioBankDashboard
{
  enum class BankState : plUInt8
  {
    NotLoaded,
    Loading,
    Loaded,
    Missing, ///< the engine process could not load the bank's built file
  };

  struct Item
  {
    plString m_sGuid;
    plString m_sName;
    bool m_bClip = false;
    bool m_bAuto = false;    ///< included because an event of the bank plays it
    bool m_bUnknown = false; ///< the reference points to no asset
    plAudioClipLoadMode::Enum m_eLoadMode = plAudioClipLoadMode::Default;
    plUInt64 m_uiBytes = 0;
    bool m_bLocalized = false;
    bool m_bMissingLanguage = false; ///< localised, but not in the selected language
    bool m_bAssumedSettings = false; ///< the clip's meta info has no build settings yet
    plUInt32 m_uiBanks = 0;          ///< banks that contain it
  };

  struct Bank
  {
    plString m_sGuid;
    plString m_sName;
    plString m_sDocumentPath;
    bool m_bThisDocument = false;
    bool m_bAutoInclude = true;
    bool m_bPacked = false;
    plDynamicArray<Item> m_Items;
    plUInt64 m_uiClipBytes = 0;            ///< the clips' sizes, each once
    plUInt32 m_uiExcludedEventClips = 0; ///< clips its events play that it does not hold, with auto-include off

    BankState m_State = BankState::NotLoaded;
    float m_fProgress = 0.0f; ///< of a loading bank
    bool m_bOverBudget = false;
    bool m_bLoadedByWindow = false;
  };

  struct Warning
  {
    plStringBuilder m_sText;
    bool m_bError = false;
    plInt32 m_iBank = -1; ///< the bank to select, -1 for none
    plString m_sAsset;    ///< the asset to highlight (GUID), empty for none
  };

  /// \name Choices
  ///@{
  plDynamicArray<plString> m_Profiles;  ///< the asset profiles' config names
  plDynamicArray<plString> m_Languages; ///< "" (the default) first, then every language a clip has
  plUInt32 m_uiProfile = 0;
  plString m_sLanguage;
  ///@}

  plDynamicArray<Bank> m_Banks; ///< the window's bank first, then by name
  plDynamicArray<Warning> m_Warnings;
  plUInt32 m_uiAssets = 0; ///< distinct events and clips in all banks

  /// \name The loaded banks on the selected platform
  ///@{
  plAudioMemoryUsage m_Loaded;
  plAudioMemoryBudget m_Budget;
  plUInt32 m_uiLoadedBanks = 0;
  ///@}

  /// \name The engine process
  ///@{
  bool m_bEngineAvailable = false;
  plAudioMemoryUsage m_Measured; ///< the loaded content as the engine process measures it, on the editor's platform
  ///@}

  /// \brief Rebuilds everything for the profile and language from the curator and the window's document.
  void Build(const plAudioBankAssetDocument& document);

  /// \brief Takes the loaded banks and the measured memory from the engine process's status and recomputes the totals
  /// and the budget warnings.
  void ApplyEngineStatus(const plAudioBankStatusMsgToEditor& status);

  /// \brief The index of the bank with the GUID, or -1.
  plInt32 FindBank(plStringView sGuid) const;

private:
  void UpdateTotals();

  plDynamicArray<Warning> m_ContentWarnings; ///< everything but the budget
  plUInt32 m_uiOrphanedEventClips = 0;
};
