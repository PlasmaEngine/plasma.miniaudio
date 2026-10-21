#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <Core/Configuration/PlatformProfile.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginPlasmaAudio/BankAsset/AudioBankAsset.h>
#include <EditorPluginPlasmaAudio/BankAsset/AudioBankDashboard.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <Foundation/Containers/HashSet.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <PlasmaAudioPlugin/Clips/AudioClipDesc.h>
#include <PlasmaAudioPlugin/Profiling/AudioProfilingSnapshot.h>

namespace
{
  /// The engine process keeps this many streams at most (PlasmaAudio.MaxStreams), each with a ring of four chunks.
  constexpr plUInt64 s_uiMaxStreams = 32;
  constexpr plUInt64 s_uiRingBytes = 4 * plAudioClipConstants::StreamChunkBytes;

  struct ClipSource
  {
    plString m_sName;
    plString m_sDocumentPath;
    plAudioClipAssetMetaInfo m_Info;
  };

  /// Meta infos made from clip documents saved before the build settings were published, by document and its
  /// modification time.
  struct ReadMetaInfo
  {
    plTimestamp m_Modified;
    bool m_bValid = false;
    plAudioClipAssetMetaInfo m_Info;
  };

  plHashTable<plString, ReadMetaInfo> s_ReadMetaInfos;

  /// The clip's meta info as it would publish it now, read from its document. False when the document cannot be read.
  bool ReadClipMetaInfo(plStringView sDocumentPath, plAudioClipAssetMetaInfo& out_info)
  {
    plFileStats stats;
    if (plOSFile::GetFileStats(sDocumentPath, stats).Failed())
      return false;

    ReadMetaInfo& cached = s_ReadMetaInfos[sDocumentPath];
    if (!cached.m_Modified.Compare(stats.m_LastModificationTime, plTimestamp::CompareMode::FileTimeEqual))
    {
      cached.m_Modified = stats.m_LastModificationTime;

      plAudioClipAssetProperties props;
      cached.m_bValid = plAudioClipAssetUtils::ReadProperties(sDocumentPath, props).Succeeded();
      if (cached.m_bValid)
        plAudioClipAssetUtils::MakeMetaInfo(props, cached.m_Info);
    }

    if (cached.m_bValid)
      out_info = cached.m_Info;

    return cached.m_bValid;
  }

  struct EventSource
  {
    plString m_sName;
    plString m_sDocumentPath;
    plDynamicArray<plString> m_Dependencies;
    plDynamicArray<plString> m_Clips; ///< resolved from the dependencies
  };

  struct BankSource
  {
    plString m_sGuid;
    plString m_sName;
    plString m_sDocumentPath;
    plAudioBankAssetMetaInfo m_Info;
  };

  /// What the curator knows about the project's audio assets, copied out of its lock.
  struct ProjectAssets
  {
    plHashTable<plString, ClipSource> m_Clips;
    plHashTable<plString, EventSource> m_Events;
    plDynamicArray<BankSource> m_Banks;
  };

  void GatherProjectAssets(ProjectAssets& out_assets)
  {
    {
      const auto subAssets = plAssetCurator::GetSingleton()->GetKnownSubAssets();
      for (auto it = subAssets->GetIterator(); it.IsValid(); ++it)
      {
        const plSubAsset& subAsset = it.Value();
        if (subAsset.m_pAssetInfo == nullptr || subAsset.m_pAssetInfo->m_Info == nullptr)
          continue;

        const plStringView sType = subAsset.m_Data.m_sSubAssetsDocumentTypeName.GetView();
        const plAssetDocumentInfo& info = *subAsset.m_pAssetInfo->m_Info;

        plStringBuilder sGuid;
        plConversionUtils::ToString(subAsset.m_Data.m_Guid, sGuid);

        if (sType == "Audio Clip")
        {
          ClipSource& clip = out_assets.m_Clips[sGuid];
          clip.m_sName = subAsset.GetName();
          clip.m_sDocumentPath = subAsset.m_pAssetInfo->m_Path.GetAbsolutePath();
          if (const plAudioClipAssetMetaInfo* pMeta = info.GetMetaInfo<plAudioClipAssetMetaInfo>())
            clip.m_Info = *pMeta;
        }
        else if (sType == "Audio Event")
        {
          EventSource& event = out_assets.m_Events[sGuid];
          event.m_sName = subAsset.GetName();
          event.m_sDocumentPath = subAsset.m_pAssetInfo->m_Path.GetAbsolutePath();
          for (const plString& sDependency : info.m_TransformDependencies)
          {
            event.m_Dependencies.PushBack(sDependency);
          }
        }
        else if (sType == "Audio Bank")
        {
          BankSource& bank = out_assets.m_Banks.ExpandAndGetRef();
          bank.m_sGuid = sGuid;
          bank.m_sName = subAsset.GetName();
          bank.m_sDocumentPath = subAsset.m_pAssetInfo->m_Path.GetAbsolutePath();
          if (const plAudioBankAssetMetaInfo* pMeta = info.GetMetaInfo<plAudioBankAssetMetaInfo>())
            bank.m_Info = *pMeta;
        }
      }
    }

    // a clip saved before its build settings were published gets them from its document
    for (auto it = out_assets.m_Clips.GetIterator(); it.IsValid(); ++it)
    {
      if (it.Value().m_Info.m_Platforms.IsEmpty())
        ReadClipMetaInfo(it.Value().m_sDocumentPath, it.Value().m_Info);
    }

    // an event's clip references are GUID strings among its dependencies
    for (auto it = out_assets.m_Events.GetIterator(); it.IsValid(); ++it)
    {
      for (const plString& sDependency : it.Value().m_Dependencies)
      {
        if (out_assets.m_Clips.Contains(sDependency))
          it.Value().m_Clips.PushBack(sDependency);
      }
    }
  }

  /// Size of the event's built file for the profile, 0 when it is not built.
  plUInt64 GetBuiltEventBytes(plStringView sGuid, const plPlatformProfile* pProfile)
  {
    plString sOutput;
    {
      const auto pSubAsset = plAssetCurator::GetSingleton()->FindSubAsset(sGuid);
      if (!pSubAsset.isValid() || pSubAsset->m_pAssetInfo == nullptr || pSubAsset->m_pAssetInfo->m_pDocumentTypeDescriptor == nullptr)
        return 0;

      const plAssetDocumentTypeDescriptor* pTypeDesc = pSubAsset->m_pAssetInfo->m_pDocumentTypeDescriptor;
      const auto* pManager = static_cast<const plAssetDocumentManager*>(pTypeDesc->m_pManager);
      sOutput = pManager->GetAbsoluteOutputFileName(pTypeDesc, pSubAsset->m_pAssetInfo->m_Path.GetAbsolutePath(), "", pProfile);
    }

    plFileStats stats;
    return plOSFile::GetFileStats(sOutput, stats).Succeeded() ? stats.m_uiFileSize : 0;
  }

  /// Built clip headers by output file, read again when the file changes.
  struct BuiltHeader
  {
    plTimestamp m_Modified;
    bool m_bValid = false;
    plAudioClipHeader m_Header;
  };

  plHashTable<plString, BuiltHeader> s_BuiltHeaders;

  /// The clip as built for the profile, null when it is not built for it.
  const plAudioClipHeader* GetBuiltHeader(plStringView sGuid, const plPlatformProfile* pProfile)
  {
    plString sOutput;
    {
      const auto pSubAsset = plAssetCurator::GetSingleton()->FindSubAsset(sGuid);
      if (!pSubAsset.isValid() || pSubAsset->m_pAssetInfo == nullptr || pSubAsset->m_pAssetInfo->m_pDocumentTypeDescriptor == nullptr)
        return nullptr;

      const plAssetDocumentTypeDescriptor* pTypeDesc = pSubAsset->m_pAssetInfo->m_pDocumentTypeDescriptor;
      const auto* pManager = static_cast<const plAssetDocumentManager*>(pTypeDesc->m_pManager);
      sOutput = pManager->GetAbsoluteOutputFileName(pTypeDesc, pSubAsset->m_pAssetInfo->m_Path.GetAbsolutePath(), "", pProfile);
    }

    plFileStats stats;
    if (plOSFile::GetFileStats(sOutput, stats).Failed())
      return nullptr;

    BuiltHeader& cached = s_BuiltHeaders[sOutput];
    if (!cached.m_Modified.Compare(stats.m_LastModificationTime, plTimestamp::CompareMode::FileTimeEqual))
    {
      cached.m_Modified = stats.m_LastModificationTime;
      cached.m_bValid = plAudioClipAssetUtils::ReadBuiltHeader(sGuid, cached.m_Header, pProfile);
    }

    return cached.m_bValid ? &cached.m_Header : nullptr;
  }

  void FormatMegabytes(plUInt64 uiBytes, plStringBuilder& out_sText)
  {
    out_sText.SetFormat("{} MB", plArgF(uiBytes / (1024.0 * 1024.0), 1));
  }
} // namespace

plInt32 plAudioBankDashboard::FindBank(plStringView sGuid) const
{
  for (plUInt32 i = 0; i < m_Banks.GetCount(); ++i)
  {
    if (m_Banks[i].m_sGuid.IsEqual_NoCase(sGuid))
      return static_cast<plInt32>(i);
  }

  return -1;
}

void plAudioBankDashboard::Build(const plAudioBankAssetDocument& document)
{
  plAssetCurator* pCurator = plAssetCurator::GetSingleton();

  m_Profiles.Clear();
  for (plUInt32 i = 0; i < pCurator->GetNumAssetProfiles(); ++i)
  {
    m_Profiles.PushBack(pCurator->GetAssetProfile(i)->GetConfigName());
  }

  m_uiProfile = plMath::Min(m_uiProfile, m_Profiles.IsEmpty() ? 0u : m_Profiles.GetCount() - 1);
  const plPlatformProfile* pProfile = m_Profiles.IsEmpty() ? nullptr : pCurator->GetAssetProfile(m_uiProfile);
  const plStringView sProfile = m_Profiles.IsEmpty() ? plStringView() : m_Profiles[m_uiProfile].GetView();

  ProjectAssets assets;
  GatherProjectAssets(assets);

  // the languages any clip has, as published or as built
  m_Languages.Clear();
  m_Languages.PushBack(plString());
  for (auto it = assets.m_Clips.GetIterator(); it.IsValid(); ++it)
  {
    for (const plAudioClipLanguageInfo& language : it.Value().m_Info.m_Languages)
    {
      if (!language.m_sLanguage.IsEmpty() && !m_Languages.Contains(language.m_sLanguage))
        m_Languages.PushBack(language.m_sLanguage);
    }

    if (const plAudioClipHeader* pBuilt = GetBuiltHeader(it.Key(), pProfile))
    {
      for (const plAudioClipLanguageEntry& language : pBuilt->m_Languages)
      {
        if (!language.m_sLanguage.IsEmpty() && !m_Languages.Contains(language.m_sLanguage))
          m_Languages.PushBack(language.m_sLanguage);
      }
    }
  }

  if (!m_Languages.Contains(m_sLanguage))
    m_sLanguage.Clear();

  // the window's bank as it is edited, not as it was saved
  plStringBuilder sDocumentGuid;
  plConversionUtils::ToString(document.GetGuid(), sDocumentGuid);

  {
    const plAudioBankAssetProperties* pProps = document.GetProperties();

    BankSource* pSource = nullptr;
    for (BankSource& bank : assets.m_Banks)
    {
      if (bank.m_sGuid.IsEqual_NoCase(sDocumentGuid))
        pSource = &bank;
    }

    if (pSource == nullptr)
    {
      pSource = &assets.m_Banks.ExpandAndGetRef();
      pSource->m_sGuid = sDocumentGuid;
      pSource->m_sName = plPathUtils::GetFileName(document.GetDocumentPath());
      pSource->m_sDocumentPath = document.GetDocumentPath();
    }

    pSource->m_Info.m_Events.Clear();
    pSource->m_Info.m_Clips.Clear();
    pSource->m_Info.m_bAutoIncludeEventClips = pProps->m_bAutoIncludeEventClips;
    pSource->m_Info.m_bPacked = pProps->m_bPacked;

    // an unknown reference stays as it is, so the bank shows it
    for (const plString& sEvent : pProps->m_Events)
    {
      const plString sGuid = plAudioBankAssetUtils::ResolveAsset(sEvent);
      if (!sEvent.IsEmpty())
        pSource->m_Info.m_Events.PushBack(sGuid.IsEmpty() ? sEvent : sGuid);
    }

    for (const plString& sClip : pProps->m_Clips)
    {
      const plString sGuid = plAudioBankAssetUtils::ResolveAsset(sClip);
      if (!sClip.IsEmpty())
        pSource->m_Info.m_Clips.PushBack(sGuid.IsEmpty() ? sClip : sGuid);
    }
  }

  // the banks and their contents
  m_Banks.Clear();
  for (const BankSource& source : assets.m_Banks)
  {
    Bank& bank = m_Banks.ExpandAndGetRef();
    bank.m_sGuid = source.m_sGuid;
    bank.m_sName = source.m_sName;
    bank.m_sDocumentPath = source.m_sDocumentPath;
    bank.m_bThisDocument = source.m_sGuid.IsEqual_NoCase(sDocumentGuid);
    bank.m_bAutoInclude = source.m_Info.m_bAutoIncludeEventClips;
    bank.m_bPacked = source.m_Info.m_bPacked;

    auto AddItem = [&](plStringView sGuid, bool bClip, bool bAuto)
    {
      for (const Item& existing : bank.m_Items)
      {
        if (existing.m_sGuid == sGuid)
          return;
      }

      Item& item = bank.m_Items.ExpandAndGetRef();
      item.m_sGuid = sGuid;
      item.m_bClip = bClip;
      item.m_bAuto = bAuto;

      if (bClip)
      {
        const ClipSource* pClip = nullptr;
        if (!assets.m_Clips.TryGetValue(item.m_sGuid, pClip))
        {
          item.m_sName = sGuid;
          item.m_bUnknown = true;
          return;
        }

        // the clip as built for the platform is exact; without a build, the estimate from its settings
        const plAudioClipHeader* pBuilt = GetBuiltHeader(item.m_sGuid, pProfile);
        const plAudioClipMemoryEstimate estimate = pBuilt != nullptr ? plAudioClipAssetUtils::EstimateMemory(*pBuilt, m_sLanguage) : plAudioClipAssetUtils::EstimateMemory(pClip->m_Info, sProfile, m_sLanguage);
        item.m_sName = pClip->m_sName;
        item.m_eLoadMode = estimate.m_eLoadMode;
        item.m_uiBytes = estimate.m_uiResidentBytes;
        item.m_bLocalized = estimate.m_bLocalized;
        item.m_bMissingLanguage = !estimate.m_bHasLanguage;
        item.m_bAssumedSettings = estimate.m_bAssumedSettings;
        bank.m_uiClipBytes += item.m_uiBytes;
      }
      else
      {
        const EventSource* pEvent = nullptr;
        if (!assets.m_Events.TryGetValue(item.m_sGuid, pEvent))
        {
          item.m_sName = sGuid;
          item.m_bUnknown = true;
          return;
        }

        item.m_sName = pEvent->m_sName;
        item.m_uiBytes = GetBuiltEventBytes(item.m_sGuid, pProfile);
      }
    };

    for (const plString& sEvent : source.m_Info.m_Events)
    {
      AddItem(sEvent, false, false);
    }

    for (const plString& sClip : source.m_Info.m_Clips)
    {
      AddItem(sClip, true, false);
    }

    for (const plString& sEvent : source.m_Info.m_Events)
    {
      const EventSource* pEvent = nullptr;
      if (!assets.m_Events.TryGetValue(sEvent, pEvent))
        continue;

      for (const plString& sClip : pEvent->m_Clips)
      {
        if (bank.m_bAutoInclude)
        {
          AddItem(sClip, true, true);
        }
        else if (!source.m_Info.m_Clips.Contains(sClip))
        {
          ++bank.m_uiExcludedEventClips;
        }
      }
    }
  }

  // the window's bank first, then by name
  m_Banks.Sort([](const Bank& a, const Bank& b)
    {
      if (a.m_bThisDocument != b.m_bThisDocument)
        return a.m_bThisDocument;
      return a.m_sName.Compare_NoCase(b.m_sName) < 0; });

  // how many banks hold each asset
  plHashTable<plString, plUInt32> bankCounts;
  for (const Bank& bank : m_Banks)
  {
    for (const Item& item : bank.m_Items)
    {
      bankCounts[item.m_sGuid] += 1;
    }
  }

  m_uiAssets = bankCounts.GetCount();
  for (Bank& bank : m_Banks)
  {
    for (Item& item : bank.m_Items)
    {
      item.m_uiBanks = bankCounts[item.m_sGuid];
    }
  }

  // the problems, worst first within each kind
  m_ContentWarnings.Clear();

  for (plUInt32 b = 0; b < m_Banks.GetCount(); ++b)
  {
    for (const Item& item : m_Banks[b].m_Items)
    {
      if (item.m_bUnknown)
      {
        Warning& warning = m_ContentWarnings.ExpandAndGetRef();
        warning.m_sText.SetFormat("{} references an asset that does not exist ({})", m_Banks[b].m_sName, item.m_sName);
        warning.m_bError = true;
        warning.m_iBank = static_cast<plInt32>(b);
        warning.m_sAsset = item.m_sGuid;
      }
      else if (item.m_bMissingLanguage)
      {
        Warning& warning = m_ContentWarnings.ExpandAndGetRef();
        warning.m_sText.SetFormat("{} has no {} recording; it plays the default language", item.m_sName, m_sLanguage);
        warning.m_bError = true;
        warning.m_iBank = static_cast<plInt32>(b);
        warning.m_sAsset = item.m_sGuid;
      }
    }
  }

  // content in more than one bank, once per asset
  {
    plHashSet<plString> reported;
    for (plUInt32 b = 0; b < m_Banks.GetCount(); ++b)
    {
      for (const Item& item : m_Banks[b].m_Items)
      {
        if (item.m_uiBanks < 2 || reported.Contains(item.m_sGuid))
          continue;

        reported.Insert(item.m_sGuid);

        plStringBuilder sBanks;
        for (const Bank& other : m_Banks)
        {
          for (const Item& otherItem : other.m_Items)
          {
            if (otherItem.m_sGuid == item.m_sGuid)
              sBanks.AppendWithSeparator(" + ", other.m_sName);
          }
        }

        Warning& warning = m_ContentWarnings.ExpandAndGetRef();
        warning.m_sText.SetFormat("{} is in {}; it stays resident while either is loaded", item.m_sName, sBanks);
        warning.m_iBank = static_cast<plInt32>(b);
        warning.m_sAsset = item.m_sGuid;
      }
    }
  }

  // the clips every bank and every event reach
  plHashSet<plString> clipsInBanks;
  plHashSet<plString> clipsOfEvents;
  for (const Bank& bank : m_Banks)
  {
    for (const Item& item : bank.m_Items)
    {
      if (item.m_bClip)
        clipsInBanks.Insert(item.m_sGuid);
    }
  }

  for (auto it = assets.m_Events.GetIterator(); it.IsValid(); ++it)
  {
    for (const plString& sClip : it.Value().m_Clips)
    {
      clipsOfEvents.Insert(sClip);
    }
  }

  // a bank without auto-include whose events play clips no bank holds
  m_uiOrphanedEventClips = 0;
  for (plUInt32 b = 0; b < m_Banks.GetCount(); ++b)
  {
    const Bank& bank = m_Banks[b];
    if (bank.m_bAutoInclude)
      continue;

    plUInt32 uiOrphans = 0;
    plString sFirst;
    for (const Item& item : bank.m_Items)
    {
      const EventSource* pEvent = nullptr;
      if (item.m_bClip || !assets.m_Events.TryGetValue(item.m_sGuid, pEvent))
        continue;

      for (const plString& sClip : pEvent->m_Clips)
      {
        if (clipsInBanks.Contains(sClip))
          continue;

        if (sFirst.IsEmpty())
          sFirst = item.m_sGuid;
        ++uiOrphans;
      }
    }

    if (uiOrphans > 0)
    {
      m_uiOrphanedEventClips += uiOrphans;

      Warning& warning = m_ContentWarnings.ExpandAndGetRef();
      warning.m_sText.SetFormat("{} clip(s) the events of {} play are in no bank; they load on first play (a hitch)", uiOrphans, bank.m_sName);
      warning.m_bError = true;
      warning.m_iBank = static_cast<plInt32>(b);
      warning.m_sAsset = sFirst;
    }
  }

  // clips nothing loads or plays
  for (auto it = assets.m_Clips.GetIterator(); it.IsValid(); ++it)
  {
    if (clipsInBanks.Contains(it.Key()) || clipsOfEvents.Contains(it.Key()))
      continue;

    Warning& warning = m_ContentWarnings.ExpandAndGetRef();
    warning.m_sText.SetFormat("{} is in no bank and no event plays it", it.Value().m_sName);
    warning.m_sAsset = it.Key();
  }

  UpdateTotals();
}

void plAudioBankDashboard::ApplyEngineStatus(const plAudioBankStatusMsgToEditor& status)
{
  m_bEngineAvailable = status.m_bAvailable;
  m_Measured.m_uiDecodedBytes = status.m_uiDecodedBytes;
  m_Measured.m_uiCompressedBytes = status.m_uiCompressedBytes;
  m_Measured.m_uiPrimingBytes = status.m_uiPrimingBytes;
  m_Measured.m_uiRingBytes = status.m_uiRingBytes;

  for (Bank& bank : m_Banks)
  {
    bank.m_State = BankState::NotLoaded;
    bank.m_fProgress = 0.0f;
    bank.m_bOverBudget = false;
    bank.m_bLoadedByWindow = false;
  }

  for (plUInt32 i = 0; i < status.m_Banks.GetCount(); ++i)
  {
    // a scene's bank loader may name the bank by its path
    plString sGuid = status.m_Banks[i];
    plInt32 iBank = FindBank(sGuid);
    if (iBank < 0)
    {
      sGuid = plAudioBankAssetUtils::ResolveAsset(status.m_Banks[i]);
      iBank = FindBank(sGuid);
    }

    if (iBank < 0)
      continue;

    Bank& bank = m_Banks[iBank];
    const plUInt32 uiItems = i < status.m_Items.GetCount() ? status.m_Items[i] : 0;
    const plUInt32 uiReady = i < status.m_ReadyItems.GetCount() ? status.m_ReadyItems[i] : 0;
    const plUInt8 uiState = i < status.m_States.GetCount() ? status.m_States[i] : plAudioBankMemorySnapshotEntry::Loading;

    if (uiState == plAudioBankMemorySnapshotEntry::Missing)
      bank.m_State = BankState::Missing;
    else if (uiState == plAudioBankMemorySnapshotEntry::Loaded && uiReady >= uiItems)
      bank.m_State = BankState::Loaded;
    else
      bank.m_State = BankState::Loading;

    bank.m_fProgress = uiItems > 0 ? static_cast<float>(uiReady) / uiItems : 0.0f;
    bank.m_bOverBudget = i < status.m_OverBudget.GetCount() && status.m_OverBudget[i] != 0;
    bank.m_bLoadedByWindow = status.m_LoadedHere.Contains(status.m_Banks[i]);
  }

  UpdateTotals();
}

void plAudioBankDashboard::UpdateTotals()
{
  const plPlatformProfile* pProfile = m_Profiles.IsEmpty() ? nullptr : plAssetCurator::GetSingleton()->GetAssetProfile(m_uiProfile);
  m_Budget = plAudioBankAssetUtils::GetBudget(pProfile);

  // each clip of the loaded banks once; a loading bank's clips by its progress
  plHashTable<plString, float> loadedClips;
  m_uiLoadedBanks = 0;
  for (const Bank& bank : m_Banks)
  {
    if (bank.m_State != BankState::Loaded && bank.m_State != BankState::Loading)
      continue;

    ++m_uiLoadedBanks;
    const float fShare = bank.m_State == BankState::Loaded ? 1.0f : bank.m_fProgress;

    for (const Item& item : bank.m_Items)
    {
      if (!item.m_bClip || item.m_bUnknown)
        continue;

      float& fLoaded = loadedClips[item.m_sGuid];
      fLoaded = plMath::Max(fLoaded, fShare);
    }
  }

  m_Loaded = plAudioMemoryUsage();
  plUInt64 uiStreamedClips = 0;
  for (const Bank& bank : m_Banks)
  {
    for (const Item& item : bank.m_Items)
    {
      float* pShare = nullptr;
      if (!item.m_bClip || !loadedClips.TryGetValue(item.m_sGuid, pShare) || *pShare < 0.0f)
        continue;

      const plUInt64 uiBytes = static_cast<plUInt64>(item.m_uiBytes * static_cast<double>(*pShare));
      switch (item.m_eLoadMode)
      {
        case plAudioClipLoadMode::DecompressOnLoad:
          m_Loaded.m_uiDecodedBytes += uiBytes;
          break;
        case plAudioClipLoadMode::CompressedInMemory:
          m_Loaded.m_uiCompressedBytes += uiBytes;
          break;
        case plAudioClipLoadMode::Streamed:
          m_Loaded.m_uiPrimingBytes += uiBytes;
          ++uiStreamedClips;
          break;
      }

      // counted once
      *pShare = -1.0f;
    }
  }

  m_Loaded.m_uiRingBytes = plMath::Min(uiStreamedClips, s_uiMaxStreams) * s_uiRingBytes;

  // the content warnings, then the budget's
  m_Warnings = m_ContentWarnings;

  plStringView sProfile = m_Profiles.IsEmpty() ? plStringView("this platform") : m_Profiles[m_uiProfile].GetView();
  auto Over = [&](plStringView sCategory, plUInt64 uiUsed, plUInt64 uiLimit)
  {
    if (uiLimit == 0 || uiUsed <= uiLimit)
      return;

    plStringBuilder sBy;
    FormatMegabytes(uiUsed - uiLimit, sBy);

    Warning& warning = m_Warnings.ExpandAndGetRef();
    warning.m_sText.SetFormat("Over the {} budget by {} on {}", sCategory, sBy, sProfile);
    warning.m_bError = true;
  };

  Over("decoded", m_Loaded.m_uiDecodedBytes, m_Budget.m_uiDecodedBytes);
  Over("compressed", m_Loaded.m_uiCompressedBytes, m_Budget.m_uiCompressedBytes);
  Over("streaming", m_Loaded.GetStreamingBytes(), m_Budget.m_uiStreamingBytes);
  Over("total", m_Loaded.GetTotalBytes(), m_Budget.m_uiTotalBytes);

  for (plUInt32 b = 0; b < m_Banks.GetCount(); ++b)
  {
    if (!m_Banks[b].m_bOverBudget)
      continue;

    Warning& warning = m_Warnings.ExpandAndGetRef();
    warning.m_sText.SetFormat("Loading {} took the engine process over its budget (see the log)", m_Banks[b].m_sName);
    warning.m_bError = true;
    warning.m_iBank = static_cast<plInt32>(b);
  }
}
