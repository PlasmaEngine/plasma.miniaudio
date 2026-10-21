#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/Mutex.h>
#include <PlasmaAudioPlugin/Platform/AudioBackends.h>

namespace
{
  plMutex s_BackendMutex;
  plHybridArray<const plAudioCustomBackend*, 2> s_CustomBackends;

  /// The built-in backends compiled for this platform, in miniaudio's order; the custom slot is the registry's.
  plUInt32 GetBuiltInBackends(ma_backend (&out_backends)[MA_BACKEND_COUNT])
  {
    ma_backend backends[MA_BACKEND_COUNT];
    size_t uiCount = 0;
    if (ma_get_enabled_backends(backends, MA_BACKEND_COUNT, &uiCount) != MA_SUCCESS)
      return 0;

    plUInt32 uiBuiltIn = 0;
    for (size_t i = 0; i < uiCount; ++i)
    {
      if (backends[i] != ma_backend_custom)
        out_backends[uiBuiltIn++] = backends[i];
    }

    return uiBuiltIn;
  }
} // namespace

void plAudioBackendRegistry::RegisterCustomBackend(const plAudioCustomBackend* pBackend)
{
  PL_LOCK(s_BackendMutex);

  if (pBackend != nullptr && !s_CustomBackends.Contains(pBackend))
    s_CustomBackends.PushBack(pBackend);
}

void plAudioBackendRegistry::UnregisterCustomBackend(const plAudioCustomBackend* pBackend)
{
  PL_LOCK(s_BackendMutex);
  s_CustomBackends.RemoveAndCopy(pBackend);
}

void plAudioBackendRegistry::GetBackendNames(plDynamicArray<plString>& out_names)
{
  out_names.Clear();

  ma_backend builtIn[MA_BACKEND_COUNT];
  const plUInt32 uiBuiltIn = GetBuiltInBackends(builtIn);

  for (plUInt32 i = 0; i < uiBuiltIn; ++i)
  {
    out_names.PushBack(ma_get_backend_name(builtIn[i]));
  }

  PL_LOCK(s_BackendMutex);
  for (const plAudioCustomBackend* pBackend : s_CustomBackends)
  {
    out_names.PushBack(pBackend->GetName());
  }
}

plAudioBackendRegistry::Choice plAudioBackendRegistry::Resolve(plStringView sName)
{
  Choice choice;
  if (sName.IsEmpty())
    return choice;

  {
    PL_LOCK(s_BackendMutex);
    for (const plAudioCustomBackend* pBackend : s_CustomBackends)
    {
      if (pBackend->GetName().IsEqual_NoCase(sName))
      {
        choice.m_bFound = true;
        choice.m_Backend = ma_backend_custom;
        choice.m_pCustom = pBackend;
        return choice;
      }
    }
  }

  ma_backend builtIn[MA_BACKEND_COUNT];
  const plUInt32 uiBuiltIn = GetBuiltInBackends(builtIn);

  for (plUInt32 i = 0; i < uiBuiltIn; ++i)
  {
    if (sName.IsEqual_NoCase(ma_get_backend_name(builtIn[i])))
    {
      choice.m_bFound = true;
      choice.m_Backend = builtIn[i];
      return choice;
    }
  }

  return choice;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Platform_AudioBackends);
