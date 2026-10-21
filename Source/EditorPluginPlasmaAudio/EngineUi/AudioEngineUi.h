#pragma once

#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioDLL.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Math/Color8UNorm.h>
#include <Foundation/Math/Rect.h>
#include <GameUiPlugin/Elements/UiDrawList.h>

/// \brief Painting helpers shared by the engine-drawn audio editors (clip, event, mixer, profiler).
///
/// The views paint into plUiDrawListWriter drawings and put transparent hit buttons over the clickable parts, so
/// hover, press, tooltips and click events come from the retained UI.
namespace plAudioEngineUi
{
  /// Package artwork for the engine views (the Qt resource prefix "PlasmaAudioIcons"; ":/AssetIcons/" resolves to
  /// the SDK's own artwork). The asset icons are coloured: draw them with bMonochrome.
  inline constexpr const char* IconClip = ":/PlasmaAudioIcons/AudioClip.svg";
  inline constexpr const char* IconEvent = ":/PlasmaAudioIcons/AudioEvent.svg";
  inline constexpr const char* IconBank = ":/PlasmaAudioIcons/AudioBank.svg";
  inline constexpr const char* IconMixerAsset = ":/PlasmaAudioIcons/AudioMixer.svg";
  inline constexpr const char* IconMixer = ":/PlasmaAudioIcons/Mixer.svg";
  inline constexpr const char* IconSliders = ":/PlasmaAudioIcons/Sliders.svg";

  /// The Audio category colour (bars, curves, section edges).
  plColorGammaUB AudioColor();

  /// A category colour by its name ("Scripting", "Utilities", ...). Only pass names plColorScheme knows.
  plColorGammaUB CategoryColor(plStringView sCategory);

  /// The colour with another alpha.
  plColorGammaUB WithAlpha(const plColorGammaUB& color, plUInt8 uiAlpha);

  /// An opaque blend of two colours, fT = 0 gives a, 1 gives b.
  plColorGammaUB Mix(const plColorGammaUB& a, const plColorGammaUB& b, float fT);

  /// Places an element at a rect measured from its parent's top left.
  void Place(plUiElement* pElement, const plRectFloat& rect);

  /// A transparent button over painted content: it takes clicks (raising sAction), shows a faint hover tint and
  /// carries the tooltip.
  plUiButton* HitButton(plUiElement* pParent, const plRectFloat& rect, plStringView sAction, plStringView sToolTip = {}, plBlackboard* pBoard = nullptr);

  /// Draws an SVG icon (white artwork) tinted into rect. bMonochrome first turns coloured artwork neutral.
  void Icon(plUiDrawListWriter& ref_writer, const plRectFloat& rect, plStringView sSvgPath, const plColorGammaUB& tint, bool bMonochrome = false);

  /// Height of a painted section header.
  inline constexpr float SectionHeight = 26.0f;

  /// A section header: a strip on the raised surface with a 3 px edge in color, the uppercase
  /// title and an optional count in the code face.
  void SectionHeader(plUiDrawListWriter& ref_writer, const plRectFloat& rect, plStringView sTitle, const plColorGammaUB& color, plStringView sCount = {});

  /// A dropdown field: a well with a hairline border, an optional dim lead word, the value and a chevron.
  void Dropdown(plUiDrawListWriter& ref_writer, const plRectFloat& rect, plStringView sLead, plStringView sValue, bool bEnabled = true, bool bRightAligned = false);

  /// A horizontal slider: a raised groove and a muted knob at fFraction, accent while bActive (dragged).
  void Slider(plUiDrawListWriter& ref_writer, const plRectFloat& rect, float fFraction, bool bEnabled = true, bool bActive = false);

  /// The slider value (0..1) at an x inside a slider's rect.
  float SliderFractionAt(const plRectFloat& rect, float x);

  /// Width of a state chip with this text.
  float ChipWidth(plStringView sText);

  /// A state chip: a pill with a dot in color, filled while on, outlined while off.
  void Chip(plUiDrawListWriter& ref_writer, const plRectFloat& rect, plStringView sText, const plColorGammaUB& color, bool bOn);

  /// A painted text button (Plate with a 1 px Line border), optional leading icon, dashed for "add" slots.
  void TextButton(plUiDrawListWriter& ref_writer, const plRectFloat& rect, plStringView sText, plStringView sIconPath = {}, bool bEnabled = true, bool bDashed = false);

  /// Width a painted text button needs for its text and icon.
  float TextButtonWidth(plStringView sText, bool bIcon);

  /// A rectangle outline drawn with dashes.
  void DashedRect(plUiDrawListWriter& ref_writer, const plRectFloat& rect, const plColorGammaUB& color, float fDash = 3.0f, float fGap = 3.0f);

  /// A text with a coloured dot in front (system colours as names with a dot, style pass D7).
  void DotText(plUiDrawListWriter& ref_writer, const plRectFloat& rect, plStringView sText, const plColorGammaUB& dotColor, const plColorGammaUB& textColor, plUInt32 uiFontSize = plEditorUi::FontSize - 1);

  /// A thin bar in a field well, filled to fFraction (audibility, memory share).
  void ValueBar(plUiDrawListWriter& ref_writer, const plRectFloat& rect, float fFraction, const plColorGammaUB& color);

  /// "512 B", "64.0 KB", "1.20 MB".
  void FormatBytes(plUInt64 uiBytes, plStringBuilder& out_sText);

  /// Linear amplitude to dBFS text: "-12.0" or "-∞".
  void FormatDbfs(float fLinear, plStringBuilder& out_sText);
} // namespace plAudioEngineUi
