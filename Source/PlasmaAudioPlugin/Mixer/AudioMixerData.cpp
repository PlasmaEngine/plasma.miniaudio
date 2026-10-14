#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <PlasmaAudioPlugin/Mixer/AudioMixerData.h>

#include <Foundation/IO/Stream.h>
#include <Foundation/Math/Math.h>

static constexpr plTypeVersion kAudioMixerDescVersion = 1;

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioEffectType, 1)
  PL_ENUM_CONSTANTS(plAudioEffectType::LowPass, plAudioEffectType::HighPass, plAudioEffectType::Eq3, plAudioEffectType::Delay, plAudioEffectType::Reverb, plAudioEffectType::Compressor, plAudioEffectType::Limiter)
PL_END_STATIC_REFLECTED_ENUM;

PL_BEGIN_STATIC_REFLECTED_ENUM(plAudioSnapshotTarget, 1)
  PL_ENUM_CONSTANTS(plAudioSnapshotTarget::BusVolume, plAudioSnapshotTarget::BusPitch, plAudioSnapshotTarget::SendLevel, plAudioSnapshotTarget::EffectParam)
PL_END_STATIC_REFLECTED_ENUM;
// clang-format on

void plAudioEffectDesc::InitDefaults()
{
  for (float& f : m_fParams)
    f = 0.0f;

  switch (m_eType)
  {
    case plAudioEffectType::LowPass:
      m_fParams[0] = 22000.0f;
      m_fParams[1] = 2.0f;
      break;
    case plAudioEffectType::HighPass:
      m_fParams[0] = 10.0f;
      m_fParams[1] = 2.0f;
      break;
    case plAudioEffectType::Eq3:
      m_fParams[1] = 120.0f;  // low shelf freq
      m_fParams[3] = 1000.0f; // mid freq
      m_fParams[4] = 1.0f;    // mid Q
      m_fParams[6] = 8000.0f; // high shelf freq
      break;
    case plAudioEffectType::Delay:
      m_fParams[0] = 250.0f; // delay ms
      m_fParams[1] = 0.3f;   // feedback
      m_fParams[2] = 0.35f;  // wet
      m_fParams[3] = 1.0f;   // dry
      break;
    case plAudioEffectType::Reverb:
      m_fParams[0] = 0.5f; // room size
      m_fParams[1] = 0.5f; // damping
      m_fParams[2] = 1.0f; // width
      m_fParams[3] = 1.0f; // wet (aux buses are usually 100% wet)
      m_fParams[4] = 0.0f; // dry
      break;
    case plAudioEffectType::Compressor:
      m_fParams[0] = -18.0f; // threshold dB
      m_fParams[1] = 3.0f;   // ratio
      m_fParams[2] = 10.0f;  // attack ms
      m_fParams[3] = 120.0f; // release ms
      m_fParams[4] = 0.0f;   // makeup dB
      break;
    case plAudioEffectType::Limiter:
      m_fParams[0] = -0.3f; // ceiling dB
      m_fParams[1] = 60.0f; // release ms
      break;
  }
}

void plAudioEffectDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream << static_cast<plUInt8>(m_eType);
  for (float f : m_fParams)
    inout_stream << f;
}

void plAudioEffectDesc::Load(plStreamReader& inout_stream)
{
  plUInt8 uiType = 0;
  inout_stream >> uiType;
  m_eType = static_cast<plAudioEffectType::Enum>(uiType);
  for (float& f : m_fParams)
    inout_stream >> f;
}

void plAudioBusSendDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_uiTargetBus;
  inout_stream << m_fLevelDb;
}

void plAudioBusSendDesc::Load(plStreamReader& inout_stream)
{
  inout_stream >> m_uiTargetBus;
  inout_stream >> m_fLevelDb;
}

void plAudioBusDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_sName;
  inout_stream << m_uiParent;
  inout_stream << m_fVolumeDb;
  inout_stream << m_fPitch;
  inout_stream << m_bIsAux;
  inout_stream << m_sEnvironmentName;

  inout_stream << m_Effects.GetCount();
  for (const auto& effect : m_Effects)
    effect.Save(inout_stream);

  inout_stream << m_Sends.GetCount();
  for (const auto& send : m_Sends)
    send.Save(inout_stream);
}

void plAudioBusDesc::Load(plStreamReader& inout_stream)
{
  inout_stream >> m_sName;
  inout_stream >> m_uiParent;
  inout_stream >> m_fVolumeDb;
  inout_stream >> m_fPitch;
  inout_stream >> m_bIsAux;
  inout_stream >> m_sEnvironmentName;

  plUInt32 uiCount = 0;
  inout_stream >> uiCount;
  m_Effects.SetCount(uiCount);
  for (auto& effect : m_Effects)
    effect.Load(inout_stream);

  inout_stream >> uiCount;
  m_Sends.SetCount(uiCount);
  for (auto& send : m_Sends)
    send.Load(inout_stream);
}

void plAudioVcaDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_sName;
  inout_stream << m_fVolumeDb;
  inout_stream << m_Buses.GetCount();
  for (plUInt8 uiBus : m_Buses)
    inout_stream << uiBus;
}

void plAudioVcaDesc::Load(plStreamReader& inout_stream)
{
  inout_stream >> m_sName;
  inout_stream >> m_fVolumeDb;
  plUInt32 uiCount = 0;
  inout_stream >> uiCount;
  m_Buses.SetCount(uiCount);
  for (plUInt8& uiBus : m_Buses)
    inout_stream >> uiBus;
}

void plAudioSnapshotOverrideDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_uiBus;
  inout_stream << static_cast<plUInt8>(m_eTarget);
  inout_stream << m_uiIndexA;
  inout_stream << m_uiIndexB;
  inout_stream << m_fValue;
}

void plAudioSnapshotOverrideDesc::Load(plStreamReader& inout_stream)
{
  inout_stream >> m_uiBus;
  plUInt8 uiTarget = 0;
  inout_stream >> uiTarget;
  m_eTarget = static_cast<plAudioSnapshotTarget::Enum>(uiTarget);
  inout_stream >> m_uiIndexA;
  inout_stream >> m_uiIndexB;
  inout_stream >> m_fValue;
}

void plAudioSnapshotDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_sName;
  inout_stream << m_FadeIn;
  inout_stream << m_FadeOut;
  inout_stream << m_uiPriority;
  inout_stream << m_Overrides.GetCount();
  for (const auto& ovr : m_Overrides)
    ovr.Save(inout_stream);
}

void plAudioSnapshotDesc::Load(plStreamReader& inout_stream)
{
  inout_stream >> m_sName;
  inout_stream >> m_FadeIn;
  inout_stream >> m_FadeOut;
  inout_stream >> m_uiPriority;
  plUInt32 uiCount = 0;
  inout_stream >> uiCount;
  m_Overrides.SetCount(uiCount);
  for (auto& ovr : m_Overrides)
    ovr.Load(inout_stream);
}

plResult plAudioMixerDesc::Save(plStreamWriter& inout_stream) const
{
  inout_stream.WriteVersion(kAudioMixerDescVersion);

  inout_stream << m_Buses.GetCount();
  for (const auto& bus : m_Buses)
    bus.Save(inout_stream);

  inout_stream << m_Vcas.GetCount();
  for (const auto& vca : m_Vcas)
    vca.Save(inout_stream);

  inout_stream << m_Snapshots.GetCount();
  for (const auto& snapshot : m_Snapshots)
    snapshot.Save(inout_stream);

  return PL_SUCCESS;
}

plResult plAudioMixerDesc::Load(plStreamReader& inout_stream)
{
  const plTypeVersion uiVersion = inout_stream.ReadVersion(kAudioMixerDescVersion);
  PL_IGNORE_UNUSED(uiVersion);

  plUInt32 uiCount = 0;

  inout_stream >> uiCount;
  m_Buses.SetCount(uiCount);
  for (auto& bus : m_Buses)
    bus.Load(inout_stream);

  inout_stream >> uiCount;
  m_Vcas.SetCount(uiCount);
  for (auto& vca : m_Vcas)
    vca.Load(inout_stream);

  inout_stream >> uiCount;
  m_Snapshots.SetCount(uiCount);
  for (auto& snapshot : m_Snapshots)
    snapshot.Load(inout_stream);

  if (m_Buses.IsEmpty())
    MakeDefault();

  return PL_SUCCESS;
}

plUInt32 plAudioMixerDesc::FindBusByName(plStringView sName) const
{
  for (plUInt32 i = 0; i < m_Buses.GetCount(); ++i)
  {
    if (m_Buses[i].m_sName.IsEqual_NoCase(sName))
      return i;
  }
  return plInvalidIndex;
}

plUInt32 plAudioMixerDesc::FindAuxBusForEnvironment(plStringView sEnvironmentName) const
{
  for (plUInt32 i = 0; i < m_Buses.GetCount(); ++i)
  {
    if (m_Buses[i].m_bIsAux && m_Buses[i].m_sEnvironmentName.IsEqual_NoCase(sEnvironmentName))
      return i;
  }
  return plInvalidIndex;
}

plUInt32 plAudioMixerDesc::FindVcaByName(plStringView sName) const
{
  for (plUInt32 i = 0; i < m_Vcas.GetCount(); ++i)
  {
    if (m_Vcas[i].m_sName.IsEqual_NoCase(sName))
      return i;
  }
  return plInvalidIndex;
}

plUInt32 plAudioMixerDesc::FindSnapshotByName(plStringView sName) const
{
  for (plUInt32 i = 0; i < m_Snapshots.GetCount(); ++i)
  {
    if (m_Snapshots[i].m_sName.IsEqual_NoCase(sName))
      return i;
  }
  return plInvalidIndex;
}

void plAudioMixerDesc::MakeDefault()
{
  m_Buses.Clear();
  m_Vcas.Clear();
  m_Snapshots.Clear();

  auto& master = m_Buses.ExpandAndGetRef();
  master.m_sName = "Master";
  master.m_uiParent = 0xFF;
}

float plAudioMixerUtils::DbToLinear(float fDb)
{
  if (fDb <= -60.0f)
    return 0.0f;

  return plMath::Pow(10.0f, fDb / 20.0f);
}

float plAudioMixerUtils::LinearToDb(float fLinear)
{
  if (fLinear <= 0.001f)
    return -60.0f;

  return 20.0f * plMath::Log10(fLinear);
}

float plAudioMixerUtils::SemitonesToPitch(float fSemitones)
{
  return plMath::Pow(2.0f, fSemitones / 12.0f);
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Mixer_AudioMixerData);
