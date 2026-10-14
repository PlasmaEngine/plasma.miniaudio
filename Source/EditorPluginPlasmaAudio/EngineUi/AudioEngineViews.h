#pragma once

#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <GameUiPlugin/Elements/UiDrawList.h>
#include <GuiFoundation/EngineUi/EngineUiWidget.h>

/// \brief A section header row (26 px) above a Qt-hosted panel such as a property grid: the 3 px edge in a system
/// colour, the uppercase title and an optional count.
class plQtAudioSectionHeaderView : public plQtEngineUiWidget
{
public:
  plQtAudioSectionHeaderView(QWidget* pParent, plStringView sTitle, plColorGammaUB color);

  void SetTitle(plStringView sTitle);
  void SetCount(plStringView sCount);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

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
class plQtAudioStatusRowView : public plQtEngineUiWidget
{
public:
  explicit plQtAudioStatusRowView(QWidget* pParent);

  /// One count: "3" "instruments".
  struct Part
  {
    plString m_sValue;
    plString m_sLabel;
  };

  void SetParts(plArrayPtr<const Part> parts);
  void SetRight(plStringView sText);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

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
class plQtAudioMeterView : public plQtEngineUiWidget
{
public:
  explicit plQtAudioMeterView(QWidget* pParent);

  /// Feeds a new linear peak (0..1).
  void SetPeak(float fLinearPeak);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

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
