#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <EditorPluginPlasmaAudio/EngineUi/AudioEngineUi.h>
#include <Foundation/Math/ColorScheme.h>
#include <Foundation/Utilities/CommandLineUtils.h>

namespace plAudioEngineUi
{
  namespace
  {
    constexpr plUInt32 s_uiChipFontSize = plEditorUi::FontSize - 1;
    constexpr float s_fChipPadding = 8.0f;
    constexpr float s_fChipDot = 7.0f;
  } // namespace

  bool UseEngineViews()
  {
    return plCommandLineUtils::GetGlobalInstance()->GetBoolOption("-engineAudioEditors", true);
  }

  plColorGammaUB AudioColor()
  {
    return CategoryColor("Audio");
  }

  plColorGammaUB CategoryColor(plStringView sCategory)
  {
    return plColorGammaUB(plColorScheme::GetCategoryColor(sCategory, plColorScheme::CategoryColorUsage::BorderColor));
  }

  plColorGammaUB WithAlpha(const plColorGammaUB& color, plUInt8 uiAlpha)
  {
    return plColorGammaUB(color.r, color.g, color.b, uiAlpha);
  }

  plColorGammaUB Mix(const plColorGammaUB& a, const plColorGammaUB& b, float fT)
  {
    auto mix = [fT](plUInt8 x, plUInt8 y)
    { return static_cast<plUInt8>(plMath::Clamp(x + (y - x) * fT + 0.5f, 0.0f, 255.0f)); };

    return plColorGammaUB(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), 255);
  }

  void Place(plUiElement* pElement, const plRectFloat& rect)
  {
    pElement->m_vAnchorMin = pElement->m_vAnchorMax = plVec2::MakeZero();
    pElement->m_vOffsets = plVec4(rect.x, rect.y, rect.width, rect.height);
  }

  plUiButton* HitButton(plUiElement* pParent, const plRectFloat& rect, plStringView sAction, plStringView sToolTip, plBlackboard* pBoard)
  {
    plUiButton* pButton = plEditorUi::Button(pParent, "", sAction);
    pButton->ClearChildren();
    Place(pButton, rect);
    pButton->m_Color = plColorGammaUB(0, 0, 0, 0);
    pButton->m_HoverColor = plColorGammaUB(255, 255, 255, 10);
    pButton->m_PressedColor = plColorGammaUB(255, 255, 255, 18);
    pButton->m_DisabledColor = plColorGammaUB(0, 0, 0, 0);
    pButton->m_vCornerRadius = plVec4(4);

    if (pBoard != nullptr && !sToolTip.IsEmpty())
      plEditorUi::SetToolTip(*pBoard, sAction, sToolTip);

    return pButton;
  }

  void Icon(plUiDrawListWriter& ref_writer, const plRectFloat& rect, plStringView sSvgPath, const plColorGammaUB& tint, bool bMonochrome)
  {
    plString sResource;
    plEditorUi::SvgTexture(bMonochrome ? plEditorUi::MonochromeIconPath(sSvgPath).GetView() : sSvgPath, &sResource);

    if (!sResource.IsEmpty())
      ref_writer.Image(rect, sResource, tint);
  }

  void SectionHeader(plUiDrawListWriter& ref_writer, const plRectFloat& rect, plStringView sTitle, const plColorGammaUB& color, plStringView sCount)
  {
    using namespace plEditorUi;

    // between Surface and Raised, as the toolkit's section headers
    ref_writer.Rect(rect, Mix(Surface(), Raised(), 0.5f));
    ref_writer.Rect(plRectFloat(rect.x, rect.y, 3.0f, rect.height), color);
    ref_writer.Rect(plRectFloat(rect.x, rect.y + rect.height - 1.0f, rect.width, 1.0f), Line());

    plStringBuilder sUpper(sTitle);
    sUpper.ToUpper();

    const float fTitleWidth = MeasureText(sUpper, MinFontSize + 1) + 2.0f;
    ref_writer.Text(plRectFloat(rect.x + 12.0f, rect.y, plMath::Min(fTitleWidth, rect.width - 16.0f), rect.height), sUpper, Muted(), MinFontSize + 1);

    if (!sCount.IsEmpty())
      ref_writer.Text(plRectFloat(rect.x + 12.0f + fTitleWidth + 8.0f, rect.y, 80.0f, rect.height), sCount, Dim(), MinFontSize, plUiTextHAlign::Left, plUiTextVAlign::Center, true);
  }

  void Dropdown(plUiDrawListWriter& ref_writer, const plRectFloat& rect, plStringView sLead, plStringView sValue, bool bEnabled, bool bRightAligned)
  {
    using namespace plEditorUi;

    ref_writer.Rect(rect, Field(), 4.0f);
    ref_writer.RectOutline(rect, Line(), 1.0f, 4.0f);

    float x = rect.x + 8.0f;
    if (!sLead.IsEmpty())
    {
      const float fLeadWidth = MeasureText(sLead, FontSize - 1) + 6.0f;
      ref_writer.Text(plRectFloat(x, rect.y, fLeadWidth, rect.height), sLead, Dim(), FontSize - 1);
      x += fLeadWidth;
    }

    const float fChevron = 16.0f;
    const plRectFloat valueRect(x, rect.y, plMath::Max(rect.x + rect.width - fChevron - 4.0f - x, 4.0f), rect.height);
    ref_writer.Text(valueRect, sValue, bEnabled ? Foreground() : Dim(), FontSize - 1, bRightAligned ? plUiTextHAlign::Right : plUiTextHAlign::Left);

    // chevron
    const float cx = rect.x + rect.width - 12.0f;
    const float cy = rect.y + rect.height * 0.5f;
    const plVec2 chevron[] = {plVec2(cx - 3.5f, cy - 1.5f), plVec2(cx, cy + 2.0f), plVec2(cx + 3.5f, cy - 1.5f)};
    ref_writer.Polyline(chevron, bEnabled ? Muted() : Dim(), 1.25f);
  }

  void Slider(plUiDrawListWriter& ref_writer, const plRectFloat& rect, float fFraction, bool bEnabled, bool bActive)
  {
    using namespace plEditorUi;

    const float fT = plMath::Clamp(fFraction, 0.0f, 1.0f);
    const float cy = rect.y + rect.height * 0.5f;
    const float x0 = rect.x + 6.0f;
    const float x1 = rect.x + rect.width - 6.0f;
    const float fKnobX = x0 + (x1 - x0) * fT;

    // a raised groove and a muted knob that turns accent while dragged
    ref_writer.Rect(plRectFloat(x0, cy - 2.0f, x1 - x0, 4.0f), Raised(), 2.0f);
    ref_writer.Circle(plVec2(fKnobX, cy), 5.0f, bEnabled ? (bActive ? Accent() : Muted()) : Dim());
  }

  float SliderFractionAt(const plRectFloat& rect, float x)
  {
    const float x0 = rect.x + 6.0f;
    const float x1 = rect.x + rect.width - 6.0f;
    return plMath::Clamp((x - x0) / plMath::Max(x1 - x0, 1.0f), 0.0f, 1.0f);
  }

  float ChipWidth(plStringView sText)
  {
    return s_fChipPadding * 2.0f + s_fChipDot + 6.0f + plEditorUi::MeasureText(sText, s_uiChipFontSize);
  }

  void Chip(plUiDrawListWriter& ref_writer, const plRectFloat& rect, plStringView sText, const plColorGammaUB& color, bool bOn)
  {
    using namespace plEditorUi;

    const float fRadius = rect.height * 0.5f;
    ref_writer.Rect(rect, bOn ? Raised() : Field(), fRadius);
    ref_writer.RectOutline(rect, bOn ? Border() : Line(), 1.0f, fRadius);

    const plVec2 dot(rect.x + s_fChipPadding + s_fChipDot * 0.5f, rect.y + rect.height * 0.5f);
    if (bOn)
      ref_writer.Circle(dot, s_fChipDot * 0.5f, color);
    else
      ref_writer.Circle(dot, s_fChipDot * 0.5f, plColorGammaUB(0, 0, 0, 0), color, 1.5f);

    const float x = rect.x + s_fChipPadding + s_fChipDot + 6.0f;
    ref_writer.Text(plRectFloat(x, rect.y, rect.x + rect.width - x, rect.height), sText, bOn ? Foreground() : Dim(), s_uiChipFontSize);
  }

  float TextButtonWidth(plStringView sText, bool bIcon)
  {
    return plMath::Ceil(plEditorUi::MeasureText(sText, plEditorUi::FontSize - 1)) + 20.0f + (bIcon ? 20.0f : 0.0f);
  }

  void TextButton(plUiDrawListWriter& ref_writer, const plRectFloat& rect, plStringView sText, plStringView sIconPath, bool bEnabled, bool bDashed)
  {
    using namespace plEditorUi;

    if (bDashed)
    {
      DashedRect(ref_writer, rect, Border());
    }
    else
    {
      ref_writer.Rect(rect, bEnabled ? Plate() : Field(), 4.0f);
      ref_writer.RectOutline(rect, Line(), 1.0f, 4.0f);
    }

    float x = rect.x + 10.0f;
    if (!sIconPath.IsEmpty())
    {
      Icon(ref_writer, plRectFloat(x, rect.y + (rect.height - 14.0f) * 0.5f, 14.0f, 14.0f), sIconPath, bEnabled ? Muted() : Dim());
      x += 20.0f;
    }

    ref_writer.Text(plRectFloat(x, rect.y, rect.x + rect.width - x - 6.0f, rect.height), sText, bEnabled ? (bDashed ? Muted() : Foreground()) : Dim(), FontSize - 1);
  }

  void DashedRect(plUiDrawListWriter& ref_writer, const plRectFloat& rect, const plColorGammaUB& color, float fDash, float fGap)
  {
    const float x0 = rect.x + 0.5f;
    const float y0 = rect.y + 0.5f;
    const float x1 = rect.x + rect.width - 0.5f;
    const float y1 = rect.y + rect.height - 0.5f;
    const plVec2 points[] = {plVec2(x0, y0), plVec2(x1, y0), plVec2(x1, y1), plVec2(x0, y1), plVec2(x0, y0)};
    ref_writer.Polyline(points, color, 1.0f, fDash, fGap);
  }

  void DotText(plUiDrawListWriter& ref_writer, const plRectFloat& rect, plStringView sText, const plColorGammaUB& dotColor, const plColorGammaUB& textColor, plUInt32 uiFontSize)
  {
    ref_writer.Circle(plVec2(rect.x + 4.0f, rect.y + rect.height * 0.5f), 3.5f, dotColor);
    ref_writer.Text(plRectFloat(rect.x + 13.0f, rect.y, plMath::Max(rect.width - 13.0f, 1.0f), rect.height), sText, textColor, uiFontSize);
  }

  void ValueBar(plUiDrawListWriter& ref_writer, const plRectFloat& rect, float fFraction, const plColorGammaUB& color)
  {
    ref_writer.Rect(rect, plEditorUi::Field(), rect.height * 0.5f);
    ref_writer.RectOutline(rect, plEditorUi::Line(), 1.0f, rect.height * 0.5f);

    const float fWidth = rect.width * plMath::Clamp(fFraction, 0.0f, 1.0f);
    if (fWidth >= 1.0f)
      ref_writer.Rect(plRectFloat(rect.x, rect.y, fWidth, rect.height), color, rect.height * 0.5f);
  }

  void FormatBytes(plUInt64 uiBytes, plStringBuilder& out_sText)
  {
    if (uiBytes >= 1024 * 1024)
      out_sText.SetFormat("{} MB", plArgF(uiBytes / (1024.0 * 1024.0), 2));
    else if (uiBytes >= 1024)
      out_sText.SetFormat("{} KB", plArgF(uiBytes / 1024.0, 1));
    else
      out_sText.SetFormat("{} B", uiBytes);
  }

  void FormatDbfs(float fLinear, plStringBuilder& out_sText)
  {
    if (fLinear < 1e-6f)
      out_sText = "-\xE2\x88\x9E";
    else
      out_sText.SetFormat("{}", plArgF(20.0f * plMath::Log10(fLinear), 1));
  }
} // namespace plAudioEngineUi
