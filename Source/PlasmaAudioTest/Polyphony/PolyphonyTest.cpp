#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <PlasmaAudioPlugin/MiniAudioAtlData.h>
#include <PlasmaAudioTest/Harness/OfflineAudio.h>

PL_CREATE_SIMPLE_TEST_GROUP(Polyphony);

namespace
{
  plAudioEventResourceHandle CreatePolyphonyEvent(plStringView sId, plUInt8 uiMaxInstances, plAudioEventStealingMode::Enum eStealing, plTime cooldown)
  {
    plAudioEventResourceDescriptor res = plAudioTestUtils::MakeSingleInstrumentEvent(plAudioTestUtils::MakeDc(4800, 0.25f));
    res.m_Desc.m_uiMaxInstances = uiMaxInstances;
    res.m_Desc.m_eStealing = eStealing;
    res.m_Desc.m_Cooldown = cooldown;
    res.m_Desc.m_StopFadeOut = plTime::MakeZero();
    return plAudioTestUtils::CreateEventResource(sId, std::move(res));
  }

  struct Voice
  {
    plPlasmaAudioEntityData m_Entity;
    plPlasmaAudioEventData m_Event;

    Voice() { m_Event.m_bLooping = true; } // held open until stopped, so only stealing ends an instance
  };
} // namespace

PL_CREATE_SIMPLE_TEST(Polyphony, StealingAndCooldown)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Oldest")
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    const plAudioEventResourceHandle hEvent = CreatePolyphonyEvent("PlasmaAudioTest:StealOldest", 2, plAudioEventStealingMode::Oldest, plTime::MakeZero());

    Voice voices[3];
    plPlasmaAudioEventManager manager;

    for (plUInt32 i = 0; i < 3; ++i)
    {
      PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:StealOldest", &voices[i].m_Entity, &voices[i].m_Event, engine.GetBusGraph(), engine.GetEngine()));
      engine.RenderBlocks(1, &manager, nullptr);
    }

    PL_TEST_INT(manager.GetInstanceCount(), 2);
    PL_TEST_BOOL(!manager.IsEventPlaying(&voices[0].m_Event));
    PL_TEST_BOOL(manager.IsEventPlaying(&voices[1].m_Event));
    PL_TEST_BOOL(manager.IsEventPlaying(&voices[2].m_Event));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Quietest")
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    const plAudioEventResourceHandle hEvent = CreatePolyphonyEvent("PlasmaAudioTest:StealQuietest", 2, plAudioEventStealingMode::Quietest, plTime::MakeZero());

    Voice voices[3];
    voices[0].m_Entity.m_fVolumeMultiplier = 1.0f;
    voices[1].m_Entity.m_fVolumeMultiplier = 0.25f;
    voices[2].m_Entity.m_fVolumeMultiplier = 1.0f;
    plPlasmaAudioEventManager manager;

    for (plUInt32 i = 0; i < 3; ++i)
    {
      PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:StealQuietest", &voices[i].m_Entity, &voices[i].m_Event, engine.GetBusGraph(), engine.GetEngine()));
      engine.RenderBlocks(1, &manager, nullptr);
    }

    PL_TEST_INT(manager.GetInstanceCount(), 2);
    PL_TEST_BOOL(manager.IsEventPlaying(&voices[0].m_Event));
    PL_TEST_BOOL(!manager.IsEventPlaying(&voices[1].m_Event));
    PL_TEST_BOOL(manager.IsEventPlaying(&voices[2].m_Event));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "None")
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    const plAudioEventResourceHandle hEvent = CreatePolyphonyEvent("PlasmaAudioTest:StealNone", 2, plAudioEventStealingMode::None, plTime::MakeZero());

    Voice voices[3];
    plPlasmaAudioEventManager manager;

    for (plUInt32 i = 0; i < 3; ++i)
    {
      // a rejected start is not an error
      PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:StealNone", &voices[i].m_Entity, &voices[i].m_Event, engine.GetBusGraph(), engine.GetEngine()));
      engine.RenderBlocks(1, &manager, nullptr);
    }

    PL_TEST_INT(manager.GetInstanceCount(), 2);
    PL_TEST_BOOL(manager.IsEventPlaying(&voices[0].m_Event));
    PL_TEST_BOOL(manager.IsEventPlaying(&voices[1].m_Event));
    PL_TEST_BOOL(!manager.IsEventPlaying(&voices[2].m_Event));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Cooldown")
  {
    plOfflineAudioEngine engine;
    if (!PL_TEST_BOOL(engine.IsValid()))
      return;

    const plAudioEventResourceHandle hEvent = CreatePolyphonyEvent("PlasmaAudioTest:Cooldown", 8, plAudioEventStealingMode::None, plTime::MakeFromMilliseconds(50));

    Voice voices[3];
    plPlasmaAudioEventManager manager;

    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:Cooldown", &voices[0].m_Entity, &voices[0].m_Event, engine.GetBusGraph(), engine.GetEngine()));
    PL_TEST_BOOL(manager.IsEventPlaying(&voices[0].m_Event));

    // 10 ms later: inside the cooldown, swallowed
    engine.RenderBlocks(1, &manager, nullptr);
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:Cooldown", &voices[1].m_Entity, &voices[1].m_Event, engine.GetBusGraph(), engine.GetEngine()));
    PL_TEST_BOOL(!manager.IsEventPlaying(&voices[1].m_Event));

    // 60 ms after the first start: accepted
    engine.RenderBlocks(5, &manager, nullptr);
    PL_TEST_RESULT(manager.ActivateEvent("PlasmaAudioTest:Cooldown", &voices[2].m_Entity, &voices[2].m_Event, engine.GetBusGraph(), engine.GetEngine()));
    PL_TEST_BOOL(manager.IsEventPlaying(&voices[2].m_Event));

    PL_TEST_INT(manager.GetInstanceCount(), 2);
  }
}
