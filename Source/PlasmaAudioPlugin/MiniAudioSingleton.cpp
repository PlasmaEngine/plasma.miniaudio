#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <PlasmaAudioPlugin/MiniAudioSingleton.h>

#if PL_ENABLED(PL_PLATFORM_WINDOWS)
#  include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#  include <objbase.h>
#  pragma comment(lib, "ole32.lib")
#endif

plCVarFloat cvar_MiniAudioMasterVolume("PlasmaAudio.Volume", 1.0f, plCVarFlags::Save, "Overall volume for all PlasmaAudio output");
plCVarBool cvar_MiniAudioMute("PlasmaAudio.Mute", false, plCVarFlags::Default, "Whether PlasmaAudio output is muted");
plCVarBool cvar_MiniAudioPause("PlasmaAudio.Pause", false, plCVarFlags::Default, "Whether PlasmaAudio output is paused");
plCVarString cvar_PlasmaAudioOutputDevice("PlasmaAudio.OutputDevice", "", plCVarFlags::Save, "Playback device name; empty = system default");

PL_IMPLEMENT_SINGLETON(plMiniAudioSingleton);

static plMiniAudioSingleton g_MiniAudioSingleton;

/// seconds between attempts to get a real device back while running without one
static constexpr double s_fDeviceRetryIntervalSeconds = 3.0;

/// weight of the newest mix in the smoothed DSP load
static constexpr float s_fDspLoadSmoothing = 0.2f;

namespace
{
  // miniaudio's heap goes through the "PlasmaAudio/miniaudio" allocator. miniaudio reallocates without passing the old
  // size, so every block starts with a header that stores it.
  constexpr size_t s_uiMaHeaderBytes = 16;

  void* MaMalloc(size_t uiBytes, void* pUserData)
  {
    plAllocator* pAllocator = static_cast<plAllocator*>(pUserData);
    plUInt8* pBlock = static_cast<plUInt8*>(pAllocator->Allocate(uiBytes + s_uiMaHeaderBytes, s_uiMaHeaderBytes));
    if (pBlock == nullptr)
      return nullptr;

    *reinterpret_cast<size_t*>(pBlock) = uiBytes;
    return pBlock + s_uiMaHeaderBytes;
  }

  void MaFree(void* p, void* pUserData)
  {
    if (p == nullptr)
      return;

    static_cast<plAllocator*>(pUserData)->Deallocate(static_cast<plUInt8*>(p) - s_uiMaHeaderBytes);
  }

  void* MaRealloc(void* p, size_t uiBytes, void* pUserData)
  {
    if (p == nullptr)
      return MaMalloc(uiBytes, pUserData);

    const size_t uiOldBytes = *reinterpret_cast<const size_t*>(static_cast<const plUInt8*>(p) - s_uiMaHeaderBytes);

    void* pNew = MaMalloc(uiBytes, pUserData);
    if (pNew == nullptr)
      return nullptr;

    plMemoryUtils::Copy(static_cast<plUInt8*>(pNew), static_cast<const plUInt8*>(p), plMath::Min(uiOldBytes, uiBytes));
    MaFree(p, pUserData);
    return pNew;
  }

#if PL_ENABLED(PL_PLATFORM_WINDOWS)
  /// Joins the COM apartment the calling thread already has, else initializes COM with dwCoInit.
  HRESULT JoinComApartment(LPVOID pvReserved, DWORD dwCoInit)
  {
    const HRESULT hr = CoInitializeEx(pvReserved, dwCoInit);
    if (hr != RPC_E_CHANGED_MODE)
      return hr;

    // the thread is in the other apartment type: a reference to that one (S_FALSE) balances a later CoUninitialize
    return CoInitializeEx(pvReserved, (dwCoInit & COINIT_APARTMENTTHREADED) != 0 ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED);
  }

  /// Replaces CoInitializeEx in the miniaudio context. miniaudio's WASAPI backend wraps the creation of its device
  /// enumerator in CoInitializeEx(COINIT_MULTITHREADED) and an unconditional CoUninitialize. On a thread in a
  /// single-threaded apartment the initialization fails, adds no reference, and the uninitialization then drops one of
  /// the thread's own references until COM is gone from the thread. Joining the existing apartment keeps the pair
  /// balanced.
  HRESULT WINAPI MaCoInitializeEx(LPVOID pvReserved, DWORD dwCoInit)
  {
    return JoinComApartment(pvReserved, dwCoInit);
  }
#endif

  /// Makes sure COM is initialized on the calling thread before it touches the miniaudio context or engine.
  ///
  /// miniaudio's WASAPI backend creates its device enumerator inside its own CoInitializeEx / CoUninitialize pair, which
  /// leaves the enumerator unusable (and starting a device crashes) on a thread without COM of its own. The first call
  /// on a thread joins the multithreaded apartment, or the single-threaded apartment the thread already is in (e.g. the
  /// Qt main thread). That reference is kept for the thread's lifetime and never released.
  ///
  /// A thread counts as done once COM is initialized (S_OK, S_FALSE) or already runs in an apartment that could not be
  /// joined (RPC_E_CHANGED_MODE); no reference is held then either. Any other failure is retried by the next call, and
  /// warned about once per thread.
  void EnsureComOnThread()
  {
#if PL_ENABLED(PL_PLATFORM_WINDOWS)
    thread_local bool s_bComInitialized = false;
    thread_local bool s_bComFailureLogged = false;
    if (s_bComInitialized)
      return;

    const HRESULT hr = JoinComApartment(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
    {
      s_bComInitialized = true;
      return;
    }

    if (!s_bComFailureLogged)
    {
      s_bComFailureLogged = true;
      plLog::Warning("PlasmaAudio: COM could not be initialized on this thread (0x{0}).", plArgU(static_cast<plUInt32>(hr), 8, true, 16));
    }
#endif
  }
} // namespace

plMiniAudioSingleton::Data::Data()
  : m_Allocator("PlasmaAudio/miniaudio", plFoundation::GetDefaultAllocator())
{
  m_AllocationCallbacks.pUserData = &m_Allocator;
  m_AllocationCallbacks.onMalloc = &MaMalloc;
  m_AllocationCallbacks.onRealloc = &MaRealloc;
  m_AllocationCallbacks.onFree = &MaFree;
}

plMiniAudioSingleton::plMiniAudioSingleton()
  : m_SingletonRegistrar(this)
{
  m_bInitialized = false;
}

plMiniAudioSingleton::~plMiniAudioSingleton() = default;

void plMiniAudioSingleton::Startup()
{
  if (m_bInitialized)
    return;

  m_pData = PL_DEFAULT_NEW(Data);

  PL_LOCK(m_pData->m_Mutex);

  EnsureComOnThread();

  ma_context_config contextConfig = ma_context_config_init();
  contextConfig.allocationCallbacks = m_pData->m_AllocationCallbacks;

  if (ma_context_init(nullptr, 0, &contextConfig, &m_pData->m_Context) != MA_SUCCESS)
  {
    plLog::Error("PlasmaAudio: failed to initialize the audio context.");
    return;
  }
  m_bContextInitialized = true;

#if PL_ENABLED(PL_PLATFORM_WINDOWS)
  // the context's own reference was taken with the original function and is released only when it succeeded
  m_pData->m_Context.win32.CoInitializeEx = reinterpret_cast<ma_proc>(&MaCoInitializeEx);
#endif

  {
    PL_LOCK(m_pData->m_OutputDeviceMutex);
    m_pData->m_sRequestedOutputDevice = cvar_PlasmaAudioOutputDevice.GetValue();
  }
  cvar_PlasmaAudioOutputDevice.m_CVarEvents.AddEventHandler(plMakeDelegate(&plMiniAudioSingleton::OnOutputDeviceCVarChanged, this));

  if (InitEngine().Failed())
    return;

  m_bInitialized = true;
  UpdateSound(plTime::MakeZero());
}

void plMiniAudioSingleton::Shutdown()
{
  if (m_pData == nullptr)
    return;

  {
    PL_LOCK(m_pData->m_Mutex);

    if (m_bInitialized)
    {
      m_bInitialized = false;
      UninitEngine();
    }

    m_pData->m_SoundGroups.Clear();

    if (m_bContextInitialized)
    {
      cvar_PlasmaAudioOutputDevice.m_CVarEvents.RemoveEventHandler(plMakeDelegate(&plMiniAudioSingleton::OnOutputDeviceCVarChanged, this));

      ma_context_uninit(&m_pData->m_Context);
      m_bContextInitialized = false;
    }
  }

  // finally delete all data
  m_pData.Clear();
}

void plMiniAudioSingleton::DeviceNotificationCallback(const ma_device_notification* pNotification)
{
  // runs on the device thread: only flags are set here, the audio thread acts on them
  plMiniAudioSingleton* pThis = GetSingleton();
  if (pThis == nullptr || pNotification == nullptr)
    return;

  switch (pNotification->type)
  {
    case ma_device_notification_type_rerouted:
      pThis->m_bDeviceRerouted = true;
      break;

    case ma_device_notification_type_stopped:
      if (!pThis->m_bDeviceStopExpected)
        pThis->m_bDeviceLost = true;
      break;

    default:
      break;
  }
}

void plMiniAudioSingleton::TimedDataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 uiFrameCount)
{
  // runs on the device thread; the device only runs between InitEngine and UninitEngine, while the singleton exists
  plMiniAudioSingleton* pThis = GetSingleton();

  const plTime tStart = plTime::Now();
  pThis->m_OriginalDataCallback(pDevice, pOutput, pInput, uiFrameCount);
  const plTime tEnd = plTime::Now();

  if (pDevice->sampleRate > 0)
  {
    pThis->RecordMixTime(tEnd - tStart, static_cast<double>(uiFrameCount) / pDevice->sampleRate);
  }
}

void plMiniAudioSingleton::RecordMixTime(plTime mixTime, double fAudioSeconds)
{
  m_fLastMixMicroseconds.store(static_cast<float>(mixTime.GetMicroseconds()), std::memory_order_relaxed);

  if (fAudioSeconds <= 0.0)
    return;

  const float fLoad = static_cast<float>(mixTime.GetSeconds() / fAudioSeconds);
  const float fOld = m_fDspLoad.load(std::memory_order_relaxed);
  m_fDspLoad.store(fOld + s_fDspLoadSmoothing * (fLoad - fOld), std::memory_order_relaxed);
}

void plMiniAudioSingleton::OnOutputDeviceCVarChanged(const plCVarEvent& e)
{
  if (e.m_EventType == plCVarEvent::ValueChanged)
  {
    // runs on the thread that set the cvar; the audio thread only reads this copy
    {
      PL_LOCK(m_pData->m_OutputDeviceMutex);
      m_pData->m_sRequestedOutputDevice = cvar_PlasmaAudioOutputDevice.GetValue();
    }
    m_bDeviceChangeRequested = true;
  }
}

plResult plMiniAudioSingleton::InitEngine()
{
  // Startup runs on the thread that starts the audio system, a device re-init on the audio thread
  EnsureComOnThread();

  m_bDeviceChangeRequested = false;
  m_bDeviceRerouted = false;
  m_pData->m_fHeadlessFrameRemainder = 0.0;

  plString sRequested;
  {
    PL_LOCK(m_pData->m_OutputDeviceMutex);
    sRequested = m_pData->m_sRequestedOutputDevice;
  }

  ma_device_id deviceId;
  bool bDeviceFound = false;

  if (!sRequested.IsEmpty())
  {
    ma_device_info* pPlaybackInfos = nullptr;
    ma_uint32 uiPlaybackCount = 0;
    if (ma_context_get_devices(&m_pData->m_Context, &pPlaybackInfos, &uiPlaybackCount, nullptr, nullptr) == MA_SUCCESS)
    {
      for (ma_uint32 i = 0; i < uiPlaybackCount; ++i)
      {
        if (plStringView(pPlaybackInfos[i].name).IsEqual_NoCase(sRequested))
        {
          deviceId = pPlaybackInfos[i].id;
          bDeviceFound = true;
          break;
        }
      }
    }

    if (!bDeviceFound)
    {
      plLog::Warning("PlasmaAudio: output device '{0}' not found, using the system default.", sRequested);
    }
  }

  // What the engine allocates (its node graph, the device through the context, every ma_sound) goes through the
  // "PlasmaAudio/miniaudio" allocator; nodes initialized with null allocation callbacks do not. A voice's ma_sound
  // still allocates its node heap on every start: ma_sound_init_ex() takes no caller-provided heap, so sounds cannot
  // be preallocated.
  ma_engine_config cfg = ma_engine_config_init();
  cfg.pContext = &m_pData->m_Context;
  cfg.allocationCallbacks = m_pData->m_AllocationCallbacks;
  cfg.pPlaybackDeviceID = bDeviceFound ? &deviceId : nullptr;
  cfg.notificationCallback = &plMiniAudioSingleton::DeviceNotificationCallback;
  cfg.listenerCount = MaxListeners;
  cfg.channels = 0; // native channel count
  cfg.noAutoStart = MA_TRUE; // started below, once the data callback is wrapped

  m_bDeviceStopExpected = false;
  m_OriginalDataCallback = nullptr;
  m_fDspLoad.store(0.0f, std::memory_order_relaxed);
  m_fLastMixMicroseconds.store(0.0f, std::memory_order_relaxed);

  bool bDeviceEngine = ma_engine_init(&cfg, &m_pData->m_Engine) == MA_SUCCESS;

  if (bDeviceEngine)
  {
    // the device is not running yet, so its data callback can be swapped for the timing wrapper
    ma_device* pDevice = ma_engine_get_device(&m_pData->m_Engine);
    m_OriginalDataCallback = pDevice->onData;
    pDevice->onData = &plMiniAudioSingleton::TimedDataCallback;

    if (ma_engine_start(&m_pData->m_Engine) != MA_SUCCESS)
    {
      plLog::Warning("PlasmaAudio: the output device '{0}' could not be started.", pDevice->playback.name);
      m_bDeviceStopExpected = true;
      ma_engine_uninit(&m_pData->m_Engine);
      m_bDeviceStopExpected = false;
      m_OriginalDataCallback = nullptr;
      bDeviceEngine = false;
    }
  }

  if (bDeviceEngine)
  {
    m_bHeadless = false;
    plLog::Info("PlasmaAudio: output device '{0}'.", ma_engine_get_device(&m_pData->m_Engine)->playback.name);
  }
  else
  {
    // no usable playback device: run the graph without output so voices and nodes stay valid,
    // and keep checking for a device (IsDeviceReinitRequired)
    cfg.pPlaybackDeviceID = nullptr;
    cfg.noDevice = MA_TRUE;
    cfg.channels = 2;
    cfg.sampleRate = 48000;

    if (ma_engine_init(&cfg, &m_pData->m_Engine) != MA_SUCCESS)
    {
      plLog::Error("PlasmaAudio: failed to initialize the audio engine.");
      return PL_FAILURE;
    }

    m_bHeadless = true;
    m_LastReinitCheck = plTime::Now();
    plLog::Warning("PlasmaAudio: no playback device available, audio output is silent.");
  }

  m_bDeviceLost = false;
  m_bEnginePaused = false;

  if (plMiniAudioDsp::InitClockNode(&m_pData->m_Engine, &m_pData->m_ClockNode).Failed())
  {
    plLog::Warning("PlasmaAudio: failed to create the clock node; delayed sounds only start while other audio plays.");
  }

  for (auto& group : m_pData->m_SoundGroups)
  {
    ma_sound_group_init(&m_pData->m_Engine, 0, nullptr, group.m_pGroup.Borrow());
    ma_sound_group_set_volume(group.m_pGroup.Borrow(), group.m_fVolume);
  }

  for (plUInt8 i = 0; i < MaxListeners; ++i)
  {
    ApplyListenerSlot(i);
  }
  ApplyListenerEnabledStates();

  return PL_SUCCESS;
}

void plMiniAudioSingleton::UninitEngine()
{
  m_bDeviceStopExpected = true;

  for (auto& group : m_pData->m_SoundGroups)
  {
    ma_sound_group_uninit(group.m_pGroup.Borrow());
  }

  plMiniAudioDsp::UninitClockNode(&m_pData->m_ClockNode);

  ma_engine_uninit(&m_pData->m_Engine);
}

bool plMiniAudioSingleton::IsDeviceReinitRequired()
{
  if (!m_bInitialized)
    return false;

  if (m_bDeviceChangeRequested)
    return true;

  if (!m_bDeviceLost && !m_bHeadless)
    return false;

  if (!m_bHeadless)
    return true;

  // without a device: re-init only once the context reports a playback device again
  const plTime now = plTime::Now();
  if (now - m_LastReinitCheck < plTime::MakeFromSeconds(s_fDeviceRetryIntervalSeconds))
    return false;

  m_LastReinitCheck = now;

  PL_LOCK(m_pData->m_Mutex);
  EnsureComOnThread();
  ma_device_info* pPlaybackInfos = nullptr;
  ma_uint32 uiPlaybackCount = 0;
  return ma_context_get_devices(&m_pData->m_Context, &pPlaybackInfos, &uiPlaybackCount, nullptr, nullptr) == MA_SUCCESS && uiPlaybackCount > 0;
}

plResult plMiniAudioSingleton::ReinitDevice()
{
  if (!m_bInitialized)
    return PL_FAILURE;

  PL_LOCK(m_pData->m_Mutex);

  UninitEngine();

  if (InitEngine().Failed())
  {
    m_bInitialized = false;
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

void plMiniAudioSingleton::GetPlaybackDeviceNames(plDynamicArray<plString>& out_names) const
{
  out_names.Clear();

  if (m_pData == nullptr || !m_bContextInitialized)
    return;

  PL_LOCK(m_pData->m_Mutex);
  EnsureComOnThread();

  ma_device_info* pPlaybackInfos = nullptr;
  ma_uint32 uiPlaybackCount = 0;
  if (ma_context_get_devices(&m_pData->m_Context, &pPlaybackInfos, &uiPlaybackCount, nullptr, nullptr) != MA_SUCCESS)
    return;

  for (ma_uint32 i = 0; i < uiPlaybackCount; ++i)
  {
    out_names.PushBack(pPlaybackInfos[i].name);
  }
}

void plMiniAudioSingleton::SetNumListeners(plUInt8 uiNumListeners)
{
  m_uiNumListeners = plMath::Clamp<plUInt8>(uiNumListeners, 1, MaxListeners);

  if (m_bInitialized)
  {
    ApplyListenerEnabledStates();
  }
}

plUInt8 plMiniAudioSingleton::GetNumListeners() const
{
  return m_uiNumListeners;
}

void plMiniAudioSingleton::LoadConfiguration(plStringView sFile)
{
}

void plMiniAudioSingleton::SetOverridePlatform(plStringView sPlatform)
{
}

void plMiniAudioSingleton::UpdateSound(plTime delta)
{
  if (!m_bInitialized)
    return;

  PL_LOCK(m_pData->m_Mutex);

  if (m_bDeviceRerouted.exchange(false))
  {
    if (const ma_device* pDevice = ma_engine_get_device(&m_pData->m_Engine))
    {
      plLog::Info("PlasmaAudio: output rerouted to '{0}'.", pDevice->playback.name);
    }
  }

  if (cvar_MiniAudioPause)
  {
    if (!m_bEnginePaused && !m_bHeadless)
    {
      // the device stop this causes is ours, not a device loss
      m_bDeviceStopExpected = true;
      PL_MA_CHECK(ma_engine_stop(&m_pData->m_Engine));
    }
    m_bEnginePaused = true;
  }
  else
  {
    if (m_bEnginePaused && !m_bHeadless)
    {
      EnsureComOnThread();
      if (ma_engine_start(&m_pData->m_Engine) == MA_SUCCESS)
      {
        m_bDeviceStopExpected = false;
      }
      else
      {
        // the device went away while paused
        m_bDeviceLost = true;
      }
    }
    m_bEnginePaused = false;

    if (cvar_MiniAudioMute)
    {
      PL_MA_CHECK(ma_engine_set_volume(&m_pData->m_Engine, 0.0f));
    }
    else
    {
      PL_MA_CHECK(ma_engine_set_volume(&m_pData->m_Engine, cvar_MiniAudioMasterVolume));
    }

    // without a device nothing pulls the graph; mixing here keeps the engine clock and voice ends moving
    if (m_bHeadless)
    {
      const plTime tStart = plTime::Now();
      plMiniAudioDsp::RenderWithoutDevice(&m_pData->m_Engine, delta, m_pData->m_fHeadlessFrameRemainder, m_pData->m_HeadlessScratch);
      RecordMixTime(plTime::Now() - tStart, delta.GetSeconds());
    }
  }
}

void plMiniAudioSingleton::SetMasterChannelVolume(float fVolume)
{
  cvar_MiniAudioMasterVolume = plMath::Clamp<float>(fVolume, 0.0f, 1.0f);
}

float plMiniAudioSingleton::GetMasterChannelVolume() const
{
  return cvar_MiniAudioMasterVolume;
}

void plMiniAudioSingleton::SetMasterChannelMute(bool bMute)
{
  cvar_MiniAudioMute = bMute;
}

bool plMiniAudioSingleton::GetMasterChannelMute() const
{
  return cvar_MiniAudioMute;
}

void plMiniAudioSingleton::SetMasterChannelPaused(bool bPaused)
{
  cvar_MiniAudioPause = bPaused;
}

bool plMiniAudioSingleton::GetMasterChannelPaused() const
{
  return cvar_MiniAudioPause;
}

void plMiniAudioSingleton::SetSoundGroupVolume(plStringView sGroupName, float fVolume)
{
  if (!m_bInitialized)
    return;

  auto& group = GetSoundGroup(sGroupName);
  group.m_fVolume = fVolume;

  ma_sound_group_set_volume(group.m_pGroup.Borrow(), fVolume);
}

float plMiniAudioSingleton::GetSoundGroupVolume(plStringView sGroupName) const
{
  for (plUInt32 i = 0; i < m_pData->m_SoundGroups.GetCount(); ++i)
  {
    if (m_pData->m_SoundGroups[i].m_sName == sGroupName)
      return m_pData->m_SoundGroups[i].m_fVolume;
  }

  return 1.0f;
}

plMiniAudioSingleton::SoundGroup& plMiniAudioSingleton::GetSoundGroup(plStringView sGroupName)
{
  PL_ASSERT_DEV(m_bInitialized, "Legacy sound groups need a valid engine.");

  for (plUInt32 i = 0; i < m_pData->m_SoundGroups.GetCount(); ++i)
  {
    if (m_pData->m_SoundGroups[i].m_sName == sGroupName)
      return m_pData->m_SoundGroups[i];
  }

  auto& group = m_pData->m_SoundGroups.ExpandAndGetRef();
  group.m_sName = sGroupName;
  group.m_pGroup = PL_DEFAULT_NEW(ma_sound_group);

  ma_sound_group_init(GetEngine(), 0, nullptr, group.m_pGroup.Borrow());

  return group;
}

void plMiniAudioSingleton::SetListenerOverrideMode(bool bEnabled)
{
  m_bListenerOverrideMode = bEnabled;
}

void plMiniAudioSingleton::SetListener(plInt32 iIndex, const plVec3& vPosition, const plVec3& vForward, const plVec3& vUp, const plVec3& vVelocity)
{
  if (m_bListenerOverrideMode)
  {
    if (iIndex != -1)
      return;

    iIndex = 0;
  }

  if (iIndex < 0 || iIndex >= m_uiNumListeners)
    return;

  const plUInt8 uiSlot = static_cast<plUInt8>(iIndex);
  ListenerSlot& slot = m_Listeners[uiSlot];
  slot.m_vPosition = vPosition;
  slot.m_vForward = vForward;
  slot.m_vUp = vUp;
  slot.m_vVelocity = vVelocity;

  const bool bWasActive = slot.m_bActive;
  slot.m_bActive = true;

  if (!m_bInitialized)
    return;

  ApplyListenerSlot(uiSlot);

  if (!bWasActive)
  {
    ma_engine_listener_set_enabled(&m_pData->m_Engine, uiSlot, MA_TRUE);
  }
}

void plMiniAudioSingleton::ReleaseListenerSlot(plUInt8 uiSlot)
{
  if (uiSlot == 0 || uiSlot >= MaxListeners)
    return;

  m_Listeners[uiSlot].m_bActive = false;

  if (m_bInitialized)
  {
    ma_engine_listener_set_enabled(&m_pData->m_Engine, uiSlot, MA_FALSE);
  }
}

void plMiniAudioSingleton::ApplyListenerSlot(plUInt8 uiSlot)
{
  const ListenerSlot& slot = m_Listeners[uiSlot];
  const plVec3 vMaUp = -slot.m_vUp; // PL and MiniAudio use different coordinate systems

  ma_engine_listener_set_position(&m_pData->m_Engine, uiSlot, slot.m_vPosition.x, slot.m_vPosition.y, slot.m_vPosition.z);
  ma_engine_listener_set_direction(&m_pData->m_Engine, uiSlot, slot.m_vForward.x, slot.m_vForward.y, slot.m_vForward.z);
  ma_engine_listener_set_world_up(&m_pData->m_Engine, uiSlot, vMaUp.x, vMaUp.y, vMaUp.z);
  ma_engine_listener_set_velocity(&m_pData->m_Engine, uiSlot, slot.m_vVelocity.x, slot.m_vVelocity.y, slot.m_vVelocity.z);
}

void plMiniAudioSingleton::ApplyListenerEnabledStates()
{
  // voices spatialize against the closest enabled slot; slot 0 is always enabled so there is one
  for (plUInt8 i = 0; i < MaxListeners; ++i)
  {
    const bool bEnabled = i == 0 || (i < m_uiNumListeners && m_Listeners[i].m_bActive);
    ma_engine_listener_set_enabled(&m_pData->m_Engine, i, bEnabled ? MA_TRUE : MA_FALSE);
  }
}
