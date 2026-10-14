#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <AudioSystem/Core/AudioSystem.h>
#include <Core/GameApplication/GameApplicationBase.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Configuration/Startup.h>
#include <PlasmaAudioPlugin/Banks/AudioBankResource.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResource.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResourceLoader.h>
#include <PlasmaAudioPlugin/MiniAudioMiddleware.h>
#include <PlasmaAudioPlugin/MiniAudioSingleton.h>

static plPlasmaAudioMiddleware* s_pMiniAudioMiddleware = nullptr;
static plAudioClipResourceLoader s_AudioClipResourceLoader;

/// The game application the profiling stats are published from; null in processes without one (the editor).
static plGameApplicationBase* s_pProfilingApplication = nullptr;

static void PlasmaAudioGameApplicationEventHandler(const plGameApplicationExecutionEvent& e)
{
  if (e.m_Type != plGameApplicationExecutionEvent::Type::AfterWorldUpdates)
    return;

  if (plPlasmaAudioMiddleware* pMiddleware = plPlasmaAudioMiddleware::GetSingleton())
  {
    pMiddleware->PublishStats();
  }
}

// clang-format off
PL_BEGIN_SUBSYSTEM_DECLARATION(PlasmaAudio, PlasmaAudioPlugin)

BEGIN_SUBSYSTEM_DEPENDENCIES
  "Foundation",
  "Core",
  "AudioSystem"
END_SUBSYSTEM_DEPENDENCIES

ON_CORESYSTEMS_STARTUP
{
  plResourceManager::SetResourceTypeLoader<plAudioClipResource>(&s_AudioClipResourceLoader);
  plResourceManager::RegisterResourceForAssetType("Audio Clip", plGetStaticRTTI<plAudioClipResource>());
  plResourceManager::RegisterResourceForAssetType("Audio Bank", plGetStaticRTTI<plAudioBankResource>());
}

ON_CORESYSTEMS_SHUTDOWN
{
  plResourceManager::SetResourceTypeLoader<plAudioClipResource>(nullptr);
}

ON_HIGHLEVELSYSTEMS_STARTUP
{
  s_pMiniAudioMiddleware = PL_DEFAULT_NEW(plPlasmaAudioMiddleware);
  plAudioSystem::GetSingleton()->Startup();

  s_pProfilingApplication = plGameApplicationBase::GetGameApplicationBaseInstance();
  if (s_pProfilingApplication != nullptr)
  {
    s_pProfilingApplication->m_ExecutionEvents.AddEventHandler(&PlasmaAudioGameApplicationEventHandler);
  }
}

ON_HIGHLEVELSYSTEMS_SHUTDOWN
{
  if (s_pProfilingApplication != nullptr)
  {
    s_pProfilingApplication->m_ExecutionEvents.RemoveEventHandler(&PlasmaAudioGameApplicationEventHandler);
    s_pProfilingApplication = nullptr;
  }

  s_pMiniAudioMiddleware->RemoveProfilingStats();

  plAudioSystem::GetSingleton()->Shutdown();
  PL_DEFAULT_DELETE(s_pMiniAudioMiddleware);
  s_pMiniAudioMiddleware = nullptr;
}

PL_END_SUBSYSTEM_DECLARATION;
// clang-format on
