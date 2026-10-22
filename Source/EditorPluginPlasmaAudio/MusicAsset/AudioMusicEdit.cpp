#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAsset.h>
#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicAsset.h>
#include <EditorPluginPlasmaAudio/MusicAsset/AudioMusicEdit.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

namespace
{
  template <typename T>
  bool IsNameTaken(const plDynamicArray<T>& elements, plStringView sName, plUInt32 uiSkip)
  {
    for (plUInt32 i = 0; i < elements.GetCount(); ++i)
    {
      if (i != uiSkip && elements[i].m_sName.IsEqual_NoCase(sName))
        return true;
    }
    return false;
  }

  /// sBase, or "sBase 2", "sBase 3", ... when another element has it.
  template <typename T>
  plString UniqueName(const plDynamicArray<T>& elements, plStringView sBase)
  {
    if (!IsNameTaken(elements, sBase, plInvalidIndex))
      return sBase;

    plStringBuilder sName;
    for (plUInt32 n = 2;; ++n)
    {
      sName.SetFormat("{} {}", sBase, n);
      if (!IsNameTaken(elements, sName.GetView(), plInvalidIndex))
        return sName;
    }
  }

  /// "Prefix N" with the first N from the count that no element has.
  template <typename T>
  plString NumberedName(const plDynamicArray<T>& elements, const char* szPrefix)
  {
    plStringBuilder sName;
    for (plUInt32 n = elements.GetCount() + 1;; ++n)
    {
      sName.SetFormat("{} {}", szPrefix, n);
      if (!IsNameTaken(elements, sName.GetView(), plInvalidIndex))
        return sName;
    }
  }

  /// One undo step: Finish commits it, otherwise it is cancelled when the scope ends.
  class Transaction
  {
  public:
    Transaction(plAudioMusicAssetDocument* pDocument, plStringView sName)
      : m_Accessor(pDocument->GetCommandHistory())
    {
      m_Accessor.StartTransaction(sName);
    }

    ~Transaction()
    {
      if (!m_bDone)
        m_Accessor.CancelTransaction();
    }

    void Finish()
    {
      m_bDone = true;
      m_Accessor.FinishTransaction();
    }

    plObjectCommandAccessor* operator->() { return &m_Accessor; }

  private:
    plObjectCommandAccessor m_Accessor;
    bool m_bDone = false;
  };

  const plDocumentObject* AddElement(plAudioMusicAssetDocument* pDocument, Transaction& ref_transaction, const plDocumentObject* pParent, const char* szArray, const plRTTI* pType)
  {
    plUuid guid = plUuid::MakeUuid();
    if (ref_transaction->AddObjectByName(pParent, szArray, -1, pType, guid).Failed())
      return nullptr;

    return pDocument->GetObjectManager()->GetObject(guid);
  }
} // namespace

const plDocumentObject* plAudioMusicEdit::GetProperties(const plAudioMusicAssetDocument* pDocument)
{
  return pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0];
}

const plDocumentObject* plAudioMusicEdit::GetElement(const plAudioMusicAssetDocument* pDocument, const plDocumentObject* pParent, const char* szArray, plUInt32 uiIndex)
{
  if (pParent == nullptr || uiIndex >= static_cast<plUInt32>(pParent->GetTypeAccessor().GetCount(szArray)))
    return nullptr;

  const plVariant value = pParent->GetTypeAccessor().GetValue(szArray, uiIndex);
  return value.IsA<plUuid>() ? pDocument->GetObjectManager()->GetObject(value.Get<plUuid>()) : nullptr;
}

const plDocumentObject* plAudioMusicEdit::GetElement(const plAudioMusicAssetDocument* pDocument, const char* szArray, plUInt32 uiIndex)
{
  return GetElement(pDocument, GetProperties(pDocument), szArray, uiIndex);
}

double plAudioMusicEdit::GetClipSeconds(plStringView sClip)
{
  plAudioClipAssetMetaInfo info;
  return plAudioClipAssetUtils::GetClipInfo(sClip, info) ? info.m_fDuration : 0.0;
}

plString plAudioMusicEdit::GetClipName(plStringView sClip)
{
  plAudioClipAssetMetaInfo info;
  plString sName;
  if (sClip.IsEmpty() || !plAudioClipAssetUtils::GetClipInfo(sClip, info, &sName))
    return "missing clip";

  return sName;
}

plUInt32 plAudioMusicEdit::GetBarsForSeconds(double fSeconds, float fTempo, plUInt32 uiBeatsPerBar)
{
  const double fBar = 60.0 / plMath::Max(static_cast<double>(fTempo), 1.0) * plMath::Max(uiBeatsPerBar, 1u);
  return plMath::Clamp(static_cast<plUInt32>(plMath::Round(fSeconds / fBar)), 1u, 512u);
}

plUInt32 plAudioMusicEdit::AddSegmentFromClip(plAudioMusicAssetDocument* pDocument, plStringView sClip, float fTempo, plUInt32 uiState, plUInt32 uiPosition)
{
  const plAudioMusicAssetProperties* pProp = pDocument->GetProperties();
  const plString sName = UniqueName(pProp->m_Segments, GetClipName(sClip));
  const plUInt32 uiBars = GetBarsForSeconds(GetClipSeconds(sClip), fTempo, 4);

  Transaction transaction(pDocument, "Add Segment");
  const plDocumentObject* pSegment = AddElement(pDocument, transaction, GetProperties(pDocument), "Segments", plGetStaticRTTI<plAudioMusicSegment>());
  if (pSegment == nullptr)
    return plInvalidIndex;

  transaction->SetValueByName(pSegment, "Name", sName).IgnoreResult();
  transaction->SetValueByName(pSegment, "Tempo", fTempo).IgnoreResult();
  transaction->SetValueByName(pSegment, "Bars", uiBars).IgnoreResult();

  const plDocumentObject* pStem = AddElement(pDocument, transaction, pSegment, "Stems", plGetStaticRTTI<plAudioMusicStem>());
  if (pStem == nullptr)
    return plInvalidIndex;

  transaction->SetValueByName(pStem, "Clip", sClip).IgnoreResult();

  if (const plDocumentObject* pState = GetElement(pDocument, "States", uiState))
  {
    const plUInt32 uiCount = pProp->m_States[uiState].m_Segments.GetCount();
    transaction->InsertValueByName(pState, "Segments", sName, plMath::Min(uiPosition, uiCount)).IgnoreResult();
  }

  transaction.Finish();
  return pProp->m_Segments.GetCount() - 1;
}

plUInt32 plAudioMusicEdit::AddEmptySegment(plAudioMusicAssetDocument* pDocument, float fTempo, plUInt32 uiState)
{
  const plAudioMusicAssetProperties* pProp = pDocument->GetProperties();
  const plString sName = NumberedName(pProp->m_Segments, "Segment");

  Transaction transaction(pDocument, "Add Segment");
  const plDocumentObject* pSegment = AddElement(pDocument, transaction, GetProperties(pDocument), "Segments", plGetStaticRTTI<plAudioMusicSegment>());
  if (pSegment == nullptr)
    return plInvalidIndex;

  transaction->SetValueByName(pSegment, "Name", sName).IgnoreResult();
  transaction->SetValueByName(pSegment, "Tempo", fTempo).IgnoreResult();

  if (const plDocumentObject* pState = GetElement(pDocument, "States", uiState))
    transaction->InsertValueByName(pState, "Segments", sName, -1).IgnoreResult();

  transaction.Finish();
  return pProp->m_Segments.GetCount() - 1;
}

void plAudioMusicEdit::DeleteSegment(plAudioMusicAssetDocument* pDocument, plUInt32 uiSegment)
{
  const plAudioMusicAssetProperties* pProp = pDocument->GetProperties();
  const plDocumentObject* pSegment = GetElement(pDocument, "Segments", uiSegment);
  if (pSegment == nullptr)
    return;

  const plString sName = pProp->m_Segments[uiSegment].m_sName;

  // what refers to it, gathered before anything changes (the typed properties follow every step)
  struct Entry
  {
    const plDocumentObject* m_pState = nullptr;
    plUInt32 m_uiPosition = 0;
  };
  plHybridArray<Entry, 8> entries;
  for (plUInt32 uiState = 0; uiState < pProp->m_States.GetCount(); ++uiState)
  {
    const plDynamicArray<plString>& segments = pProp->m_States[uiState].m_Segments;
    for (plUInt32 i = segments.GetCount(); i > 0; --i)
    {
      if (segments[i - 1].IsEqual_NoCase(sName))
        entries.PushBack({GetElement(pDocument, "States", uiState), i - 1});
    }
  }

  plHybridArray<const plDocumentObject*, 8> vias;
  for (plUInt32 i = 0; i < pProp->m_Transitions.GetCount(); ++i)
  {
    if (pProp->m_Transitions[i].m_sVia.IsEqual_NoCase(sName))
      vias.PushBack(GetElement(pDocument, "Transitions", i));
  }

  Transaction transaction(pDocument, "Delete Segment");
  for (const Entry& entry : entries)
    transaction->RemoveValueByName(entry.m_pState, "Segments", entry.m_uiPosition).IgnoreResult();

  for (const plDocumentObject* pTransition : vias)
    transaction->SetValueByName(pTransition, "Via", "").IgnoreResult();

  if (transaction->RemoveObject(pSegment).Failed())
    return;

  transaction.Finish();
}

void plAudioMusicEdit::RenameSegment(plAudioMusicAssetDocument* pDocument, plUInt32 uiSegment, plStringView sName)
{
  const plAudioMusicAssetProperties* pProp = pDocument->GetProperties();
  const plDocumentObject* pSegment = GetElement(pDocument, "Segments", uiSegment);
  plStringBuilder sNewName = sName;
  sNewName.Trim(" \t");

  if (pSegment == nullptr || sNewName.IsEmpty() || pProp->m_Segments[uiSegment].m_sName == sNewName || IsNameTaken(pProp->m_Segments, sNewName.GetView(), uiSegment))
    return;

  const plString sOldName = pProp->m_Segments[uiSegment].m_sName;

  Transaction transaction(pDocument, "Rename Segment");
  for (plUInt32 uiState = 0; uiState < pProp->m_States.GetCount(); ++uiState)
  {
    const plDocumentObject* pState = GetElement(pDocument, "States", uiState);
    for (plUInt32 i = 0; i < pProp->m_States[uiState].m_Segments.GetCount(); ++i)
    {
      if (pProp->m_States[uiState].m_Segments[i].IsEqual_NoCase(sOldName))
        transaction->SetValueByName(pState, "Segments", sNewName.GetView(), i).IgnoreResult();
    }
  }

  for (plUInt32 i = 0; i < pProp->m_Transitions.GetCount(); ++i)
  {
    if (pProp->m_Transitions[i].m_sVia.IsEqual_NoCase(sOldName))
      transaction->SetValueByName(GetElement(pDocument, "Transitions", i), "Via", sNewName.GetView()).IgnoreResult();
  }

  transaction->SetValueByName(pSegment, "Name", sNewName.GetView()).IgnoreResult();
  transaction.Finish();
}

void plAudioMusicEdit::ToggleExitCue(plAudioMusicAssetDocument* pDocument, plUInt32 uiSegment, plUInt32 uiBar)
{
  const plDocumentObject* pSegment = GetElement(pDocument, "Segments", uiSegment);
  if (pSegment == nullptr)
    return;

  const plDynamicArray<plUInt32>& cues = pDocument->GetProperties()->m_Segments[uiSegment].m_ExitCues;
  const plUInt32 uiIndex = cues.IndexOf(uiBar);

  Transaction transaction(pDocument, "Set Exit Cue");
  if (uiIndex != plInvalidIndex)
    transaction->RemoveValueByName(pSegment, "ExitCues", uiIndex).IgnoreResult();
  else
    transaction->InsertValueByName(pSegment, "ExitCues", uiBar, -1).IgnoreResult();
  transaction.Finish();
}

void plAudioMusicEdit::SetBars(plAudioMusicAssetDocument* pDocument, plUInt32 uiSegment, plUInt32 uiBars)
{
  const plDocumentObject* pSegment = GetElement(pDocument, "Segments", uiSegment);
  const plAudioMusicSegment& segment = pDocument->GetProperties()->m_Segments[uiSegment];
  uiBars = plMath::Clamp(uiBars, 1u, 512u);
  if (pSegment == nullptr || segment.m_uiBars == uiBars)
    return;

  // a cue after the last bar is no cue
  plHybridArray<plUInt32, 8> outside;
  for (plUInt32 i = segment.m_ExitCues.GetCount(); i > 0; --i)
  {
    if (segment.m_ExitCues[i - 1] >= uiBars)
      outside.PushBack(i - 1);
  }

  Transaction transaction(pDocument, "Set Bars");
  for (plUInt32 uiIndex : outside)
    transaction->RemoveValueByName(pSegment, "ExitCues", uiIndex).IgnoreResult();

  transaction->SetValueByName(pSegment, "Bars", uiBars).IgnoreResult();
  transaction.Finish();
}

void plAudioMusicEdit::AddStem(plAudioMusicAssetDocument* pDocument, plUInt32 uiSegment, plStringView sClip)
{
  const plDocumentObject* pSegment = GetElement(pDocument, "Segments", uiSegment);
  if (pSegment == nullptr)
    return;

  Transaction transaction(pDocument, "Add Stem");
  const plDocumentObject* pStem = AddElement(pDocument, transaction, pSegment, "Stems", plGetStaticRTTI<plAudioMusicStem>());
  if (pStem == nullptr)
    return;

  transaction->SetValueByName(pStem, "Clip", sClip).IgnoreResult();
  transaction.Finish();
}

void plAudioMusicEdit::RemoveStem(plAudioMusicAssetDocument* pDocument, plUInt32 uiSegment, plUInt32 uiStem)
{
  const plDocumentObject* pStem = GetElement(pDocument, GetElement(pDocument, "Segments", uiSegment), "Stems", uiStem);
  if (pStem == nullptr)
    return;

  Transaction transaction(pDocument, "Remove Stem");
  if (transaction->RemoveObject(pStem).Failed())
    return;

  transaction.Finish();
}

plUInt32 plAudioMusicEdit::AddState(plAudioMusicAssetDocument* pDocument)
{
  const plAudioMusicAssetProperties* pProp = pDocument->GetProperties();
  const plString sName = NumberedName(pProp->m_States, "State");

  Transaction transaction(pDocument, "Add State");
  const plDocumentObject* pState = AddElement(pDocument, transaction, GetProperties(pDocument), "States", plGetStaticRTTI<plAudioMusicState>());
  if (pState == nullptr)
    return plInvalidIndex;

  transaction->SetValueByName(pState, "Name", sName).IgnoreResult();
  transaction.Finish();
  return pProp->m_States.GetCount() - 1;
}

void plAudioMusicEdit::DeleteState(plAudioMusicAssetDocument* pDocument, plUInt32 uiState)
{
  const plAudioMusicAssetProperties* pProp = pDocument->GetProperties();
  const plDocumentObject* pState = GetElement(pDocument, "States", uiState);
  if (pState == nullptr)
    return;

  const plString sName = pProp->m_States[uiState].m_sName;

  plHybridArray<const plDocumentObject*, 8> transitions;
  for (plUInt32 i = 0; i < pProp->m_Transitions.GetCount(); ++i)
  {
    if (pProp->m_Transitions[i].m_sFrom.IsEqual_NoCase(sName) || pProp->m_Transitions[i].m_sTo.IsEqual_NoCase(sName))
      transitions.PushBack(GetElement(pDocument, "Transitions", i));
  }

  Transaction transaction(pDocument, "Delete State");
  for (const plDocumentObject* pTransition : transitions)
    transaction->RemoveObject(pTransition).IgnoreResult();

  if (pProp->m_sInitialState.IsEqual_NoCase(sName))
    transaction->SetValueByName(GetProperties(pDocument), "InitialState", "").IgnoreResult();

  if (transaction->RemoveObject(pState).Failed())
    return;

  transaction.Finish();
}

void plAudioMusicEdit::RenameState(plAudioMusicAssetDocument* pDocument, plUInt32 uiState, plStringView sName)
{
  const plAudioMusicAssetProperties* pProp = pDocument->GetProperties();
  const plDocumentObject* pState = GetElement(pDocument, "States", uiState);
  plStringBuilder sNewName = sName;
  sNewName.Trim(" \t");

  if (pState == nullptr || sNewName.IsEmpty() || pProp->m_States[uiState].m_sName == sNewName || IsNameTaken(pProp->m_States, sNewName.GetView(), uiState))
    return;

  const plString sOldName = pProp->m_States[uiState].m_sName;

  Transaction transaction(pDocument, "Rename State");
  for (plUInt32 i = 0; i < pProp->m_Transitions.GetCount(); ++i)
  {
    const plDocumentObject* pTransition = GetElement(pDocument, "Transitions", i);
    if (pProp->m_Transitions[i].m_sFrom.IsEqual_NoCase(sOldName))
      transaction->SetValueByName(pTransition, "From", sNewName.GetView()).IgnoreResult();
    if (pProp->m_Transitions[i].m_sTo.IsEqual_NoCase(sOldName))
      transaction->SetValueByName(pTransition, "To", sNewName.GetView()).IgnoreResult();
  }

  if (pProp->m_sInitialState.IsEqual_NoCase(sOldName))
    transaction->SetValueByName(GetProperties(pDocument), "InitialState", sNewName.GetView()).IgnoreResult();

  transaction->SetValueByName(pState, "Name", sNewName.GetView()).IgnoreResult();
  transaction.Finish();
}

void plAudioMusicEdit::SetInitialState(plAudioMusicAssetDocument* pDocument, plUInt32 uiState)
{
  const plAudioMusicAssetProperties* pProp = pDocument->GetProperties();
  if (uiState >= pProp->m_States.GetCount())
    return;

  SetValue(pDocument, GetProperties(pDocument), "InitialState", pProp->m_States[uiState].m_sName, "Set Initial State");
}

void plAudioMusicEdit::InsertStateSegment(plAudioMusicAssetDocument* pDocument, plUInt32 uiState, plUInt32 uiPosition, plUInt32 uiSegment)
{
  const plAudioMusicAssetProperties* pProp = pDocument->GetProperties();
  const plDocumentObject* pState = GetElement(pDocument, "States", uiState);
  if (pState == nullptr || uiSegment >= pProp->m_Segments.GetCount())
    return;

  const plUInt32 uiCount = pProp->m_States[uiState].m_Segments.GetCount();

  Transaction transaction(pDocument, "Add Segment to State");
  transaction->InsertValueByName(pState, "Segments", pProp->m_Segments[uiSegment].m_sName, plMath::Min(uiPosition, uiCount)).IgnoreResult();
  transaction.Finish();
}

void plAudioMusicEdit::RemoveStateSegment(plAudioMusicAssetDocument* pDocument, plUInt32 uiState, plUInt32 uiPosition)
{
  const plDocumentObject* pState = GetElement(pDocument, "States", uiState);
  if (pState == nullptr || uiPosition >= pDocument->GetProperties()->m_States[uiState].m_Segments.GetCount())
    return;

  Transaction transaction(pDocument, "Remove Segment from State");
  transaction->RemoveValueByName(pState, "Segments", uiPosition).IgnoreResult();
  transaction.Finish();
}

void plAudioMusicEdit::MoveStateSegment(plAudioMusicAssetDocument* pDocument, plUInt32 uiState, plUInt32 uiFrom, plUInt32 uiTo)
{
  const plDocumentObject* pState = GetElement(pDocument, "States", uiState);
  if (pState == nullptr)
    return;

  const plDynamicArray<plString>& segments = pDocument->GetProperties()->m_States[uiState].m_Segments;
  uiTo = plMath::Min(uiTo, segments.GetCount() - 1);
  if (uiFrom >= segments.GetCount() || uiFrom == uiTo)
    return;

  // out and in again, so uiTo is where it ends up
  const plString sName = segments[uiFrom];

  Transaction transaction(pDocument, "Reorder Segments");
  transaction->RemoveValueByName(pState, "Segments", uiFrom).IgnoreResult();
  transaction->InsertValueByName(pState, "Segments", sName, uiTo).IgnoreResult();
  transaction.Finish();
}

plUInt32 plAudioMusicEdit::FindTransition(const plAudioMusicAssetDocument* pDocument, plUInt32 uiFrom, plUInt32 uiTo)
{
  const plAudioMusicAssetProperties* pProp = pDocument->GetProperties();
  if (uiTo >= pProp->m_States.GetCount() || (uiFrom != plInvalidIndex && uiFrom >= pProp->m_States.GetCount()))
    return plInvalidIndex;

  const plStringView sFrom = uiFrom == plInvalidIndex ? plStringView() : pProp->m_States[uiFrom].m_sName.GetView();
  const plStringView sTo = pProp->m_States[uiTo].m_sName;

  for (plUInt32 i = 0; i < pProp->m_Transitions.GetCount(); ++i)
  {
    const plAudioMusicTransition& transition = pProp->m_Transitions[i];
    if (transition.m_sFrom.IsEqual_NoCase(sFrom) && transition.m_sTo.IsEqual_NoCase(sTo))
      return i;
  }

  return plInvalidIndex;
}

plUInt32 plAudioMusicEdit::AddTransition(plAudioMusicAssetDocument* pDocument, plUInt32 uiFrom, plUInt32 uiTo)
{
  const plAudioMusicAssetProperties* pProp = pDocument->GetProperties();
  if (uiTo >= pProp->m_States.GetCount())
    return plInvalidIndex;

  if (const plUInt32 uiExisting = FindTransition(pDocument, uiFrom, uiTo); uiExisting != plInvalidIndex)
    return uiExisting;

  Transaction transaction(pDocument, "Add Transition");
  const plDocumentObject* pTransition = AddElement(pDocument, transaction, GetProperties(pDocument), "Transitions", plGetStaticRTTI<plAudioMusicTransition>());
  if (pTransition == nullptr)
    return plInvalidIndex;

  if (uiFrom < pProp->m_States.GetCount())
    transaction->SetValueByName(pTransition, "From", pProp->m_States[uiFrom].m_sName).IgnoreResult();
  transaction->SetValueByName(pTransition, "To", pProp->m_States[uiTo].m_sName).IgnoreResult();
  transaction.Finish();
  return pProp->m_Transitions.GetCount() - 1;
}

void plAudioMusicEdit::RemoveTransition(plAudioMusicAssetDocument* pDocument, plUInt32 uiTransition)
{
  const plDocumentObject* pTransition = GetElement(pDocument, "Transitions", uiTransition);
  if (pTransition == nullptr)
    return;

  Transaction transaction(pDocument, "Remove Transition");
  if (transaction->RemoveObject(pTransition).Failed())
    return;

  transaction.Finish();
}

plUInt32 plAudioMusicEdit::AddStinger(plAudioMusicAssetDocument* pDocument, plStringView sClip)
{
  const plAudioMusicAssetProperties* pProp = pDocument->GetProperties();
  const plString sName = sClip.IsEmpty() ? NumberedName(pProp->m_Stingers, "Stinger") : UniqueName(pProp->m_Stingers, GetClipName(sClip));

  Transaction transaction(pDocument, "Add Stinger");
  const plDocumentObject* pStinger = AddElement(pDocument, transaction, GetProperties(pDocument), "Stingers", plGetStaticRTTI<plAudioMusicStinger>());
  if (pStinger == nullptr)
    return plInvalidIndex;

  transaction->SetValueByName(pStinger, "Name", sName).IgnoreResult();
  if (!sClip.IsEmpty())
    transaction->SetValueByName(pStinger, "Clip", sClip).IgnoreResult();
  transaction.Finish();
  return pProp->m_Stingers.GetCount() - 1;
}

void plAudioMusicEdit::DeleteStinger(plAudioMusicAssetDocument* pDocument, plUInt32 uiStinger)
{
  const plDocumentObject* pStinger = GetElement(pDocument, "Stingers", uiStinger);
  if (pStinger == nullptr)
    return;

  Transaction transaction(pDocument, "Delete Stinger");
  if (transaction->RemoveObject(pStinger).Failed())
    return;

  transaction.Finish();
}

void plAudioMusicEdit::RenameStinger(plAudioMusicAssetDocument* pDocument, plUInt32 uiStinger, plStringView sName)
{
  const plAudioMusicAssetProperties* pProp = pDocument->GetProperties();
  plStringBuilder sNewName = sName;
  sNewName.Trim(" \t");

  if (uiStinger >= pProp->m_Stingers.GetCount() || sNewName.IsEmpty() || IsNameTaken(pProp->m_Stingers, sNewName.GetView(), uiStinger))
    return;

  SetValue(pDocument, GetElement(pDocument, "Stingers", uiStinger), "Name", sNewName.GetView(), "Rename Stinger");
}

void plAudioMusicEdit::SetValue(plAudioMusicAssetDocument* pDocument, const plDocumentObject* pObject, const char* szProperty, const plVariant& value, const char* szTransaction)
{
  if (pObject == nullptr || pObject->GetTypeAccessor().GetValue(szProperty) == value)
    return;

  Transaction transaction(pDocument, szTransaction);
  transaction->SetValueByName(pObject, szProperty, value).IgnoreResult();
  transaction.Finish();
}
