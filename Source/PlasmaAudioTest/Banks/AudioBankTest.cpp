#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <AudioSystem/Core/AudioSystemData.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/FileSystem/FileWriter.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <PlasmaAudioPlugin/Banks/AudioBankResource.h>
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

    plResourceLock<plAudioClipResource> pClip(hClip, plResourceAcquireMode::BlockTillLoaded);
    if (!PL_TEST_BOOL(pClip.GetAcquireResult() == plResourceAcquireResult::Final))
      return;

    PL_TEST_STRING(pClip->GetActiveLanguage(), "");
    PL_TEST_INT(MaxAbsDiff(pClip->GetPcm(), expectedEnglish), 0);
  }
}
