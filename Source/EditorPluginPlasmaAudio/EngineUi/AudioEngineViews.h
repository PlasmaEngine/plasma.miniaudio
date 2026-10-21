#pragma once

#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <EditorUi/Content.h>
#include <GameUiPlugin/Elements/UiDrawList.h>

/// \brief A section header row (26 px) above a panel such as a property grid: the 3 px edge in a system colour, the
/// uppercase title and an optional count.
class plEditorUiAudioSectionHeader : public plEditorUiContent
{
public:
  plEditorUiAudioSectionHeader(plStringView sTitle, plColorGammaUB color);

  void SetTitle(plStringView sTitle);
  void SetCount(plStringView sCount);

  static float GetHeight();

protected:
  void OnUiUpdate() override;

private:
  plString m_sTitle;
  plString m_sCount;
  plColorGammaUB m_Color;
  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vDrawnSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
};

/// \brief A panel's status row (style pass D11): counts on the left, each value in the code face and the label
/// dim after it, and a dim note on the right.
class plEditorUiAudioStatusRow : public plEditorUiContent
{
public:
  plEditorUiAudioStatusRow();

  /// One count: "3" "instruments".
  struct Part
  {
    plString m_sValue;
    plString m_sLabel;
  };

  void SetParts(plArrayPtr<const Part> parts);
  void SetRight(plStringView sText);

  static float GetHeight();

protected:
  void OnUiUpdate() override;

private:
  plDynamicArray<Part> m_Parts;
  plString m_sRight;
  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vDrawnSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
};

/// \brief A horizontal output meter on the -60..0 dBFS scale with its readout, for a toolbar row: "OUT" is the
/// strip's label, this view draws the meter and "-3.1 dB" after it. The peak line holds and decays.
class plEditorUiAudioMeter : public plEditorUiContent
{
public:
  plEditorUiAudioMeter();

  /// Feeds a new linear peak (0..1).
  void SetPeak(float fLinearPeak);

  static plVec2 GetSize();

  plString GetToolTipAt(const plVec2& vLogical) const override;

protected:
  void OnUiUpdate() override;

private:
  float m_fPeak = 0.0f;
  float m_fPeakHold = 0.0f;
  bool m_bRebuild = true;
  bool m_bDrawDirty = true;
  plVec2 m_vDrawnSize = plVec2::MakeZero();
  plUiDrawListWriter m_Writer;
};
