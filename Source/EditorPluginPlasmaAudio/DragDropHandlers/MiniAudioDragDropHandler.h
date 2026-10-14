#pragma once

#include <EditorFramework/DragDrop/ComponentDragDropHandler.h>

class plMiniAudioSoundComponentDragDropHandler : public plComponentDragDropHandler
{
  PL_ADD_DYNAMIC_REFLECTION(plMiniAudioSoundComponentDragDropHandler, plComponentDragDropHandler);

public:
  float CanHandle(const plDragDropInfo* pInfo) const override;

  virtual void OnDragBegin(const plDragDropInfo* pInfo) override;
};
