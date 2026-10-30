#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

PL_CREATE_SIMPLE_TEST_GROUP(DeviceRecovery);

PL_CREATE_SIMPLE_TEST(DeviceRecovery, PlayingEventSurvivesEngineReplacement)
{
  plOfflineAudioEngine engine;
  if (!PL_TEST_BOOL(engine.IsValid()))
    return;

  // a looping DC event is audible at every frame, so any gap in the output is a lost voice
  const plAudioEventResourceHandle hEvent = plAudioTestUtils::CreateEventResource("PlasmaAudioTest:DeviceRecovery", plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(9600, 0.5f)));

  plPlasmaAudioEventData eventData;
  eventData.m_bLooping = true;
  plPlasmaAudioEventManager manager;
  PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:DeviceRecovery", nullptr, &eventData, engine.GetBusGraph(), engine.GetEngine()));

  plDynamicArray<float> before;
  engine.RenderBlocks(10, &manager, &before);

  const plUInt32 uiOnset = plAudioTestUtils::FindFirstAbove(before, 0.4f);
  if (!PL_TEST_BOOL_MSG(uiOnset != plInvalidIndex, "the event never became audible before the device change"))
    return;

  PL_TEST_BOOL(manager.IsEventPlaying(&eventData));

  PL_TEST_BLOCK(plTestBlock::Enabled, "Detached state is retained")
  {
    manager.DetachForDeviceRecovery();
    PL_TEST_BOOL(manager.IsEventPlaying(&eventData));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "A failed restore keeps the event for another attempt")
  {
    PL_TEST_BOOL(manager.RestoreAfterDeviceRecovery(nullptr).Failed());
    PL_TEST_BOOL(manager.IsEventPlaying(&eventData));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Restore resumes playback without a dropout")
  {
    PL_TEST_RESULT(manager.RestoreAfterDeviceRecovery(engine.GetEngine()));
    PL_TEST_BOOL(manager.IsEventPlaying(&eventData));

    plDynamicArray<float> after;
    engine.RenderBlocks(10, &manager, &after);

    const plUInt32 uiResumed = plAudioTestUtils::FindFirstAbove(after, 0.4f);
    if (!PL_TEST_BOOL_MSG(uiResumed != plInvalidIndex, "the event stayed silent after the device was restored"))
      return;

    PL_TEST_BOOL_MSG(uiResumed <= 4 * plOfflineAudioEngine::BlockFrames, "output resumed after %u frames", uiResumed);

    plUInt32 uiDropouts = 0;
    for (plUInt32 i = uiResumed; i < after.GetCount(); ++i)
    {
      if (after[i] < 0.4f)
        ++uiDropouts;
    }
    PL_TEST_INT(uiDropouts, 0);
  }
}
