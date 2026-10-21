#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <AudioSystem/Core/AudioSystemData.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/FileSystem/FileWriter.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <PlasmaAudioPlugin/Banks/AudioBankResource.h>
#include <PlasmaAudioPlugin/Banks/AudioPackedContent.h>
#include <PlasmaAudioPlugin/Clips/AudioClipBuilder.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResource.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResourceLoader.h>
#include <PlasmaAudioPlugin/Clips/AudioClipVoiceSource.h>
#include <PlasmaAudioPlugin/Events/AudioEventResource.h>
#include <PlasmaAudioPlugin/MiniAudioMiddleware.h>

#include <cmath>

PL_CREATE_SIMPLE_TEST_GROUP(AudioBank);

namespace
{
  plAudioClipSourcePcm MakeSine(plUInt32 uiFrames, double fFrequency, float fAmplitude)
  {
    plAudioClipSourcePcm source;
    source.m_uiChannels = 1;
    source.m_uiSampleRate = 48000;
    source.m_Samples.SetCountUninitialized(uiFrames);

    for (plUInt32 f = 0; f < uiFrames; ++f)
    {
      source.m_Samples[f] = fAmplitude * static_cast<float>(std::sin(2.0 * plMath::Pi<double>() * fFrequency * f / 48000.0));
    }

    return source;
  }

  plDynamicArray<plInt16> ToPcm16(const plAudioClipSourcePcm& source)
  {
    plDynamicArray<plInt16> pcm;
    pcm.SetCountUninitialized(source.m_Samples.GetCount());
    for (plUInt32 i = 0; i < source.m_Samples.GetCount(); ++i)
    {
      pcm[i] = static_cast<plInt16>(plMath::Clamp<plInt32>(static_cast<plInt32>(std::lround(plMath::Clamp(source.m_Samples[i], -1.0f, 1.0f) * 32767.0f)), -32768, 32767));
    }
    return pcm;
  }

  plInt32 MaxAbsDiff(plArrayPtr<const plInt16> a, plArrayPtr<const plInt16> b)
  {
    if (a.GetCount() != b.GetCount())
      return plMath::MaxValue<plInt32>();

    plInt32 iMax = 0;
    for (plUInt32 i = 0; i < a.GetCount(); ++i)
      iMax = plMath::Max(iMax, plMath::Abs(static_cast<plInt32>(a[i]) - static_cast<plInt32>(b[i])));
    return iMax;
  }

  plResult WriteAssetFile(plStringView sPath, plArrayPtr<const plUInt8> content)
  {
    plFileWriter file;
    PL_SUCCEED_OR_RETURN(file.Open(sPath));

    plAssetFileHeader assetHeader;
    assetHeader.SetFileHashAndVersion(1, 1);
    PL_SUCCEED_OR_RETURN(assetHeader.Write(file));

    return file.WriteBytes(content.GetPtr(), content.GetCount());
  }

  plResult WriteBankFile(plStringView sPath, const plAudioBankDesc& desc)
  {
    plDynamicArray<plUInt8> bytes;
    plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&bytes);
    plMemoryStreamWriter writer(&storage);
    PL_SUCCEED_OR_RETURN(desc.Save(writer));
    return WriteAssetFile(sPath, bytes);
  }

  void CreateTestClip(plStringView sId)
  {
    plAudioClipResourceDescriptor desc;
    desc.m_Header.m_uiFrames = 480;
    desc.m_Pcm = ToPcm16(MakeSine(480, 440.0, 0.5f));
    plResourceManager::CreateResource<plAudioClipResource>(sId, std::move(desc));
  }

  /// The bytes of an asset file: the asset header, then the content.
  plDynamicArray<plUInt8> MakeAssetBytes(plArrayPtr<const plUInt8> content)
  {
    plDynamicArray<plUInt8> bytes;
    plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&bytes);
    plMemoryStreamWriter writer(&storage);

    plAssetFileHeader assetHeader;
    assetHeader.SetFileHashAndVersion(1, 1);
    assetHeader.Write(writer).AssertSuccess();
    writer.WriteBytes(content.GetPtr(), content.GetCount()).AssertSuccess();
    return bytes;
  }

  /// Updates the middleware until the bank finished loading, for at most five seconds.
  bool WaitForBankReady(plPlasmaAudioMiddleware& ref_middleware, plAudioSystemDataID uiBankId)
  {
    for (plUInt32 i = 0; i < 5000; ++i)
    {
      ref_middleware.Update(plTime::MakeFromMilliseconds(1));
      if (ref_middleware.IsBankReady(uiBankId))
        return true;

      plThreadUtils::Sleep(plTime::MakeFromMilliseconds(1));
    }

    return false;
  }

  /// Updates the middleware until the bank's content is requested, for at most five seconds.
  bool WaitForBankContent(plPlasmaAudioMiddleware& ref_middleware, plAudioSystemDataID uiBankId)
  {
    for (plUInt32 i = 0; i < 5000; ++i)
    {
      ref_middleware.Update(plTime::MakeFromMilliseconds(1));
      if (ref_middleware.IsBankContentRequested(uiBankId))
        return true;

      plThreadUtils::Sleep(plTime::MakeFromMilliseconds(1));
    }

    return false;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(AudioBank, Desc)
{
  if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(">pltest/", "PlasmaAudioBankTest", "banktest", plDataDirUsage::AllowWrites)))
    return;

  PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioBankTest"));

  plAudioBankDesc desc;
  desc.m_Events.PushBack("{ 01234567-89ab-cdef-0123-456789abcdef }");
  desc.m_Events.PushBack("{ 11111111-2222-3333-4444-555555555555 }");
  desc.m_Clips.PushBack("{ aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee }");

  desc.m_Budget.m_uiDecodedBytes = 8 * 1024 * 1024;
  desc.m_Budget.m_uiStreamingBytes = 3 * 1024 * 1024;
  desc.m_Budget.m_uiTotalBytes = 48 * 1024 * 1024;
  desc.m_Packed.PushBack({"{ 01234567-89ab-cdef-0123-456789abcdef }", false, 0, 120});
  desc.m_Packed.PushBack({"{ aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee }", true, 120, 4000});

  PL_TEST_BLOCK(plTestBlock::Enabled, "Round trip")
  {
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    PL_TEST_RESULT(desc.Save(writer));

    plMemoryStreamReader reader(&storage);
    plAudioBankDesc loaded;
    PL_TEST_RESULT(loaded.Load(reader));

    PL_TEST_BOOL(loaded.m_Events == desc.m_Events);
    PL_TEST_BOOL(loaded.m_Clips == desc.m_Clips);
    PL_TEST_BOOL(loaded.m_Budget == desc.m_Budget);
    PL_TEST_BOOL(loaded.m_Packed == desc.m_Packed);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Version 1")
  {
    // a bank built before budgets and packing: the content lists only
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    writer.WriteVersion(1);
    writer << desc.m_Events.GetCount();
    for (const plString& sEvent : desc.m_Events)
      writer << sEvent;
    writer << desc.m_Clips.GetCount();
    for (const plString& sClip : desc.m_Clips)
      writer << sClip;

    plMemoryStreamReader reader(&storage);
    plAudioBankDesc loaded;
    loaded.m_Budget.m_uiTotalBytes = 1;
    PL_TEST_RESULT(loaded.Load(reader));

    PL_TEST_BOOL(loaded.m_Events == desc.m_Events);
    PL_TEST_BOOL(loaded.m_Clips == desc.m_Clips);
    PL_TEST_BOOL(!loaded.m_Budget.IsSet());
    PL_TEST_BOOL(loaded.m_Packed.IsEmpty());
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Excess")
  {
    plAudioMemoryUsage usage;
    usage.m_uiDecodedBytes = 12 * 1024 * 1024;
    usage.m_uiPrimingBytes = 1 * 1024 * 1024;
    usage.m_uiRingBytes = 1 * 1024 * 1024;

    plStringBuilder sExcess;
    usage.DescribeExcess(desc.m_Budget, sExcess);
    PL_TEST_STRING(sExcess, "decoded 12.0 MB of 8.0 MB");

    usage.m_uiRingBytes = 5 * 512 * 1024;
    usage.DescribeExcess(desc.m_Budget, sExcess);
    PL_TEST_STRING(sExcess, "decoded 12.0 MB of 8.0 MB, streaming 3.5 MB of 3.0 MB");

    plAudioMemoryBudget tiny;
    tiny.m_uiDecodedBytes = 100;
    usage.DescribeExcess(tiny, sExcess);
    PL_TEST_STRING(sExcess, "decoded 12.0 MB of 100 B");

    usage.DescribeExcess(plAudioMemoryBudget(), sExcess);
    PL_TEST_BOOL(sExcess.IsEmpty());
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Resource from file")
  {
    if (!PL_TEST_RESULT(WriteBankFile(":banktest/Desc.plBinAudioBank", desc)))
      return;

    plAudioBankResourceHandle hBank = plResourceManager::LoadResource<plAudioBankResource>(":banktest/Desc.plBinAudioBank");
    plResourceLock<plAudioBankResource> pBank(hBank, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pBank.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    PL_TEST_BOOL(pBank->GetDescriptor().m_Events == desc.m_Events);
    PL_TEST_BOOL(pBank->GetDescriptor().m_Clips == desc.m_Clips);
  }
}

PL_CREATE_SIMPLE_TEST(AudioBank, LoadUnload)
{
  if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(">pltest/", "PlasmaAudioBankTest", "banktest", plDataDirUsage::AllowWrites)))
    return;

  PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioBankTest"));

  plUniquePtr<plPlasmaAudioMiddleware> pMiddleware = PL_DEFAULT_NEW(plPlasmaAudioMiddleware);

  PL_TEST_BLOCK(plTestBlock::Enabled, "Bank created from a descriptor")
  {
    plResourceManager::CreateResource<plAudioEventResource>("PlasmaAudioTest:BankEvent", plAudioEventResourceDescriptor());
    CreateTestClip("PlasmaAudioTest:BankClip");

    {
      plAudioBankResourceDescriptor bankDesc;
      bankDesc.m_Desc.m_Events.PushBack("PlasmaAudioTest:BankEvent");
      bankDesc.m_Desc.m_Clips.PushBack("PlasmaAudioTest:BankClip");
      plResourceManager::CreateResource<plAudioBankResource>("PlasmaAudioTest:Bank", std::move(bankDesc));
    }

    const plAudioSystemDataID uiBankId = 1001;
    plAudioSystemStringBankData bankData(uiBankId, "PlasmaAudioTest:Bank");

    PL_TEST_RESULT(pMiddleware->LoadBank(&bankData));
    PL_TEST_BOOL(pMiddleware->IsBankContentRequested(uiBankId));

    // the bank's handles keep the resources alive
    plResourceManager::FreeAllUnusedResources();
    PL_TEST_BOOL(plResourceManager::GetExistingResource<plAudioBankResource>("PlasmaAudioTest:Bank").IsValid());
    PL_TEST_BOOL(plResourceManager::GetExistingResource<plAudioEventResource>("PlasmaAudioTest:BankEvent").IsValid());
    PL_TEST_BOOL(plResourceManager::GetExistingResource<plAudioClipResource>("PlasmaAudioTest:BankClip").IsValid());

    PL_TEST_RESULT(pMiddleware->UnloadBank(&bankData));
    PL_TEST_BOOL(!pMiddleware->IsBankContentRequested(uiBankId));

    plResourceManager::FreeAllUnusedResources();
    PL_TEST_BOOL(!plResourceManager::GetExistingResource<plAudioBankResource>("PlasmaAudioTest:Bank").IsValid());
    PL_TEST_BOOL(!plResourceManager::GetExistingResource<plAudioEventResource>("PlasmaAudioTest:BankEvent").IsValid());
    PL_TEST_BOOL(!plResourceManager::GetExistingResource<plAudioClipResource>("PlasmaAudioTest:BankClip").IsValid());
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Bank loaded from a file")
  {
    CreateTestClip("PlasmaAudioTest:FileBankClip");

    plAudioBankDesc desc;
    desc.m_Clips.PushBack("PlasmaAudioTest:FileBankClip");
    if (!PL_TEST_RESULT(WriteBankFile(":banktest/Load.plBinAudioBank", desc)))
      return;

    const plAudioSystemDataID uiBankId = 1002;
    plAudioSystemStringBankData bankData(uiBankId, ":banktest/Load.plBinAudioBank");

    PL_TEST_RESULT(pMiddleware->LoadBank(&bankData));

    // the content is requested by Update once the bank resource has been loaded
    PL_TEST_BOOL(WaitForBankContent(*pMiddleware, uiBankId));

    plResourceManager::FreeAllUnusedResources();
    PL_TEST_BOOL(plResourceManager::GetExistingResource<plAudioClipResource>("PlasmaAudioTest:FileBankClip").IsValid());

    PL_TEST_RESULT(pMiddleware->UnloadBank(&bankData));

    plResourceManager::FreeAllUnusedResources();
    PL_TEST_BOOL(!plResourceManager::GetExistingResource<plAudioClipResource>("PlasmaAudioTest:FileBankClip").IsValid());
  }
}

PL_CREATE_SIMPLE_TEST(AudioBank, Language)
{
  if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(">pltest/", "PlasmaAudioBankTest", "banktest", plDataDirUsage::AllowWrites)))
    return;

  PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioBankTest"));
  PL_SCOPE_EXIT(plAudioClipResourceLoader::SetLanguage(""));

  plUniquePtr<plPlasmaAudioMiddleware> pMiddleware = PL_DEFAULT_NEW(plPlasmaAudioMiddleware);
  pMiddleware->SetLanguage("").IgnoreResult();
  pMiddleware->Update(plTime::MakeZero());

  const plAudioClipSourcePcm english = MakeSine(24000, 440.0, 0.5f);
  const plAudioClipSourcePcm german = MakeSine(24000, 660.0, 0.25f);
  const plAudioClipSourcePcm* sources[] = {&english, &german};

  plAudioClipBuildSettings settings;
  settings.m_eCodec = plAudioClipCodec::Pcm16;
  settings.m_eLoadMode = plAudioClipLoadMode::DecompressOnLoad;
  settings.m_Languages.PushBack("");
  settings.m_Languages.PushBack("de");

  plDynamicArray<plUInt8> clip;
  {
    plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&clip);
    plMemoryStreamWriter writer(&storage);
    PL_TEST_RESULT(plAudioClipBuilder::Build(plArrayPtr<const plAudioClipSourcePcm* const>(sources), settings, writer));
  }

  if (!PL_TEST_RESULT(WriteAssetFile(":banktest/Speech.plBinAudioClip", clip)))
    return;

  const plDynamicArray<plInt16> expectedEnglish = ToPcm16(english);
  const plDynamicArray<plInt16> expectedGerman = ToPcm16(german);

  // held for the whole test, like a playing voice would
  plAudioClipResourceHandle hClip = plResourceManager::LoadResource<plAudioClipResource>(":banktest/Speech.plBinAudioClip");

  // The resource manager skips a reload while the load task of the clip still finishes (right after a blocking load
  // returned); the middleware tries again on its next updates. Updates until the clip is in sLanguage, at most 2 s.
  auto UpdateUntilLanguage = [&](plStringView sLanguage)
  {
    const plTime tEnd = plTime::Now() + plTime::MakeFromSeconds(2);
    while (plTime::Now() < tEnd)
    {
      {
        plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
        if (pClip.GetAcquireResult() == plResourceAcquireResult::Final && pClip->GetActiveLanguage() == sLanguage)
          return;
      }

      plThreadUtils::Sleep(plTime::MakeFromMilliseconds(1));
      pMiddleware->Update(plTime::MakeZero());
    }
  };

  PL_TEST_BLOCK(plTestBlock::Enabled, "Default language")
  {
    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    PL_TEST_STRING(pClip->GetActiveLanguage(), "");
    PL_TEST_INT(MaxAbsDiff(pClip->GetPcm(), expectedEnglish), 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "SetLanguage reloads loaded clips")
  {
    PL_TEST_RESULT(pMiddleware->SetLanguage("de"));

    // applied on the audio thread's next update
    PL_TEST_STRING(pMiddleware->GetLanguage(), "");
    pMiddleware->Update(plTime::MakeZero());
    PL_TEST_STRING(pMiddleware->GetLanguage(), "de");
    PL_TEST_STRING(plAudioClipResourceLoader::GetLanguage(), "de");

    UpdateUntilLanguage("de");

    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    PL_TEST_STRING(pClip->GetActiveLanguage(), "de");
    PL_TEST_INT(MaxAbsDiff(pClip->GetPcm(), expectedGerman), 0);

    plAudioClipVoiceSource voice;
    if (PL_TEST_RESULT(pClip->CreateVoiceSource(voice, nullptr)))
    {
      plDynamicArray<plInt16> played;
      played.SetCount(24000);
      ma_uint64 uiRead = 0;
      PL_TEST_BOOL(ma_data_source_read_pcm_frames(voice.m_pSource, played.GetData(), 24000, &uiRead) == MA_SUCCESS);
      PL_TEST_INT(uiRead, 24000);
      PL_TEST_INT(MaxAbsDiff(played, expectedGerman), 0);
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Back to the default language")
  {
    PL_TEST_RESULT(pMiddleware->SetLanguage(""));
    pMiddleware->Update(plTime::MakeZero());
    UpdateUntilLanguage("");

    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    PL_TEST_STRING(pClip->GetActiveLanguage(), "");
    PL_TEST_INT(MaxAbsDiff(pClip->GetPcm(), expectedEnglish), 0);
  }
}

PL_CREATE_SIMPLE_TEST(AudioBank, Packed)
{
  if (!PL_TEST_RESULT(plFileSystem::AddDataDirectory(">pltest/", "PlasmaAudioBankTest", "banktest", plDataDirUsage::AllowWrites)))
    return;

  PL_SCOPE_EXIT(plFileSystem::RemoveDataDirectoryGroup("PlasmaAudioBankTest"));

  plUniquePtr<plPlasmaAudioMiddleware> pMiddleware = PL_DEFAULT_NEW(plPlasmaAudioMiddleware);

  // an event and a clip that exist only inside the bank file
  const plStringView sEventId = ":banktest/PackedOnly.plBinAudioEvent";
  const plStringView sClipId = ":banktest/PackedOnly.plBinAudioClip";

  const plAudioClipSourcePcm source = MakeSine(4800, 330.0, 0.5f);
  const plDynamicArray<plInt16> expected = ToPcm16(source);

  plDynamicArray<plUInt8> clipFile;
  {
    plAudioClipBuildSettings settings;
    settings.m_eCodec = plAudioClipCodec::Pcm16;
    settings.m_eLoadMode = plAudioClipLoadMode::DecompressOnLoad;

    const plAudioClipSourcePcm* sources[] = {&source};
    plDynamicArray<plUInt8> clip;
    plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&clip);
    plMemoryStreamWriter writer(&storage);
    PL_TEST_RESULT(plAudioClipBuilder::Build(plArrayPtr<const plAudioClipSourcePcm* const>(sources), settings, writer));
    clipFile = MakeAssetBytes(clip);
  }

  plDynamicArray<plUInt8> eventFile;
  {
    plAudioEventDesc eventDesc;
    eventDesc.m_fMaxDistance = 77.0f;

    plDynamicArray<plUInt8> event;
    plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&event);
    plMemoryStreamWriter writer(&storage);
    PL_TEST_RESULT(eventDesc.Save(writer));
    writer << plUInt32(0); // no embedded samples
    eventFile = MakeAssetBytes(event);
  }

  plAudioBankDesc desc;
  desc.m_Events.PushBack(sEventId);
  desc.m_Clips.PushBack(sClipId);
  desc.m_Packed.PushBack({sEventId, false, 0, eventFile.GetCount()});
  desc.m_Packed.PushBack({sClipId, true, eventFile.GetCount(), clipFile.GetCount()});

  plDynamicArray<plUInt8> packedData = eventFile;
  packedData.PushBackRange(clipFile);

  {
    plDynamicArray<plUInt8> bank;
    plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&bank);
    plMemoryStreamWriter writer(&storage);
    PL_TEST_RESULT(desc.Save(writer));
    PL_TEST_RESULT(writer.WriteBytes(packedData.GetData(), packedData.GetCount()));

    if (!PL_TEST_RESULT(WriteAssetFile(":banktest/Packed.plBinAudioBank", bank)))
      return;
  }

  PL_TEST_BOOL(!plFileSystem::ExistsFile(sClipId));
  PL_TEST_BOOL(!plFileSystem::ExistsFile(sEventId));

  const plAudioSystemDataID uiBankId = 1003;
  plAudioSystemStringBankData bankData(uiBankId, ":banktest/Packed.plBinAudioBank");

  PL_TEST_BLOCK(plTestBlock::Enabled, "Load from the bank file")
  {
    PL_TEST_RESULT(pMiddleware->LoadBank(&bankData));
    if (!PL_TEST_BOOL(WaitForBankReady(*pMiddleware, uiBankId)))
      return;

    PL_TEST_INT(plAudioPackedContent::GetCount(), 2);

    plAudioClipResourceHandle hClip = plResourceManager::LoadResource<plAudioClipResource>(sClipId);
    {
      plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
      if (PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      {
        PL_TEST_INT(MaxAbsDiff(pClip->GetPcm(), expected), 0);
      }
    }

    plAudioEventResourceHandle hEvent = plResourceManager::LoadResource<plAudioEventResource>(sEventId);
    {
      plResourceLock<plAudioEventResource> pEvent(hEvent, plResourceAcquireMode::BlockTillLoaded);
      if (PL_TEST_BOOL(pEvent.GetAcquireResult() == plResourceAcquireResult::Final))
      {
        PL_TEST_FLOAT(pEvent->GetDescriptor().m_fMaxDistance, 77.0f, 0.0f);
      }
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Unload")
  {
    PL_TEST_RESULT(pMiddleware->UnloadBank(&bankData));
    PL_TEST_INT(plAudioPackedContent::GetCount(), 0);

    plResourceManager::FreeAllUnusedResources();
    PL_TEST_BOOL(!plResourceManager::GetExistingResource<plAudioClipResource>(sClipId).IsValid());
    PL_TEST_BOOL(!plResourceManager::GetExistingResource<plAudioEventResource>(sEventId).IsValid());
  }
}

PL_CREATE_SIMPLE_TEST(AudioBank, Budget)
{
  plUniquePtr<plPlasmaAudioMiddleware> pMiddleware = PL_DEFAULT_NEW(plPlasmaAudioMiddleware);

  // 480 frames of decoded mono PCM: 960 bytes
  CreateTestClip("PlasmaAudioTest:BudgetClip");

  auto MakeBank = [](plStringView sId, plUInt64 uiDecodedBudget)
  {
    plAudioBankResourceDescriptor bankDesc;
    bankDesc.m_Desc.m_Clips.PushBack("PlasmaAudioTest:BudgetClip");
    bankDesc.m_Desc.m_Budget.m_uiDecodedBytes = uiDecodedBudget;
    plResourceManager::CreateResource<plAudioBankResource>(sId, std::move(bankDesc));
  };

  PL_TEST_BLOCK(plTestBlock::Enabled, "Over the budget")
  {
    MakeBank("PlasmaAudioTest:TightBank", 100);

    const plAudioSystemDataID uiBankId = 1004;
    plAudioSystemStringBankData bankData(uiBankId, "PlasmaAudioTest:TightBank");

    PL_TEST_RESULT(pMiddleware->LoadBank(&bankData));
    if (!PL_TEST_BOOL(WaitForBankReady(*pMiddleware, uiBankId)))
      return;

    PL_TEST_INT(pMiddleware->GetMemoryBudget().m_uiDecodedBytes, 100);
    PL_TEST_BOOL(pMiddleware->GetMemoryUsage().m_uiDecodedBytes >= 960);
    PL_TEST_BOOL(pMiddleware->IsBankOverBudget(uiBankId));

    PL_TEST_RESULT(pMiddleware->UnloadBank(&bankData));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Within the budget")
  {
    MakeBank("PlasmaAudioTest:RoomyBank", 1024ull * 1024 * 1024);

    const plAudioSystemDataID uiBankId = 1005;
    plAudioSystemStringBankData bankData(uiBankId, "PlasmaAudioTest:RoomyBank");

    PL_TEST_RESULT(pMiddleware->LoadBank(&bankData));
    if (!PL_TEST_BOOL(WaitForBankReady(*pMiddleware, uiBankId)))
      return;

    PL_TEST_INT(pMiddleware->GetMemoryBudget().m_uiDecodedBytes, 1024ull * 1024 * 1024);
    PL_TEST_BOOL(!pMiddleware->IsBankOverBudget(uiBankId));

    PL_TEST_RESULT(pMiddleware->UnloadBank(&bankData));
  }

  plResourceManager::FreeAllUnusedResources();
}
