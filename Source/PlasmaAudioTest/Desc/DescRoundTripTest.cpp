#include <PlasmaAudioTest/PlasmaAudioTestPCH.h>

#include <AudioSystem/Mixer/AudioMixerData.h>
#include <PlasmaAudioPlugin/Events/AudioEventResource.h>

PL_CREATE_SIMPLE_TEST_GROUP(Desc);

namespace
{
  plAudioEventDesc MakePopulatedEventDesc()
  {
    plAudioEventDesc desc;
    desc.m_sOutputBus = "SFX/Footsteps";
    desc.m_uiMaxInstances = 3;
    desc.m_eStealing = plAudioEventStealingMode::Quietest;
    desc.m_uiPriority = 37;
    desc.m_eVirtualMode = plAudioEventVirtualMode::Restart;
    desc.m_Cooldown = plTime::MakeFromMilliseconds(75);
    desc.m_FadeIn = plTime::MakeFromMilliseconds(120);
    desc.m_StopFadeOut = plTime::MakeFromMilliseconds(250);
    desc.m_bLoopRegion = true;
    desc.m_LoopStart = plTime::MakeFromMilliseconds(500);
    desc.m_LoopEnd = plTime::MakeFromMilliseconds(2500);
    desc.m_bSpatialize = false;
    desc.m_fMinDistance = 2.5f;
    desc.m_fMaxDistance = 40.0f;
    desc.m_fRolloff = 1.75f;
    desc.m_fDopplerFactor = 0.5f;

    {
      plAudioEventInstrumentDesc& instrument = desc.m_Instruments.ExpandAndGetRef();
      instrument.m_sName = "Steps";
      instrument.m_StartDelay = plTime::MakeFromMilliseconds(10);
      instrument.m_ePlaylistMode = plAudioEventPlaylistMode::Shuffle;
      instrument.m_bLoop = true;
      instrument.m_bMute = false;
      instrument.m_sSwitchState = "Surface/Grass";
      instrument.m_MinRetriggerDelay = plTime::MakeFromMilliseconds(200);
      instrument.m_MaxRetriggerDelay = plTime::MakeFromMilliseconds(400);
      instrument.m_fVolumeDb = -3.0f;
      instrument.m_fVolumeRandDb = 1.5f;
      instrument.m_fPitchSemitones = 2.0f;
      instrument.m_fPitchRandSemitones = 0.5f;

      auto& e0 = instrument.m_Playlist.ExpandAndGetRef();
      e0.m_uiSample = 0;
      e0.m_fWeight = 1.0f;
      auto& e1 = instrument.m_Playlist.ExpandAndGetRef();
      e1.m_uiSample = 1;
      e1.m_fWeight = 2.5f;
      auto& e2 = instrument.m_Playlist.ExpandAndGetRef();
      e2.m_uiSample = plAudioEventSilenceSample;
      e2.m_fWeight = 0.25f;
      auto& e3 = instrument.m_Playlist.ExpandAndGetRef();
      e3.m_uiSample = plAudioEventSilenceSample;
      e3.m_fWeight = 0.75f;
      e3.m_sClip = "{ 0b3c1a2e-7d44-4f1e-9a55-21c3d7e9f001 }";
    }

    {
      plAudioEventInstrumentDesc& instrument = desc.m_Instruments.ExpandAndGetRef();
      instrument.m_sName = "Rustle";
      instrument.m_StartDelay = plTime::MakeFromMilliseconds(900);
      instrument.m_ePlaylistMode = plAudioEventPlaylistMode::Sequential;
      instrument.m_bLoop = false;
      instrument.m_bMute = true;
      instrument.m_fVolumeDb = -12.0f;
      instrument.m_fPitchSemitones = -1.0f;

      auto& e0 = instrument.m_Playlist.ExpandAndGetRef();
      e0.m_uiSample = 2;
      e0.m_fWeight = 1.0f;
    }

    {
      auto& p = desc.m_Parameters.ExpandAndGetRef();
      p.m_sName = "Speed";
      p.m_fMin = 0.0f;
      p.m_fMax = 10.0f;
      p.m_fDefault = 2.0f;
    }
    {
      auto& p = desc.m_Parameters.ExpandAndGetRef();
      p.m_sName = "Distance";
      p.m_fMin = 0.0f;
      p.m_fMax = 50.0f;
      p.m_fDefault = 0.0f;
    }

    {
      auto& a = desc.m_Automations.ExpandAndGetRef();
      a.m_uiParameter = 0;
      a.m_eTarget = plAudioEventAutomationTarget::EventPitch;
      a.m_Curve.AddControlPoint(0.0).m_Position.y = 0.8;
      a.m_Curve.AddControlPoint(5.0).m_Position.y = 1.0;
      a.m_Curve.AddControlPoint(10.0).m_Position.y = 1.4;
    }

    desc.m_SampleNames.PushBack("Step_A.wav");
    desc.m_SampleNames.PushBack("Step_B.wav");
    desc.m_SampleNames.PushBack("Rustle.ogg");

    return desc;
  }

  void CompareInstrument(const plAudioEventInstrumentDesc& a, const plAudioEventInstrumentDesc& b, bool bHasMute, bool bHasSwitch, bool bHasClip)
  {
    PL_TEST_STRING(a.m_sName, b.m_sName);
    PL_TEST_DOUBLE(a.m_StartDelay.GetSeconds(), b.m_StartDelay.GetSeconds(), 0.0);
    PL_TEST_INT(a.m_ePlaylistMode, b.m_ePlaylistMode);
    PL_TEST_BOOL(a.m_bLoop == b.m_bLoop);
    PL_TEST_BOOL(a.m_bMute == (bHasMute ? b.m_bMute : false));
    PL_TEST_STRING(a.m_sSwitchState, bHasSwitch ? b.m_sSwitchState.GetView() : plStringView());
    PL_TEST_DOUBLE(a.m_MinRetriggerDelay.GetSeconds(), b.m_MinRetriggerDelay.GetSeconds(), 0.0);
    PL_TEST_DOUBLE(a.m_MaxRetriggerDelay.GetSeconds(), b.m_MaxRetriggerDelay.GetSeconds(), 0.0);
    PL_TEST_FLOAT(a.m_fVolumeDb, b.m_fVolumeDb, 0.0f);
    PL_TEST_FLOAT(a.m_fVolumeRandDb, b.m_fVolumeRandDb, 0.0f);
    PL_TEST_FLOAT(a.m_fPitchSemitones, b.m_fPitchSemitones, 0.0f);
    PL_TEST_FLOAT(a.m_fPitchRandSemitones, b.m_fPitchRandSemitones, 0.0f);

    if (PL_TEST_INT(a.m_Playlist.GetCount(), b.m_Playlist.GetCount()))
    {
      for (plUInt32 i = 0; i < a.m_Playlist.GetCount(); ++i)
      {
        PL_TEST_INT(a.m_Playlist[i].m_uiSample, b.m_Playlist[i].m_uiSample);
        PL_TEST_FLOAT(a.m_Playlist[i].m_fWeight, b.m_Playlist[i].m_fWeight, 0.0f);
        PL_TEST_STRING(a.m_Playlist[i].m_sClip, bHasClip ? b.m_Playlist[i].m_sClip.GetView() : plStringView());
      }
    }
  }

  /// \param uiVersion the stream version the loaded desc came from; fields newer than it must hold their defaults
  void CompareEventDesc(const plAudioEventDesc& loaded, const plAudioEventDesc& reference, plTypeVersion uiVersion)
  {
    const plAudioEventDesc defaults;

    PL_TEST_STRING(loaded.m_sOutputBus, reference.m_sOutputBus);
    PL_TEST_INT(loaded.m_uiMaxInstances, reference.m_uiMaxInstances);
    PL_TEST_INT(loaded.m_eStealing, reference.m_eStealing);

    const plAudioEventDesc& v6 = uiVersion >= 6 ? reference : defaults;
    PL_TEST_INT(loaded.m_uiPriority, v6.m_uiPriority);
    PL_TEST_INT(loaded.m_eVirtualMode, v6.m_eVirtualMode);

    PL_TEST_DOUBLE(loaded.m_Cooldown.GetSeconds(), reference.m_Cooldown.GetSeconds(), 0.0);
    PL_TEST_DOUBLE(loaded.m_StopFadeOut.GetSeconds(), reference.m_StopFadeOut.GetSeconds(), 0.0);

    const plAudioEventDesc& v3 = uiVersion >= 3 ? reference : defaults;
    PL_TEST_DOUBLE(loaded.m_FadeIn.GetSeconds(), v3.m_FadeIn.GetSeconds(), 0.0);
    PL_TEST_BOOL(loaded.m_bLoopRegion == v3.m_bLoopRegion);
    PL_TEST_DOUBLE(loaded.m_LoopStart.GetSeconds(), v3.m_LoopStart.GetSeconds(), 0.0);
    PL_TEST_DOUBLE(loaded.m_LoopEnd.GetSeconds(), v3.m_LoopEnd.GetSeconds(), 0.0);

    PL_TEST_BOOL(loaded.m_bSpatialize == reference.m_bSpatialize);
    PL_TEST_FLOAT(loaded.m_fMinDistance, reference.m_fMinDistance, 0.0f);
    PL_TEST_FLOAT(loaded.m_fMaxDistance, reference.m_fMaxDistance, 0.0f);
    PL_TEST_FLOAT(loaded.m_fRolloff, reference.m_fRolloff, 0.0f);
    PL_TEST_FLOAT(loaded.m_fDopplerFactor, reference.m_fDopplerFactor, 0.0f);

    if (PL_TEST_INT(loaded.m_Instruments.GetCount(), reference.m_Instruments.GetCount()))
    {
      for (plUInt32 i = 0; i < loaded.m_Instruments.GetCount(); ++i)
      {
        CompareInstrument(loaded.m_Instruments[i], reference.m_Instruments[i], uiVersion >= 2, uiVersion >= 4, uiVersion >= 5);
      }
    }

    if (PL_TEST_INT(loaded.m_Parameters.GetCount(), reference.m_Parameters.GetCount()))
    {
      for (plUInt32 i = 0; i < loaded.m_Parameters.GetCount(); ++i)
      {
        PL_TEST_STRING(loaded.m_Parameters[i].m_sName, reference.m_Parameters[i].m_sName);
        PL_TEST_FLOAT(loaded.m_Parameters[i].m_fMin, reference.m_Parameters[i].m_fMin, 0.0f);
        PL_TEST_FLOAT(loaded.m_Parameters[i].m_fMax, reference.m_Parameters[i].m_fMax, 0.0f);
        PL_TEST_FLOAT(loaded.m_Parameters[i].m_fDefault, reference.m_Parameters[i].m_fDefault, 0.0f);
      }
    }

    if (PL_TEST_INT(loaded.m_Automations.GetCount(), reference.m_Automations.GetCount()))
    {
      for (plUInt32 i = 0; i < loaded.m_Automations.GetCount(); ++i)
      {
        const auto& a = loaded.m_Automations[i];
        const auto& b = reference.m_Automations[i];
        PL_TEST_INT(a.m_uiParameter, b.m_uiParameter);
        PL_TEST_INT(a.m_eTarget, b.m_eTarget);

        if (PL_TEST_INT(a.m_Curve.GetNumControlPoints(), b.m_Curve.GetNumControlPoints()))
        {
          for (plUInt32 cp = 0; cp < a.m_Curve.GetNumControlPoints(); ++cp)
          {
            PL_TEST_DOUBLE(a.m_Curve.GetControlPoint(cp).m_Position.x, b.m_Curve.GetControlPoint(cp).m_Position.x, 0.0);
            PL_TEST_DOUBLE(a.m_Curve.GetControlPoint(cp).m_Position.y, b.m_Curve.GetControlPoint(cp).m_Position.y, 0.0);
          }
        }

        // the loaded curve is ready to evaluate (Load builds the linear approximation)
        PL_TEST_DOUBLE(a.m_Curve.Evaluate(10.0), 1.4, 1e-5);
      }
    }

    if (PL_TEST_INT(loaded.m_SampleNames.GetCount(), reference.m_SampleNames.GetCount()))
    {
      for (plUInt32 i = 0; i < loaded.m_SampleNames.GetCount(); ++i)
      {
        PL_TEST_STRING(loaded.m_SampleNames[i], reference.m_SampleNames[i]);
      }
    }
  }

  /// \brief Writes the desc in the field order of an older stream version (1..5), the way that version's Save did.
  void WriteLegacyEventDesc(plStreamWriter& inout_stream, const plAudioEventDesc& desc, plTypeVersion uiVersion)
  {
    inout_stream.WriteVersion(uiVersion);

    inout_stream << desc.m_sOutputBus;
    inout_stream << desc.m_uiMaxInstances;
    inout_stream << static_cast<plUInt8>(desc.m_eStealing);
    inout_stream << desc.m_Cooldown;
    inout_stream << desc.m_StopFadeOut;

    if (uiVersion >= 3)
    {
      inout_stream << desc.m_FadeIn;
      inout_stream << desc.m_bLoopRegion;
      inout_stream << desc.m_LoopStart;
      inout_stream << desc.m_LoopEnd;
    }

    inout_stream << desc.m_bSpatialize;
    inout_stream << desc.m_fMinDistance;
    inout_stream << desc.m_fMaxDistance;
    inout_stream << desc.m_fRolloff;
    inout_stream << desc.m_fDopplerFactor;

    inout_stream << desc.m_Instruments.GetCount();
    for (const auto& instrument : desc.m_Instruments)
    {
      inout_stream << instrument.m_sName;
      inout_stream << instrument.m_StartDelay;
      inout_stream << static_cast<plUInt8>(instrument.m_ePlaylistMode);
      inout_stream << instrument.m_bLoop;

      if (uiVersion >= 2)
        inout_stream << instrument.m_bMute;

      if (uiVersion >= 4)
        inout_stream << instrument.m_sSwitchState;

      inout_stream << instrument.m_MinRetriggerDelay;
      inout_stream << instrument.m_MaxRetriggerDelay;
      inout_stream << instrument.m_fVolumeDb;
      inout_stream << instrument.m_fVolumeRandDb;
      inout_stream << instrument.m_fPitchSemitones;
      inout_stream << instrument.m_fPitchRandSemitones;

      inout_stream << instrument.m_Playlist.GetCount();
      for (const auto& entry : instrument.m_Playlist)
      {
        inout_stream << entry.m_uiSample;
        inout_stream << entry.m_fWeight;

        if (uiVersion >= 5)
          inout_stream << entry.m_sClip;
      }
    }

    inout_stream << desc.m_Parameters.GetCount();
    for (const auto& parameter : desc.m_Parameters)
    {
      inout_stream << parameter.m_sName;
      inout_stream << parameter.m_fMin;
      inout_stream << parameter.m_fMax;
      inout_stream << parameter.m_fDefault;
    }

    inout_stream << desc.m_Automations.GetCount();
    for (const auto& automation : desc.m_Automations)
    {
      inout_stream << automation.m_uiParameter;
      inout_stream << static_cast<plUInt8>(automation.m_eTarget);
      automation.m_Curve.Save(inout_stream);
    }

    inout_stream << desc.m_SampleNames.GetCount();
    for (const auto& sName : desc.m_SampleNames)
      inout_stream << sName;
  }

  plAudioMixerDesc MakePopulatedMixerDesc()
  {
    plAudioMixerDesc desc;

    {
      auto& bus = desc.m_Buses.ExpandAndGetRef();
      bus.m_sName = "Master";
      bus.m_uiParent = 0xFF;
      bus.m_fVolumeDb = -1.0f;

      auto& limiter = bus.m_Effects.ExpandAndGetRef();
      limiter.m_eType = plAudioEffectType::Limiter;
      limiter.InitDefaults();
    }
    {
      auto& bus = desc.m_Buses.ExpandAndGetRef();
      bus.m_sName = "SFX";
      bus.m_uiParent = 0;
      bus.m_fVolumeDb = -4.5f;
      bus.m_fPitch = 1.25f;

      auto& lp = bus.m_Effects.ExpandAndGetRef();
      lp.m_eType = plAudioEffectType::LowPass;
      lp.InitDefaults();
      lp.m_fParams[0] = 8000.0f;

      auto& eq = bus.m_Effects.ExpandAndGetRef();
      eq.m_eType = plAudioEffectType::Eq3;
      eq.InitDefaults();
      eq.m_fParams[0] = 3.0f;
      eq.m_fParams[5] = -2.0f;

      auto& send = bus.m_Sends.ExpandAndGetRef();
      send.m_uiTargetBus = 2;
      send.m_fLevelDb = -9.0f;
    }
    {
      auto& bus = desc.m_Buses.ExpandAndGetRef();
      bus.m_sName = "CaveReverb";
      bus.m_uiParent = 0;
      bus.m_bIsAux = true;
      bus.m_sEnvironmentName = "Cave";

      auto& reverb = bus.m_Effects.ExpandAndGetRef();
      reverb.m_eType = plAudioEffectType::Reverb;
      reverb.InitDefaults();
      reverb.m_fParams[0] = 0.8f;
    }

    {
      auto& vca = desc.m_Vcas.ExpandAndGetRef();
      vca.m_sName = "Game";
      vca.m_fVolumeDb = -6.0f;
      vca.m_Buses.PushBack(1);
      vca.m_Buses.PushBack(2);
    }

    {
      auto& snapshot = desc.m_Snapshots.ExpandAndGetRef();
      snapshot.m_sName = "Underwater";
      snapshot.m_FadeIn = plTime::MakeFromMilliseconds(300);
      snapshot.m_FadeOut = plTime::MakeFromMilliseconds(700);
      snapshot.m_uiPriority = 4;

      auto& o0 = snapshot.m_Overrides.ExpandAndGetRef();
      o0.m_uiBus = 1;
      o0.m_eTarget = plAudioSnapshotTarget::BusVolume;
      o0.m_fValue = -18.0f;

      auto& o1 = snapshot.m_Overrides.ExpandAndGetRef();
      o1.m_uiBus = 1;
      o1.m_eTarget = plAudioSnapshotTarget::BusPitch;
      o1.m_fValue = 0.8f;

      auto& o2 = snapshot.m_Overrides.ExpandAndGetRef();
      o2.m_uiBus = 1;
      o2.m_eTarget = plAudioSnapshotTarget::SendLevel;
      o2.m_uiIndexA = 0;
      o2.m_fValue = 0.0f;

      auto& o3 = snapshot.m_Overrides.ExpandAndGetRef();
      o3.m_uiBus = 1;
      o3.m_eTarget = plAudioSnapshotTarget::EffectParam;
      o3.m_uiIndexA = 0;
      o3.m_uiIndexB = 0;
      o3.m_fValue = 600.0f;
    }
    {
      auto& snapshot = desc.m_Snapshots.ExpandAndGetRef();
      snapshot.m_sName = "Pause";
      snapshot.m_FadeIn = plTime::MakeZero();
      snapshot.m_FadeOut = plTime::MakeFromSeconds(1.0);
      snapshot.m_uiPriority = 9;

      auto& o0 = snapshot.m_Overrides.ExpandAndGetRef();
      o0.m_uiBus = 0;
      o0.m_eTarget = plAudioSnapshotTarget::BusVolume;
      o0.m_fValue = -30.0f;
    }

    return desc;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Desc, EventRoundTrip)
{
  const plAudioEventDesc reference = MakePopulatedEventDesc();

  PL_TEST_BLOCK(plTestBlock::Enabled, "Current version")
  {
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    PL_TEST_RESULT(reference.Save(writer));

    plMemoryStreamReader reader(&storage);
    plAudioEventDesc loaded;
    PL_TEST_RESULT(loaded.Load(reader));

    CompareEventDesc(loaded, reference, 6);

    // the whole stream was consumed
    plUInt8 uiExtra = 0;
    PL_TEST_INT(reader.ReadBytes(&uiExtra, 1), 0);
  }

  for (plTypeVersion uiVersion = 1; uiVersion <= 5; ++uiVersion)
  {
    plStringBuilder sBlock;
    sBlock.SetFormat("Version {}", uiVersion);

    PL_TEST_BLOCK(plTestBlock::Enabled, sBlock.GetData())
    {
      plDefaultMemoryStreamStorage storage;
      plMemoryStreamWriter writer(&storage);
      WriteLegacyEventDesc(writer, reference, uiVersion);

      plMemoryStreamReader reader(&storage);
      plAudioEventDesc loaded;
      PL_TEST_RESULT(loaded.Load(reader));

      CompareEventDesc(loaded, reference, uiVersion);

      plUInt8 uiExtra = 0;
      PL_TEST_INT(reader.ReadBytes(&uiExtra, 1), 0);
    }
  }
}

PL_CREATE_SIMPLE_TEST(Desc, MixerRoundTrip)
{
  const plAudioMixerDesc reference = MakePopulatedMixerDesc();

  plDefaultMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);
  PL_TEST_RESULT(reference.Save(writer));

  plMemoryStreamReader reader(&storage);
  plAudioMixerDesc loaded;
  PL_TEST_RESULT(loaded.Load(reader));

  plUInt8 uiExtra = 0;
  PL_TEST_INT(reader.ReadBytes(&uiExtra, 1), 0);

  PL_TEST_BLOCK(plTestBlock::Enabled, "Buses")
  {
    if (PL_TEST_INT(loaded.m_Buses.GetCount(), reference.m_Buses.GetCount()))
    {
      for (plUInt32 b = 0; b < loaded.m_Buses.GetCount(); ++b)
      {
        const auto& l = loaded.m_Buses[b];
        const auto& r = reference.m_Buses[b];
        PL_TEST_STRING(l.m_sName, r.m_sName);
        PL_TEST_INT(l.m_uiParent, r.m_uiParent);
        PL_TEST_FLOAT(l.m_fVolumeDb, r.m_fVolumeDb, 0.0f);
        PL_TEST_FLOAT(l.m_fPitch, r.m_fPitch, 0.0f);
        PL_TEST_BOOL(l.m_bIsAux == r.m_bIsAux);
        PL_TEST_STRING(l.m_sEnvironmentName, r.m_sEnvironmentName);

        if (PL_TEST_INT(l.m_Effects.GetCount(), r.m_Effects.GetCount()))
        {
          for (plUInt32 e = 0; e < l.m_Effects.GetCount(); ++e)
          {
            PL_TEST_INT(l.m_Effects[e].m_eType, r.m_Effects[e].m_eType);
            for (plUInt32 p = 0; p < plAudioEffectMaxParams; ++p)
              PL_TEST_FLOAT(l.m_Effects[e].m_fParams[p], r.m_Effects[e].m_fParams[p], 0.0f);
          }
        }

        if (PL_TEST_INT(l.m_Sends.GetCount(), r.m_Sends.GetCount()))
        {
          for (plUInt32 s = 0; s < l.m_Sends.GetCount(); ++s)
          {
            PL_TEST_INT(l.m_Sends[s].m_uiTargetBus, r.m_Sends[s].m_uiTargetBus);
            PL_TEST_FLOAT(l.m_Sends[s].m_fLevelDb, r.m_Sends[s].m_fLevelDb, 0.0f);
          }
        }
      }
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "VCAs")
  {
    if (PL_TEST_INT(loaded.m_Vcas.GetCount(), reference.m_Vcas.GetCount()))
    {
      for (plUInt32 v = 0; v < loaded.m_Vcas.GetCount(); ++v)
      {
        PL_TEST_STRING(loaded.m_Vcas[v].m_sName, reference.m_Vcas[v].m_sName);
        PL_TEST_FLOAT(loaded.m_Vcas[v].m_fVolumeDb, reference.m_Vcas[v].m_fVolumeDb, 0.0f);
        PL_TEST_BOOL(loaded.m_Vcas[v].m_Buses == reference.m_Vcas[v].m_Buses);
      }
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Snapshots")
  {
    if (PL_TEST_INT(loaded.m_Snapshots.GetCount(), reference.m_Snapshots.GetCount()))
    {
      for (plUInt32 s = 0; s < loaded.m_Snapshots.GetCount(); ++s)
      {
        const auto& l = loaded.m_Snapshots[s];
        const auto& r = reference.m_Snapshots[s];
        PL_TEST_STRING(l.m_sName, r.m_sName);
        PL_TEST_DOUBLE(l.m_FadeIn.GetSeconds(), r.m_FadeIn.GetSeconds(), 0.0);
        PL_TEST_DOUBLE(l.m_FadeOut.GetSeconds(), r.m_FadeOut.GetSeconds(), 0.0);
        PL_TEST_INT(l.m_uiPriority, r.m_uiPriority);

        if (PL_TEST_INT(l.m_Overrides.GetCount(), r.m_Overrides.GetCount()))
        {
          for (plUInt32 o = 0; o < l.m_Overrides.GetCount(); ++o)
          {
            PL_TEST_INT(l.m_Overrides[o].m_uiBus, r.m_Overrides[o].m_uiBus);
            PL_TEST_INT(l.m_Overrides[o].m_eTarget, r.m_Overrides[o].m_eTarget);
            PL_TEST_INT(l.m_Overrides[o].m_uiIndexA, r.m_Overrides[o].m_uiIndexA);
            PL_TEST_INT(l.m_Overrides[o].m_uiIndexB, r.m_Overrides[o].m_uiIndexB);
            PL_TEST_FLOAT(l.m_Overrides[o].m_fValue, r.m_Overrides[o].m_fValue, 0.0f);
          }
        }
      }
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Lookups")
  {
    PL_TEST_INT(loaded.FindBusByName("sfx"), 1);
    PL_TEST_INT(loaded.FindAuxBusForEnvironment("Cave"), 2);
    PL_TEST_INT(loaded.FindVcaByName("GAME"), 0);
    PL_TEST_INT(loaded.FindSnapshotByName("Pause"), 1);
    PL_TEST_INT(loaded.FindBusByName("Missing"), plInvalidIndex);
  }
}
