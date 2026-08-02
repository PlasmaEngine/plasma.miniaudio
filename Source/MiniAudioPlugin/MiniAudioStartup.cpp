#include <MiniAudioPlugin/MiniAudioPluginPCH.h>

#include <AudioSystem/Core/AudioSystem.h>
#include <Foundation/Configuration/Startup.h>
#include <MiniAudioPlugin/MiniAudioMiddleware.h>
#include <MiniAudioPlugin/MiniAudioSingleton.h>

static plMiniAudioMiddleware* s_pMiniAudioMiddleware = nullptr;

// clang-format off
PL_BEGIN_SUBSYSTEM_DECLARATION(MiniAudio, MiniAudioPlugin)

BEGIN_SUBSYSTEM_DEPENDENCIES
  "Foundation",
  "Core",
  "AudioSystem"
END_SUBSYSTEM_DEPENDENCIES

ON_CORESYSTEMS_STARTUP
{
}

ON_CORESYSTEMS_SHUTDOWN
{
}

ON_HIGHLEVELSYSTEMS_STARTUP
{
  s_pMiniAudioMiddleware = PL_DEFAULT_NEW(plMiniAudioMiddleware);
  plAudioSystem::GetSingleton()->Startup();
}

ON_HIGHLEVELSYSTEMS_SHUTDOWN
{
  plAudioSystem::GetSingleton()->Shutdown();
  PL_DEFAULT_DELETE(s_pMiniAudioMiddleware);
  s_pMiniAudioMiddleware = nullptr;
}

PL_END_SUBSYSTEM_DECLARATION;
// clang-format on
