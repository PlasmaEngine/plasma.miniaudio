#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/EditorApp/EditorApp.h>
#include <EditorFramework/EngineShell/DocumentFrame.h>
#include <EditorFramework/EngineShell/EngineShell.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineViews.h>
#include <EditorPluginPlasmaAudio/EngineUi/AudioShellPanels.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerAssetWindow.h>
#include <EditorPluginPlasmaAudio/MixerAsset/AudioMixerViews.h>
#include <EditorPluginPlasmaAudio/Preview/AudioEnumRegistry.h>
#include <EditorUi/DockManager.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/MemoryStream.h>
#include <GuiFoundation/EngineUi/EngineUiControlStrip.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

namespace
{
  /// How often the console asks the engine process for its meters.
  constexpr plTime s_PollInterval = plTime::MakeFromMilliseconds(50);

  /// How often a dragged value is sent to the engine process at most.
  constexpr plTime s_LiveEditInterval = plTime::MakeFromMilliseconds(40);

  /// How long a status message stays in the status row.
  constexpr plTime s_ToastDuration = plTime::MakeFromSeconds(6);

  template <typename ARRAY>
  void GatherChildren(const plDocumentObject* pParent, const char* szProperty, ARRAY& out_children)
  {
    out_children.Clear();

    if (pParent == nullptr)
      return;

    for (const plDocumentObject* pChild : pParent->GetChildren())
    {
      if (pChild->GetParentProperty().IsEqual(szProperty))
        out_children.PushBack(pChild);
    }
  }

  plString GetName(const plDocumentObject* pObject)
  {
    return pObject->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
  }

  /// Changes every string property (or string array entry) of the objects that equals sOld. Returns how many changed.
  plUInt32 RenameStrings(plObjectCommandAccessor& ref_accessor, plArrayPtr<const plDocumentObject* const> objects, const char* szProperty, plStringView sOld, plStringView sNew, bool bArray = false)
  {
    plUInt32 uiChanged = 0;

    for (const plDocumentObject* pObject : objects)
    {
      const auto& accessor = pObject->GetTypeAccessor();

      if (bArray)
      {
        for (plInt32 i = 0; i < accessor.GetCount(szProperty); ++i)
        {
          if (accessor.GetValue(szProperty, i).ConvertTo<plString>().IsEqual_NoCase(sOld))
          {
            ref_accessor.SetValueByName(pObject, szProperty, sNew, i).IgnoreResult();
            ++uiChanged;
          }
        }
      }
      else if (accessor.GetValue(szProperty).ConvertTo<plString>().IsEqual_NoCase(sOld))
      {
        ref_accessor.SetValueByName(pObject, szProperty, sNew).IgnoreResult();
        ++uiChanged;
      }
    }

    return uiChanged;
  }

  /// Points every reference to the bus sOld in the mixer at sNew: parents, sends, sidechain keys, VCAs and snapshot
  /// overrides. Returns how many references changed.
  plUInt32 RenameBusReferences(plObjectCommandAccessor& ref_accessor, const plDocumentObject* pProps, plStringView sOld, plStringView sNew)
  {
    plHybridArray<const plDocumentObject*, 12> buses, vcas, snapshots;
    GatherChildren(pProps, "Buses", buses);
    GatherChildren(pProps, "VCAs", vcas);
    GatherChildren(pProps, "Snapshots", snapshots);

    plHybridArray<const plDocumentObject*, 16> sends, effects, overrides;
    for (const plDocumentObject* pBus : buses)
    {
      plHybridArray<const plDocumentObject*, 4> children;
      GatherChildren(pBus, "Sends", children);
      sends.PushBackRange(children);
      GatherChildren(pBus, "Effects", children);
      effects.PushBackRange(children);
    }

    for (const plDocumentObject* pSnapshot : snapshots)
    {
      plHybridArray<const plDocumentObject*, 8> children;
      GatherChildren(pSnapshot, "Overrides", children);
      overrides.PushBackRange(children);
    }

    plUInt32 uiChanged = 0;
    uiChanged += RenameStrings(ref_accessor, buses, "Parent", sOld, sNew);
    uiChanged += RenameStrings(ref_accessor, sends, "TargetBus", sOld, sNew);
    uiChanged += RenameStrings(ref_accessor, effects, "SidechainBus", sOld, sNew);
    uiChanged += RenameStrings(ref_accessor, vcas, "Buses", sOld, sNew, true);
    uiChanged += RenameStrings(ref_accessor, overrides, "Bus", sOld, sNew);
    uiChanged += RenameStrings(ref_accessor, overrides, "Send", sOld, sNew);
    return uiChanged;
  }
} // namespace

plAudioMixerAssetDocumentWindow::plAudioMixerAssetDocumentWindow(plDocument* pDocument)
  : plDocumentWindow(pDocument)
{
  m_pAssetDoc = static_cast<plAudioMixerAssetDocument*>(pDocument);

  SetMenuBarActions("AudioMixerAssetMenuBar");
  SetToolBarActions("AudioMixerAssetToolBar");

  CreateViews();
  CreatePropertyDock();

  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);

  GetDocument()->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::StructureEventHandler, this));
  GetDocument()->GetSelectionManager()->m_Events.AddEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::SelectionEventHandler, this));
  GetDocument()->m_EventsOne.AddEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::DocumentEventHandler, this));
  m_ConsoleSubscription = m_pAssetDoc->m_ConsoleEvents.AddEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::OnConsoleStatus, this));

  // the engine process answers every request with its meters; a dragged value waits here for its turn
  m_Timers.Start(s_PollInterval, [this]()
    {
      if (m_bApplyPending && plTime::Now() - m_LastApply >= s_LiveEditInterval)
        ApplyToEngine(true);
      else
        SendConsoleCommand(plAudioMixerConsoleCommand::Poll);

      if (!m_sToast.IsEmpty() && plTime::Now() - m_ToastTime > s_ToastDuration)
      {
        m_sToast.Clear();
        UpdateStatusRow();
      } },
    true);

  TakeBaseline();
  QueueRefresh();
}

plAudioMixerAssetDocumentWindow::~plAudioMixerAssetDocumentWindow()
{
  // the tool strip and the views call back into this window
  DestroyPanels();

  m_Timers.StopAll();

  m_pAssetDoc->m_ConsoleEvents.RemoveEventHandler(m_ConsoleSubscription);
  GetDocument()->m_EventsOne.RemoveEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::DocumentEventHandler, this));
  GetDocument()->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::StructureEventHandler, this));
  GetDocument()->GetSelectionManager()->m_Events.RemoveEventHandler(plMakeDelegate(&plAudioMixerAssetDocumentWindow::SelectionEventHandler, this));
}

void plAudioMixerAssetDocumentWindow::CreateViews()
{
  using plEditorUi::Icon;

  // Mixer Console: the add buttons, the source and the validation, the strips, the snapshot rail, the status row
  {
    auto* pConsole = AddPanel<plEditorUiAudioShellStack>({"AudioMixerConsoleDockWidget", "Mixer Console"});

    plUniquePtr<plEditorUiControlStrip> pToolStrip = PL_DEFAULT_NEW(plEditorUiControlStrip);
    m_pToolStrip = pToolStrip.Borrow();
    m_uiAddBus = m_pToolStrip->AddIconTextButton(Icon::Add, "Bus", "Adds a mixer bus (the first bus is the Master bus)");
    m_uiAddAuxBus = m_pToolStrip->AddIconTextButton(Icon::Add, "Aux Bus", "Adds an aux/return bus (reverb; set its Environment name to feed it from environment components)");
    m_uiAddVca = m_pToolStrip->AddIconTextButton(Icon::Add, "VCA", "Adds a VCA (options-menu volume group over a set of buses)");
    m_uiAddSnapshot = m_pToolStrip->AddIconTextButton(Icon::Add, "Snapshot", "Adds a blendable mixer snapshot");
    m_uiRemoveSelected = m_pToolStrip->AddIconTextButton(Icon::Remove, "Remove Selected", "Removes the selected bus / VCA / snapshot");
    m_pToolStrip->AddSpacer();
    m_pToolStrip->AddLabel("Source");
    m_uiSourcePreview = m_pToolStrip->AddIconTextButton(Icon::Play, "Preview", "Loads this mixer into the editor's engine process, so event auditions and the test signal play through it", true);
    m_uiSourceEngine = m_pToolStrip->AddIconTextButton(Icon::Render, "Engine process", "Shows the mixer the engine process plays (a running scene's); edits apply live when that is this mixer", true);
    m_uiTestSignal = m_pToolStrip->AddIconTextButton(Icon::Volume, "Test signal", "Plays noise in bursts into every bus without child buses, each in its own rhythm", true);
    m_pToolStrip->AddSeparator();
    m_uiValidation = m_pToolStrip->AddChip("Validation: 0 issues", plAudioEngineUi::AudioColor(), "Routing, sidechain and reference checks; a mixer with issues does not transform and is not applied");
    m_pToolStrip->SetChecked(m_uiSourcePreview, true);

    m_pToolStrip->m_OnChanged = [this](plUInt32 uiControl, plInt32 iValue)
    {
      if (uiControl == m_uiAddBus)
        OnAddBus(false);
      else if (uiControl == m_uiAddAuxBus)
        OnAddBus(true);
      else if (uiControl == m_uiAddVca)
        OnAddVca();
      else if (uiControl == m_uiAddSnapshot)
        OnAddSnapshot();
      else if (uiControl == m_uiRemoveSelected)
        OnRemoveSelected();
      else if (uiControl == m_uiSourcePreview)
        SetSource(Source::Preview);
      else if (uiControl == m_uiSourceEngine)
        SetSource(Source::EngineProcess);
      else if (uiControl == m_uiTestSignal)
      {
        m_bTestSignal = iValue != 0;
        SendConsoleCommand(plAudioMixerConsoleCommand::SetTestSignal, {}, m_bTestSignal ? 1.0f : 0.0f);
        ShowToast(m_bTestSignal ? "Test signal: noise bursts play into the buses without child buses" : "Test signal off");
      }
    };

    // the row keeps the strip's own height
    pConsole->AddOwnedPart(std::move(pToolStrip), m_pToolStrip->GetHeight());

    m_pStripsView = static_cast<plEditorUiAudioMixerStrips*>(pConsole->AddOwnedPart(PL_DEFAULT_NEW(plEditorUiAudioMixerStrips, GetDocument(), &m_State), 0.0f));

    m_pStripsView->m_OnLiveEdit = plMakeDelegate(&plAudioMixerAssetDocumentWindow::OnLiveEdit, this);
    m_pStripsView->m_OnMute = plMakeDelegate(&plAudioMixerAssetDocumentWindow::OnMute, this);
    m_pStripsView->m_OnSolo = plMakeDelegate(&plAudioMixerAssetDocumentWindow::OnSolo, this);
    m_pStripsView->m_OnSelectInsert = plMakeDelegate(&plAudioMixerAssetDocumentWindow::OnSelectInsert, this);

    m_pRailView = static_cast<plEditorUiAudioMixerSnapshotRail*>(pConsole->AddOwnedPart(PL_DEFAULT_NEW(plEditorUiAudioMixerSnapshotRail, GetDocument(), &m_State), plEditorUiAudioMixerSnapshotRail::GetHeight()));

    m_pRailView->m_OnIntensity = plMakeDelegate(&plAudioMixerAssetDocumentWindow::OnSnapshotIntensity, this);
    m_pRailView->m_OnCapture = plMakeDelegate(&plAudioMixerAssetDocumentWindow::OnCapture, this);

    m_pStatusView = static_cast<plEditorUiAudioStatusRow*>(pConsole->AddOwnedPart(PL_DEFAULT_NEW(plEditorUiAudioStatusRow), plEditorUiAudioStatusRow::GetHeight()));
  }

  // Routing: the routing tree
  m_pRoutingView = AddPanel<plEditorUiAudioMixerRouting>({"AudioMixerRoutingDockWidget", "Routing"}, GetDocument());

  // Insert: the insert editor
  {
    m_pInsertView = AddPanel<plEditorUiAudioMixerInsertEditor>({"AudioMixerInsertDockWidget", "Insert"}, GetDocument(), &m_State);

    m_pInsertView->m_OnLiveEdit = plMakeDelegate(&plAudioMixerAssetDocumentWindow::OnLiveEdit, this);
    m_pInsertView->m_OnRename = plMakeDelegate(&plAudioMixerAssetDocumentWindow::RenameBus, this);
  }
}

void plAudioMixerAssetDocumentWindow::CreatePropertyDock()
{
  // Properties: the asset's status and the properties of the current selection (the selected bus, VCA or snapshot)
  AddPropertiesPanel({"AudioMixerAssetDockWidget", "Properties"});
}

void plAudioMixerAssetDocumentWindow::ArrangePanels(plEditorUiDockManager& ref_panels)
{
  ref_panels.DockPanel("AudioMixerRoutingDockWidget", "AudioMixerConsoleDockWidget", plEditorUiDockSide::Left, 0.18f);
  ref_panels.DockPanel("AudioMixerInsertDockWidget", "AudioMixerConsoleDockWidget", plEditorUiDockSide::Right, 0.24f);
  ref_panels.DockPanel("AudioMixerAssetDockWidget", "AudioMixerInsertDockWidget", plEditorUiDockSide::Tab);
  ref_panels.SetCurrentPanel("AudioMixerInsertDockWidget");
}

const plDocumentObject* plAudioMixerAssetDocumentWindow::GetPropertiesObject() const
{
  const auto& children = GetDocument()->GetObjectManager()->GetRootObject()->GetChildren();
  return children.IsEmpty() ? nullptr : children[0];
}

void plAudioMixerAssetDocumentWindow::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  // a bus renamed in the property grid: its references follow once the rename's step is done
  if (e.m_sProperty == "Name" && e.m_pObject->GetTypeAccessor().GetType() == plGetStaticRTTI<plAudioMixerBus>() && !m_bUpdatingReferences && !GetDocument()->GetCommandHistory()->IsInUndoRedo())
  {
    const plString sOld = e.m_OldValue.ConvertTo<plString>();
    const plString sNew = e.m_NewValue.ConvertTo<plString>();

    if (!sOld.IsEmpty() && !sNew.IsEmpty() && !sOld.IsEqual_NoCase(sNew))
    {
      m_sPendingRenameFrom = sOld;
      m_sPendingRenameTo = sNew;
    }
  }

  QueueRefresh();
}

void plAudioMixerAssetDocumentWindow::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  PL_IGNORE_UNUSED(e);
  QueueRefresh();
}

void plAudioMixerAssetDocumentWindow::SelectionEventHandler(const plSelectionManagerEvent& e)
{
  PL_IGNORE_UNUSED(e);

  // selecting a bus edits the bus in the insert panel; an insert stays selected while its bus is
  const plDocumentObject* pSelected = GetDocument()->GetSelectionManager()->GetCurrentObject();
  if (pSelected != nullptr && pSelected->GetTypeAccessor().GetType() == plGetStaticRTTI<plAudioMixerBus>())
  {
    if (m_State.m_SelectedBus != pSelected->GetGuid())
    {
      m_State.m_SelectedBus = pSelected->GetGuid();
      m_State.m_iSelectedEffect = -1;
    }
  }
  else
  {
    m_State.m_SelectedBus = plUuid();
    m_State.m_iSelectedEffect = -1;
  }

  QueueRefresh();
}

void plAudioMixerAssetDocumentWindow::DocumentEventHandler(const plDocumentEvent& e)
{
  // the saved mix is what Capture compares against
  if (e.m_Type == plDocumentEvent::Type::DocumentSaved)
    TakeBaseline();
}

void plAudioMixerAssetDocumentWindow::QueueRefresh()
{
  if (m_bRefreshQueued)
    return;

  m_bRefreshQueued = true;

  m_Timers.Start(plTime::MakeZero(), [this]()
    {
      m_bRefreshQueued = false;

      if (!m_sPendingRenameFrom.IsEmpty())
      {
        const plString sFrom = m_sPendingRenameFrom;
        const plString sTo = m_sPendingRenameTo;
        m_sPendingRenameFrom.Clear();
        m_sPendingRenameTo.Clear();
        UpdateReferencesAfterRename(sFrom, sTo);
      }

      RebuildDesc();
      UpdateConsoleState();

      m_pStripsView->Refresh();
      m_pRailView->Refresh();
      m_pRoutingView->Refresh();
      m_pInsertView->Refresh();

      // the engine process hears every edit right away
      ApplyToEngine(true);

      // offer these names as dropdown choices on the audio components
      if (m_bDescValid)
        plAudioEnumRegistry::RegisterMixerNames(m_Desc);

      const plUInt32 uiIssues = m_State.m_Issues.GetCount();
      plStringBuilder sValidation;
      if (uiIssues == 0)
        sValidation = "Validation: 0 issues";
      else
        sValidation.SetFormat("Validation: {} {}", uiIssues, uiIssues == 1 ? "error" : "errors");
      m_pToolStrip->SetText(m_uiValidation, sValidation);
      m_pToolStrip->SetColor(m_uiValidation, uiIssues == 0 ? plAudioEngineUi::AudioColor() : plEditorUi::Danger());

      UpdateStatusRow(); });
}

void plAudioMixerAssetDocumentWindow::RebuildDesc()
{
  m_State.m_Issues.Clear();
  m_bDescValid = plAudioMixerAssetProperties::BuildRuntimeDesc(*m_pAssetDoc->GetProperties(), m_Desc, &m_State.m_Issues).Succeeded();

  // a value dragged before this edit is part of the document now
  m_LiveEdit = plAudioMixerLiveEdit();
}

void plAudioMixerAssetDocumentWindow::ApplyToEngine(bool bForce)
{
  if (!m_bDescValid)
  {
    // the engine process keeps the last valid mixer; the banner says why
    SendConsoleCommand(plAudioMixerConsoleCommand::Poll);
    return;
  }

  if (!bForce && plTime::Now() - m_LastApply < s_LiveEditInterval)
  {
    m_bApplyPending = true;
    return;
  }

  plAudioMixerDesc desc = m_Desc;

  // the dragged value, heard before it is written to the document
  const plAudioMixerLiveEdit& edit = m_LiveEdit;
  if (edit.m_Type != plAudioMixerLiveEdit::Type::None && edit.m_uiBus < desc.m_Buses.GetCount())
  {
    plAudioBusDesc& bus = desc.m_Buses[edit.m_uiBus];

    switch (edit.m_Type)
    {
      case plAudioMixerLiveEdit::Type::BusVolume:
        bus.m_fVolumeDb = edit.m_fValue;
        break;

      case plAudioMixerLiveEdit::Type::SendLevel:
        if (edit.m_uiIndex < bus.m_Sends.GetCount())
          bus.m_Sends[edit.m_uiIndex].m_fLevelDb = edit.m_fValue;
        break;

      case plAudioMixerLiveEdit::Type::EffectParam:
        if (edit.m_uiIndex < bus.m_Effects.GetCount() && edit.m_uiParam < plAudioEffectMaxParams)
          bus.m_Effects[edit.m_uiIndex].m_fParams[edit.m_uiParam] = edit.m_fValue;
        break;

      default:
        break;
    }
  }

  plAudioMixerConsoleMsgToEngine msg;
  msg.m_Command = plAudioMixerConsoleCommand::Apply;
  msg.m_bLoad = m_Source == Source::Preview;

  plMemoryStreamContainerWrapperStorage<plDataBuffer> storage(&msg.m_MixerData);
  plMemoryStreamWriter writer(&storage);
  if (m_pAssetDoc->WriteConsoleData(writer, desc).Failed())
    return;

  m_pAssetDoc->SendMessageToEngine(&msg);
  m_LastApply = plTime::Now();
  m_bApplyPending = false;
}

bool plAudioMixerAssetDocumentWindow::SendConsoleCommand(plAudioMixerConsoleCommand::Enum command, plStringView sTarget, float fValue)
{
  plAudioMixerConsoleMsgToEngine msg;
  msg.m_Command = command;
  msg.m_sTarget = sTarget;
  msg.m_fValue = fValue;
  return m_pAssetDoc->SendMessageToEngine(&msg);
}

void plAudioMixerAssetDocumentWindow::OnConsoleStatus(const plAudioMixerConsoleStatusMsgToEditor& status)
{
  PL_IGNORE_UNUSED(status);

  if (!status.m_sError.IsEmpty())
    ShowToast(status.m_sError);

  UpdateConsoleState();

  m_pStripsView->Refresh();
  m_pRailView->Refresh();
  m_pInsertView->Refresh();
  UpdateStatusRow();
}

void plAudioMixerAssetDocumentWindow::UpdateConsoleState()
{
  const plAudioMixerConsoleStatusMsgToEditor& status = m_pAssetDoc->GetConsoleStatus();
  const plAudioMixerAssetProperties& props = *m_pAssetDoc->GetProperties();
  const plUInt32 uiBusCount = props.m_Buses.GetCount();

  m_State.m_bLive = status.m_bAvailable && status.m_bIsThisMixer;

  m_State.m_BusPeak.SetCount(uiBusCount);
  m_State.m_BusRms.SetCount(uiBusCount);
  m_State.m_BusMuted.SetCount(uiBusCount);
  m_State.m_BusSoloed.SetCount(uiBusCount);
  m_State.m_BusSilenced.SetCount(uiBusCount);
  m_State.m_BusError.SetCount(uiBusCount);
  m_State.m_BusSnapshotDb.SetCount(uiBusCount);
  m_State.m_EffectGainReduction.SetCount(uiBusCount);

  // the engine process's values belong to its buses by name; its effects follow the applied desc
  plUInt32 uiEffectOffset = 0;
  for (plUInt32 i = 0; i < uiBusCount; ++i)
  {
    const plAudioMixerBus& bus = props.m_Buses[i];

    m_State.m_BusPeak[i] = 0.0f;
    m_State.m_BusRms[i] = 0.0f;
    m_State.m_BusMuted[i] = false;
    m_State.m_BusSoloed[i] = false;
    m_State.m_BusError[i] = false;
    m_State.m_EffectGainReduction[i].SetCount(bus.m_Effects.GetCount());
    for (float& f : m_State.m_EffectGainReduction[i])
      f = 0.0f;

    if (!m_State.m_bLive)
      continue;

    for (plUInt32 j = 0; j < status.m_BusNames.GetCount(); ++j)
    {
      if (!status.m_BusNames[j].IsEqual_NoCase(bus.m_sName))
        continue;

      m_State.m_BusPeak[i] = j < status.m_BusPeaks.GetCount() ? status.m_BusPeaks[j] : 0.0f;
      m_State.m_BusRms[i] = j < status.m_BusRms.GetCount() ? status.m_BusRms[j] : 0.0f;
      m_State.m_BusMuted[i] = j < status.m_BusMuted.GetCount() && status.m_BusMuted[j] != 0;
      m_State.m_BusSoloed[i] = j < status.m_BusSoloed.GetCount() && status.m_BusSoloed[j] != 0;
      break;
    }

    if (m_bDescValid && i < m_Desc.m_Buses.GetCount())
    {
      const plUInt32 uiEffects = m_Desc.m_Buses[i].m_Effects.GetCount();
      for (plUInt32 e = 0; e < uiEffects && e < m_State.m_EffectGainReduction[i].GetCount(); ++e)
      {
        if (uiEffectOffset + e < status.m_EffectGainReductions.GetCount())
          m_State.m_EffectGainReduction[i][e] = status.m_EffectGainReductions[uiEffectOffset + e];
      }
      uiEffectOffset += uiEffects;
    }
  }

  // silenced: muted, or outside every soloed bus's routing
  bool bAnySolo = false;
  for (bool bSoloed : m_State.m_BusSoloed)
    bAnySolo |= bSoloed;

  for (plUInt32 i = 0; i < uiBusCount; ++i)
  {
    bool bSilenced = m_State.m_BusMuted[i];

    if (!bSilenced && bAnySolo && m_bDescValid)
    {
      bSilenced = true;
      for (plUInt32 s = 0; s < uiBusCount; ++s)
      {
        if (m_State.m_BusSoloed[s] && (m_Desc.IsRoutedTo(i, s) || m_Desc.IsRoutedTo(s, i)))
          bSilenced = false;
      }
    }

    m_State.m_BusSilenced[i] = bSilenced;
  }

  for (const plAudioMixerIssue& issue : m_State.m_Issues)
  {
    if (issue.m_uiBus < uiBusCount)
      m_State.m_BusError[issue.m_uiBus] = true;
    if (issue.m_uiOtherBus < uiBusCount)
      m_State.m_BusError[issue.m_uiOtherBus] = true;
  }

  // snapshot intensities by name, and the volumes with the playing snapshots blended in (by priority, like the runtime)
  m_State.m_SnapshotIntensity.SetCount(props.m_Snapshots.GetCount());
  for (plUInt32 s = 0; s < props.m_Snapshots.GetCount(); ++s)
  {
    m_State.m_SnapshotIntensity[s] = 0.0f;

    for (plUInt32 j = 0; j < status.m_SnapshotNames.GetCount() && m_State.m_bLive; ++j)
    {
      if (status.m_SnapshotNames[j].IsEqual_NoCase(props.m_Snapshots[s].m_sName) && j < status.m_SnapshotIntensities.GetCount())
        m_State.m_SnapshotIntensity[s] = status.m_SnapshotIntensities[j];
    }
  }

  plHybridArray<plUInt32, 8> order;
  for (plUInt32 s = 0; s < props.m_Snapshots.GetCount(); ++s)
  {
    if (m_State.m_SnapshotIntensity[s] > 0.0f)
      order.PushBack(s);
  }
  order.Sort([&](plUInt32 a, plUInt32 b)
    { return props.m_Snapshots[a].m_uiPriority < props.m_Snapshots[b].m_uiPriority; });

  for (plUInt32 i = 0; i < uiBusCount; ++i)
  {
    float fDb = props.m_Buses[i].m_fVolumeDb;

    for (plUInt32 s : order)
    {
      for (const plAudioMixerSnapshotOverride& ovr : props.m_Snapshots[s].m_Overrides)
      {
        if (ovr.m_eTarget == plAudioSnapshotTarget::BusVolume && ovr.m_sBus.IsEqual_NoCase(props.m_Buses[i].m_sName))
          fDb = plMath::Lerp(fDb, ovr.m_fValue, m_State.m_SnapshotIntensity[s]);
      }
    }

    m_State.m_BusSnapshotDb[i] = fDb;
  }
}

void plAudioMixerAssetDocumentWindow::SetSource(Source source)
{
  m_Source = source;
  m_pToolStrip->SetChecked(m_uiSourcePreview, source == Source::Preview);
  m_pToolStrip->SetChecked(m_uiSourceEngine, source == Source::EngineProcess);
  m_pToolStrip->SetEnabled(m_uiTestSignal, source == Source::Preview);

  // the test signal belongs to the preview
  if (source == Source::EngineProcess && m_bTestSignal)
  {
    m_bTestSignal = false;
    m_pToolStrip->SetChecked(m_uiTestSignal, false);
    SendConsoleCommand(plAudioMixerConsoleCommand::SetTestSignal, {}, 0.0f);
  }

  ShowToast(source == Source::Preview ? "Preview: the engine process plays this mixer" : "Engine process: shows the mixer that plays; edits apply live when it is this one");
  ApplyToEngine(true);
}

void plAudioMixerAssetDocumentWindow::OnLiveEdit(const plAudioMixerLiveEdit& edit)
{
  m_LiveEdit = edit;
  ApplyToEngine(false);
}

void plAudioMixerAssetDocumentWindow::OnMute(const plUuid& bus, bool bMute)
{
  const plDocumentObject* pBus = GetDocument()->GetObjectManager()->GetObject(bus);
  if (pBus == nullptr)
    return;

  if (!m_State.m_bLive)
  {
    ShowToast("Mute acts on the engine process's mixer; it does not play this one. Switch the source to Preview.");
    return;
  }

  SendConsoleCommand(plAudioMixerConsoleCommand::SetBusMute, GetName(pBus), bMute ? 1.0f : 0.0f);
}

void plAudioMixerAssetDocumentWindow::OnSolo(const plUuid& bus, bool bSolo)
{
  const plDocumentObject* pBus = GetDocument()->GetObjectManager()->GetObject(bus);
  if (pBus == nullptr)
    return;

  if (!m_State.m_bLive)
  {
    ShowToast("Solo acts on the engine process's mixer; it does not play this one. Switch the source to Preview.");
    return;
  }

  SendConsoleCommand(plAudioMixerConsoleCommand::SetBusSolo, GetName(pBus), bSolo ? 1.0f : 0.0f);
}

void plAudioMixerAssetDocumentWindow::OnSelectInsert(const plUuid& bus, plInt32 iEffect)
{
  m_State.m_SelectedBus = bus;
  m_State.m_iSelectedEffect = iEffect;

  // the insert editor shows what was clicked
  if (iEffect >= 0)
  {
    const plEditorUiEngineShell* pShell = plEditorUiEngineShell::GetSingleton();
    if (plEditorUiDocumentFrame* pFrame = pShell != nullptr ? pShell->FindFrame(GetDocument()) : nullptr)
      pFrame->GetPanels().SetCurrentPanel("AudioMixerInsertDockWidget");
  }

  m_pStripsView->Refresh();
  m_pInsertView->Refresh();
}

void plAudioMixerAssetDocumentWindow::OnSnapshotIntensity(const plUuid& snapshot, float fIntensity)
{
  const plDocumentObject* pSnapshot = GetDocument()->GetObjectManager()->GetObject(snapshot);
  if (pSnapshot == nullptr)
    return;

  if (!m_State.m_bLive)
  {
    ShowToast("Snapshots are auditioned in the engine process's mixer; it does not play this one. Switch the source to Preview.");
    return;
  }

  SendConsoleCommand(plAudioMixerConsoleCommand::SetSnapshot, GetName(pSnapshot), fIntensity);

  plStringBuilder sToast;
  sToast.SetFormat("Auditioning {} at {}: overridden faders show the blended value", GetName(pSnapshot), plArgF(fIntensity, 2));
  ShowToast(sToast);
}

void plAudioMixerAssetDocumentWindow::TakeBaseline()
{
  m_Baseline.Clear();

  plHybridArray<const plDocumentObject*, 12> buses;
  GatherChildren(GetPropertiesObject(), "Buses", buses);

  for (const plDocumentObject* pBus : buses)
  {
    m_Baseline[pBus->GetGuid()] = pBus->GetTypeAccessor().GetValue("Volume").ConvertTo<float>();
  }
}

void plAudioMixerAssetDocumentWindow::OnCapture(const plUuid& snapshot)
{
  const plDocumentObject* pSnapshot = GetDocument()->GetObjectManager()->GetObject(snapshot);
  if (pSnapshot == nullptr)
    return;

  plHybridArray<const plDocumentObject*, 12> buses, overrides;
  GatherChildren(GetPropertiesObject(), "Buses", buses);
  GatherChildren(pSnapshot, "Overrides", overrides);

  // the faders that moved since the window opened, the mix was saved or the last capture
  plHybridArray<const plDocumentObject*, 12> changed;
  for (const plDocumentObject* pBus : buses)
  {
    float fBaseline = 0.0f;
    if (m_Baseline.TryGetValue(pBus->GetGuid(), fBaseline) && plMath::Abs(pBus->GetTypeAccessor().GetValue("Volume").ConvertTo<float>() - fBaseline) > 0.05f)
      changed.PushBack(pBus);
  }

  if (changed.IsEmpty())
  {
    ShowToast("Capture: no fader differs from the authored mix. Move a fader first.");
    return;
  }

  plStringBuilder sTransaction("Capture into ", GetName(pSnapshot));

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction(sTransaction);

  for (const plDocumentObject* pBus : changed)
  {
    const plString sBus = GetName(pBus);
    const float fVolume = pBus->GetTypeAccessor().GetValue("Volume").ConvertTo<float>();

    // the snapshot's volume override of this bus takes the fader's value; without one, one is added
    const plDocumentObject* pOverride = nullptr;
    for (const plDocumentObject* pCandidate : overrides)
    {
      const auto& ovr = pCandidate->GetTypeAccessor();
      if (ovr.GetValue("Target").ConvertTo<plInt64>() == plAudioSnapshotTarget::BusVolume && ovr.GetValue("Bus").ConvertTo<plString>().IsEqual_NoCase(sBus))
        pOverride = pCandidate;
    }

    if (pOverride == nullptr)
    {
      plUuid overrideGuid;
      if (accessor.AddObjectByName(pSnapshot, "Overrides", -1, plGetStaticRTTI<plAudioMixerSnapshotOverride>(), overrideGuid).Failed())
        continue;

      pOverride = GetDocument()->GetObjectManager()->GetObject(overrideGuid);
      accessor.SetValueByName(pOverride, "Bus", sBus.GetView()).IgnoreResult();
      accessor.SetValueByName(pOverride, "Target", static_cast<plInt64>(plAudioSnapshotTarget::BusVolume)).IgnoreResult();
    }

    accessor.SetValueByName(pOverride, "Value", fVolume).IgnoreResult();

    // the fader goes back to the authored mix
    accessor.SetValueByName(pBus, "Volume", m_Baseline[pBus->GetGuid()]).IgnoreResult();
  }

  accessor.FinishTransaction();

  plStringBuilder sToast;
  sToast.SetFormat("Captured {} fader change(s) into {}; faders reset to the authored mix", changed.GetCount(), GetName(pSnapshot));
  ShowToast(sToast);
}

void plAudioMixerAssetDocumentWindow::RenameBus(const plUuid& bus, plStringView sNewName)
{
  const plDocumentObject* pBus = GetDocument()->GetObjectManager()->GetObject(bus);
  if (pBus == nullptr)
    return;

  const plString sOld = GetName(pBus);
  if (sOld == sNewName || sNewName.IsEmpty())
    return;

  m_bUpdatingReferences = true;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Rename Bus");
  accessor.SetValueByName(pBus, "Name", sNewName).IgnoreResult();
  const plUInt32 uiReferences = RenameBusReferences(accessor, GetPropertiesObject(), sOld, sNewName);
  accessor.FinishTransaction();

  m_bUpdatingReferences = false;

  const plUInt32 uiEvents = RenameEventOutputBuses(sOld, sNewName);

  plStringBuilder sToast;
  sToast.SetFormat("Renamed {} \xE2\x86\x92 {}: updated {} reference(s) in the mixer and {} event(s)", sOld, sNewName, uiReferences, uiEvents);
  ShowToast(sToast);
}

void plAudioMixerAssetDocumentWindow::UpdateReferencesAfterRename(plStringView sOldName, plStringView sNewName)
{
  m_bUpdatingReferences = true;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Update Bus References");
  const plUInt32 uiReferences = RenameBusReferences(accessor, GetPropertiesObject(), sOldName, sNewName);

  if (uiReferences > 0)
  {
    accessor.FinishTransaction();

    // one undo step with the rename itself
    GetDocument()->GetCommandHistory()->MergeLastTwoTransactions();
  }
  else
  {
    accessor.CancelTransaction();
  }

  m_bUpdatingReferences = false;

  const plUInt32 uiEvents = RenameEventOutputBuses(sOldName, sNewName);

  plStringBuilder sToast;
  sToast.SetFormat("Renamed {} \xE2\x86\x92 {}: updated {} reference(s) in the mixer and {} event(s)", sOldName, sNewName, uiReferences, uiEvents);
  ShowToast(sToast);
}

plUInt32 plAudioMixerAssetDocumentWindow::RenameEventOutputBuses(plStringView sOldName, plStringView sNewName)
{
  // the events that output to the bus: their files name it, so only those are opened
  plStringBuilder sNeedle;
  sNeedle.SetFormat("%OutputBus{\"{}\"}", sOldName);

  plHybridArray<plString, 16> eventPaths;
  {
    const auto knownSubAssets = plAssetCurator::GetSingleton()->GetKnownSubAssets();

    for (auto it = knownSubAssets->GetIterator(); it.IsValid(); ++it)
    {
      const plSubAsset& subAsset = it.Value();
      if (!subAsset.m_bMainAsset || subAsset.m_pAssetInfo == nullptr || subAsset.m_pAssetInfo->m_pDocumentTypeDescriptor == nullptr)
        continue;

      if (!subAsset.m_pAssetInfo->m_pDocumentTypeDescriptor->m_sDocumentTypeName.IsEqual_NoCase("Audio Event"))
        continue;

      eventPaths.PushBack(subAsset.m_pAssetInfo->m_Path.GetAbsolutePath());
    }
  }

  plUInt32 uiChanged = 0;

  for (const plString& sPath : eventPaths)
  {
    const plDocumentTypeDescriptor* pTypeDesc = nullptr;
    if (plDocumentManager::FindDocumentTypeFromPath(sPath, false, pTypeDesc).Failed())
      continue;

    plDocument* pDoc = pTypeDesc->m_pManager->GetDocumentByPath(sPath);
    const bool bOpenedHere = pDoc == nullptr;

    // an open event is checked as it is; a closed one is opened only when its file names the bus
    if (bOpenedHere)
    {
      plFileReader file;
      if (file.Open(sPath).Failed())
        continue;

      plStringBuilder sContent;
      sContent.ReadAll(file);
      if (sContent.FindSubString(sNeedle) == nullptr)
        continue;

      pDoc = plEditorApp::GetSingleton()->OpenDocument(sPath, plDocumentFlags::None);
      if (pDoc == nullptr)
        continue;
    }

    const auto& rootChildren = pDoc->GetObjectManager()->GetRootObject()->GetChildren();
    const plDocumentObject* pProps = rootChildren.IsEmpty() ? nullptr : rootChildren[0];

    if (pProps != nullptr && pProps->GetTypeAccessor().GetValue("OutputBus").ConvertTo<plString>().IsEqual_NoCase(sOldName))
    {
      plObjectCommandAccessor accessor(pDoc->GetCommandHistory());
      accessor.StartTransaction("Rename Output Bus");
      accessor.SetValueByName(pProps, "OutputBus", sNewName).IgnoreResult();
      accessor.FinishTransaction();

      // an event someone has open keeps the change unsaved, like any other edit
      if (!bOpenedHere || pDoc->SaveDocument(true).Succeeded())
        ++uiChanged;
      else
        plLog::Error("Saving the audio event '{0}' with its renamed output bus failed.", sPath);
    }

    if (bOpenedHere)
      pDoc->GetDocumentManager()->CloseDocument(pDoc);
  }

  return uiChanged;
}

void plAudioMixerAssetDocumentWindow::ShowToast(plStringView sText)
{
  m_sToast = sText;
  m_ToastTime = plTime::Now();
  UpdateStatusRow();
}

void plAudioMixerAssetDocumentWindow::UpdateStatusRow()
{
  if (m_pStatusView == nullptr)
    return;

  const plAudioMixerConsoleStatusMsgToEditor& status = m_pAssetDoc->GetConsoleStatus();

  plStringBuilder sSource = m_Source == Source::Preview ? "Preview" : "Engine process";
  plStringBuilder sPlays;
  if (!status.m_bAvailable)
    sPlays = "no PlasmaAudio in the engine process";
  else if (status.m_bIsThisMixer)
    sPlays.SetFormat("plays this mixer \xC2\xB7 {} voices", status.m_uiRealVoices);
  else if (status.m_bHasMixer)
    sPlays = "plays another mixer";
  else
    sPlays = "no mixer loaded";

  plStringBuilder sRebuilds;
  sRebuilds.SetFormat("{}", status.m_uiLayoutRebuilds);

  plHybridArray<plEditorUiAudioStatusRow::Part, 4> parts;
  parts.PushBack({sSource, sPlays});
  parts.PushBack({"no voices stopped", "hot-apply"});
  parts.PushBack({sRebuilds, "layout rebuilds"});
  m_pStatusView->SetParts(parts);
  m_pStatusView->SetRight(m_sToast.IsEmpty() ? plStringView("backend: PlasmaAudio") : m_sToast.GetView());
}

void plAudioMixerAssetDocumentWindow::OnAddBus(bool bAux)
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plHybridArray<const plDocumentObject*, 12> buses;
  GatherChildren(pProps, "Buses", buses);

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction(bAux ? "Add Aux Bus" : "Add Bus");

  plUuid busGuid;
  if (accessor.AddObjectByName(pProps, "Buses", -1, plGetStaticRTTI<plAudioMixerBus>(), busGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  const plDocumentObject* pBus = GetDocument()->GetObjectManager()->GetObject(busGuid);

  plStringBuilder sName;
  if (buses.IsEmpty())
  {
    sName = "Master";
  }
  else
  {
    sName.SetFormat(bAux ? "Reverb {0}" : "Bus {0}", buses.GetCount());
  }

  accessor.SetValueByName(pBus, "Name", sName.GetView()).IgnoreResult();

  if (bAux)
  {
    accessor.SetValueByName(pBus, "IsAux", true).IgnoreResult();
  }

  accessor.FinishTransaction();

  m_Baseline[busGuid] = 0.0f;
  GetDocument()->GetSelectionManager()->SetSelection(pBus);
}

void plAudioMixerAssetDocumentWindow::OnAddVca()
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add VCA");

  plUuid vcaGuid;
  if (accessor.AddObjectByName(pProps, "VCAs", -1, plGetStaticRTTI<plAudioMixerVca>(), vcaGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  accessor.FinishTransaction();

  GetDocument()->GetSelectionManager()->SetSelection(GetDocument()->GetObjectManager()->GetObject(vcaGuid));
}

void plAudioMixerAssetDocumentWindow::OnAddSnapshot()
{
  const plDocumentObject* pProps = GetPropertiesObject();
  if (pProps == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Add Snapshot");

  plUuid snapshotGuid;
  if (accessor.AddObjectByName(pProps, "Snapshots", -1, plGetStaticRTTI<plAudioMixerSnapshot>(), snapshotGuid).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  accessor.FinishTransaction();

  GetDocument()->GetSelectionManager()->SetSelection(GetDocument()->GetObjectManager()->GetObject(snapshotGuid));
}

void plAudioMixerAssetDocumentWindow::OnRemoveSelected()
{
  const plDocumentObject* pSelected = GetDocument()->GetSelectionManager()->GetCurrentObject();

  if (pSelected == nullptr || pSelected == GetPropertiesObject() || pSelected->GetParent() == GetDocument()->GetObjectManager()->GetRootObject())
  {
    ShowTemporaryStatusBarMsg("Select a bus, VCA or snapshot to remove.");
    return;
  }

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Remove Selected");

  if (accessor.RemoveObject(pSelected).Failed())
  {
    accessor.CancelTransaction();
    return;
  }

  accessor.FinishTransaction();

  GetDocument()->GetSelectionManager()->SetSelection(GetPropertiesObject());
}
