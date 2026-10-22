#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <AudioSystem/Core/AudioMiddleware.h>
#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicAssetWindow.h>
#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicEdit.h>
#include <EditorPluginPlasmaAudio/Preferences/MiniAudioPreferences.h>
#include <EditorUi/DockManager.h>
#include <Foundation/IO/MemoryStream.h>
#include <GuiFoundation/UIServices/DynamicStringEnum.h>

namespace
{
  constexpr int kPollIntervalMs = 40;
}

plAudioMusicAssetDocumentWindow::plAudioMusicAssetDocumentWindow(plDocument* pDocument)
  : plDocumentWindow(pDocument)
{
  m_pAssetDoc = static_cast<plAudioMusicAssetDocument*>(pDocument);

  SetMenuBarActions("AudioMusicAssetMenuBar");
  SetToolBarActions("AudioMusicAssetToolBar");

  // Audio Music: clips, lanes, transitions and the inspector; Music Properties on the right (all of what the view
  // selects, for what its inspector leaves out)
  m_pView = AddPanel<plEditorUiAudioMusicView>({"AudioMusicViewDockWidget", "Audio Music"}, m_pAssetDoc);
  AddPropertiesPanel({"AudioMusicAssetDockWidget", "Music Properties", plEditorUiDockRole::Right});

  m_pView->m_OnPlay = [this]()
  { Play(m_uiInitialState); };
  m_pView->m_OnStop = [this]()
  { Stop(); };
  m_pView->m_OnStateChosen = [this](plUInt32 uiState)
  { ChooseState(uiState); };
  m_pView->m_OnStinger = [this](plUInt32 uiStinger)
  { PlayStinger(uiStinger); };
  m_pView->m_OnSelect = [this](plEditorUiAudioMusicView::Selection selection, plUInt32 uiIndex)
  { Select(selection, uiIndex); };

  m_PropertySubscription = GetDocument()->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plAudioMusicAssetDocumentWindow::OnPropertyEvent, this));
  m_StructureSubscription = GetDocument()->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plAudioMusicAssetDocumentWindow::OnStructureEvent, this));
  m_AuditionSubscription = m_pAssetDoc->m_AuditionEvents.AddEventHandler(plMakeDelegate(&plAudioMusicAssetDocumentWindow::OnAuditionStatus, this));

  m_Timers.Start(plTime::MakeFromMilliseconds(kPollIntervalMs), [this]()
    {
      if (m_bAuditioning)
        SendCommand(plAudioAuditionCommand::Poll); },
    true);

  RefreshMusic();

  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);
}

plAudioMusicAssetDocumentWindow::~plAudioMusicAssetDocumentWindow()
{
  m_Timers.StopAll();

  if (m_bAuditioning)
    SendCommand(plAudioAuditionCommand::Stop);

  m_pAssetDoc->m_AuditionEvents.RemoveEventHandler(m_AuditionSubscription);
  GetDocument()->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(m_PropertySubscription);
  GetDocument()->GetObjectManager()->m_StructureEvents.RemoveEventHandler(m_StructureSubscription);

  // the view calls back into this window
  DestroyPanels();
}

void plAudioMusicAssetDocumentWindow::ArrangePanels(plEditorUiDockManager& ref_panels)
{
  ref_panels.DockPanel("AudioMusicAssetDockWidget", "AudioMusicViewDockWidget", plEditorUiDockSide::Right, 0.2f);
}

void plAudioMusicAssetDocumentWindow::OnPropertyEvent(const plDocumentObjectPropertyEvent& e)
{
  PL_IGNORE_UNUSED(e);
  RequestRefresh();
}

void plAudioMusicAssetDocumentWindow::OnStructureEvent(const plDocumentObjectStructureEvent& e)
{
  PL_IGNORE_UNUSED(e);
  RequestRefresh();
}

void plAudioMusicAssetDocumentWindow::RequestRefresh()
{
  // an edit changes many properties at once: the music is built again once, after them
  if (m_bRefreshPending)
    return;

  m_bRefreshPending = true;
  m_Timers.Start(plTime::MakeZero(), [this]()
    { RefreshMusic(); });
}

void plAudioMusicAssetDocumentWindow::RefreshMusic()
{
  m_bRefreshPending = false;

  plAudioEventDesc desc;
  const plStatus status = m_pAssetDoc->BuildEventDesc(desc);

  m_Music = status.Succeeded() ? desc.m_Music : plAudioMusicDesc();
  m_uiInitialState = 0;
  if (status.Succeeded() && !desc.m_Parameters.IsEmpty())
    m_uiInitialState = static_cast<plUInt32>(plMath::Max(desc.m_Parameters[0].m_fDefault, 0.0f));

  m_pView->SetBuildError(status.Succeeded() ? plStringView() : status.m_sMessage.GetView());
  m_pView->Refresh();

  // the names the states and transitions pick from
  const plAudioMusicAssetProperties* pProp = m_pAssetDoc->GetProperties();

  auto& segments = plDynamicStringEnum::CreateDynamicEnum("AudioMusic.Segments");
  segments.Clear();
  for (const plAudioMusicSegment& segment : pProp->m_Segments)
  {
    if (!segment.m_sName.IsEmpty())
      segments.AddValidValue(segment.m_sName);
  }

  auto& states = plDynamicStringEnum::CreateDynamicEnum("AudioMusic.States");
  states.Clear();
  for (const plAudioMusicState& state : pProp->m_States)
  {
    if (!state.m_sName.IsEmpty())
      states.AddValidValue(state.m_sName);
  }
}

void plAudioMusicAssetDocumentWindow::SendCommand(plAudioAuditionCommand::Enum command)
{
  plAudioAuditionMsgToEngine msg;
  msg.m_Command = command;

  if (!m_pAssetDoc->SendMessageToEngine(&msg))
  {
    m_bAuditioning = false;
    m_pView->SetAuditioning(false);
  }
}

void plAudioMusicAssetDocumentWindow::SendParameter(plStringView sName, float fValue)
{
  plAudioAuditionMsgToEngine msg;
  msg.m_Command = plAudioAuditionCommand::SetParameters;
  msg.m_ParameterNames.PushBack(sName);
  msg.m_ParameterValues.PushBack(fValue);
  m_pAssetDoc->SendMessageToEngine(&msg);
}

void plAudioMusicAssetDocumentWindow::Play(plUInt32 uiState)
{
  plAudioEventDesc desc;
  if (plStatus res = m_pAssetDoc->BuildEventDesc(desc); res.Failed())
  {
    ShowTemporaryStatusBarMsg(res.m_sMessage.GetView());
    return;
  }

  // the music starts in the state asked for
  if (!desc.m_Parameters.IsEmpty() && uiState < desc.m_Music.m_States.GetCount())
    desc.m_Parameters[0].m_fDefault = static_cast<float>(uiState);

  plContiguousMemoryStreamStorage storage;
  {
    plMemoryStreamWriter writer(&storage);
    if (plStatus res = m_pAssetDoc->WriteAuditionData(writer, desc); res.Failed())
    {
      ShowTemporaryStatusBarMsg(res.m_sMessage.GetView());
      return;
    }
  }

  plAudioAuditionMsgToEngine msg;
  msg.m_Command = plAudioAuditionCommand::Play;
  msg.m_EventData.PushBackRange(plArrayPtr<const plUInt8>(storage.GetData(), storage.GetStorageSize32()));

  if (plPlasmaAudioProjectPreferences* pPreferences = plPreferences::QueryPreferences<plPlasmaAudioProjectPreferences>())
    msg.m_sMixer = pPreferences->GetAuditionMixer();

  m_bAuditioning = m_pAssetDoc->SendMessageToEngine(&msg);
  m_bMixerNoteShown = false;

  if (!m_bAuditioning)
  {
    ShowTemporaryStatusBarMsg("The engine process is not running - the audition plays there.");
    return;
  }

  m_pView->ClearMessages();
  m_pView->SetAuditioning(true);

  plStringBuilder sStart;
  sStart.SetFormat("start in \"{}\"", desc.m_Music.m_States[plMath::Min(uiState, desc.m_Music.m_States.GetCount() - 1)].m_sName);
  m_pView->AddMessage(sStart, plEditorUiAudioMusicView::MessageKind::Note);
}

void plAudioMusicAssetDocumentWindow::Stop()
{
  if (!m_bAuditioning)
    return;

  // the status shows the stop fade; the audition ends once it reports nothing playing
  SendCommand(plAudioAuditionCommand::Stop);
}

void plAudioMusicAssetDocumentWindow::ChooseState(plUInt32 uiState)
{
  if (uiState >= m_Music.m_States.GetCount())
    return;

  // without music playing, the state starts it
  if (!m_bAuditioning)
  {
    Play(uiState);
    return;
  }

  SendParameter(plAudioMusicDesc::StateParameter, static_cast<float>(uiState));

  plStringBuilder sText;
  sText.SetFormat("state \"{}\" asked for", m_Music.m_States[uiState].m_sName);
  m_pView->AddMessage(sText, plEditorUiAudioMusicView::MessageKind::Note);
}

void plAudioMusicAssetDocumentWindow::PlayStinger(plUInt32 uiStinger)
{
  const plAudioMusicAssetProperties* pProp = m_pAssetDoc->GetProperties();
  if (!m_bAuditioning || uiStinger >= pProp->m_Stingers.GetCount())
    return;

  // a stinger without a clip is not in the built music
  const plString& sStinger = pProp->m_Stingers[uiStinger].m_sName;
  if (m_Music.FindStinger(sStinger) == plInvalidIndex)
  {
    ShowTemporaryStatusBarMsg("The stinger has no clip.");
    return;
  }

  // the name the game would send
  plStringBuilder sName(plAudioSystemReservedParameter::StingerPrefix, sStinger);
  SendParameter(sName, 1.0f);
  m_pView->NoteStingerRequest(uiStinger);
}

void plAudioMusicAssetDocumentWindow::Select(plEditorUiAudioMusicView::Selection selection, plUInt32 uiIndex)
{
  const plDocumentObject* pSelect = nullptr;

  switch (selection)
  {
    case plEditorUiAudioMusicView::Selection::Segment:
      pSelect = plAudioMusicEdit::GetElement(m_pAssetDoc, "Segments", uiIndex);
      break;
    case plEditorUiAudioMusicView::Selection::State:
      pSelect = plAudioMusicEdit::GetElement(m_pAssetDoc, "States", uiIndex);
      break;
    case plEditorUiAudioMusicView::Selection::Transition:
      pSelect = plAudioMusicEdit::GetElement(m_pAssetDoc, "Transitions", uiIndex);
      break;
    case plEditorUiAudioMusicView::Selection::Stinger:
      pSelect = plAudioMusicEdit::GetElement(m_pAssetDoc, "Stingers", uiIndex);
      break;
    default:
      break;
  }

  // the music's own properties for everything else
  GetDocument()->GetSelectionManager()->SetSelection(pSelect != nullptr ? pSelect : plAudioMusicEdit::GetProperties(m_pAssetDoc));
}

void plAudioMusicAssetDocumentWindow::OnAuditionStatus(const plAudioAuditionStatusMsgToEditor& status)
{
  if (!status.m_sError.IsEmpty())
    ShowTemporaryStatusBarMsg(status.m_sError.GetView());

  if (status.m_bPlaying && !status.m_bWaiting && !status.m_bHasMixer && !m_bMixerNoteShown)
  {
    m_bMixerNoteShown = true;
    ShowTemporaryStatusBarMsg("No mixer is loaded: the music plays straight to the output. Set 'Audition Mixer' in the PlasmaAudio project preferences.");
  }

  plEditorUiAudioMusicView::Status viewStatus;
  viewStatus.m_bPlaying = status.m_bAvailable && status.m_bPlaying && status.m_bMusic;
  viewStatus.m_uiState = status.m_uiMusicState;
  viewStatus.m_uiSegment = status.m_uiMusicSegment;
  viewStatus.m_bTransitionSegment = status.m_bMusicTransitionSegment;
  viewStatus.m_uiBar = status.m_uiMusicBar;
  viewStatus.m_uiBeat = status.m_uiMusicBeat;
  viewStatus.m_fTempo = status.m_fMusicTempo;
  viewStatus.m_uiBeatsPerBar = status.m_uiMusicBeatsPerBar;
  viewStatus.m_uiPendingState = status.m_uiMusicPendingState;
  viewStatus.m_fPendingIn = status.m_fMusicPendingIn;
  m_pView->SetStatus(viewStatus);

  // what the game object would get
  for (const plString& sMarker : status.m_Markers)
  {
    if (sMarker.StartsWith(plAudioSystemReservedMarker::BeatPrefix))
    {
      plStringView sBeat = sMarker;
      sBeat.Shrink(plStringUtils::GetCharacterCount(plAudioSystemReservedMarker::BeatPrefix), 0);

      plHybridArray<plStringView, 3> parts;
      sBeat.Split(true, parts, ":");

      plStringBuilder sText;
      if (parts.GetCount() >= 3)
        sText.SetFormat("beat {}.{} {}", parts[0], parts[1], parts[2]);
      else
        sText = sBeat;

      m_pView->AddMessage(sText, plEditorUiAudioMusicView::MessageKind::Beat);
    }
    else if (sMarker.StartsWith("Stinger: "))
    {
      plStringView sName = sMarker;
      sName.Shrink(9, 0);
      m_pView->AddMessage(sName, plEditorUiAudioMusicView::MessageKind::Stinger);
    }
    else
    {
      m_pView->AddMessage(sMarker, plEditorUiAudioMusicView::MessageKind::Transition);
    }
  }

  // the end, after the messages of the last status
  if (m_bAuditioning && (!status.m_bAvailable || (!status.m_bPlaying && !status.m_bWaiting)))
  {
    m_bAuditioning = false;
    m_pView->SetAuditioning(false);
    m_pView->AddMessage("music stopped", plEditorUiAudioMusicView::MessageKind::Note);
  }
}
