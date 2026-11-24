#pragma once
// UI-5: the studio-grade design tokens shared by the plugin and the desktop
// widgets in khensu/ui (meters, KhensuKnob). Near-monochrome surfaces, ONE
// accent (Palette::accentTeal) and the severity colours only for severity.
// Type scale: 28 display/metric, 17 title, 13 body, 11 label - and every
// number in tabular figures (numberFont) so readouts do not jitter.
// THREAD_CONTEXT(UIThread)

#include <juce_graphics/juce_graphics.h>
#include <khensu/ui/KhensuPalette.h>

namespace khensu::ui::studio {

inline constexpr float kDisplaySize = 28.0f; ///< measured values, the largest text
inline constexpr float kTitleSize = 17.0f;
inline constexpr float kBodySize = 13.0f;
inline constexpr float kLabelSize = 11.0f;

inline constexpr int kPad = 12;    ///< outer padding of a card / strip
inline constexpr int kGap = 8;     ///< gap between controls
inline constexpr float kRadius = 6.0f;

/// Palette::uiFont with OpenType tabular figures ("tnum"), for every number.
inline juce::Font numberFont(float size, int style = juce::Font::plain)
{
    auto font = Palette::uiFont(size, style);
    font.setFeatureSetting({juce::FontFeatureTag{"tnum"}, juce::FontFeatureSetting::featureEnabled});
    return font;
}

/// Small upper-case labels ("INTEGRATED", "dBTP").
inline juce::Font labelFont(float size = kLabelSize) { return Palette::uiFont(size, juce::Font::plain); }

// Derived surface tones. Functions, not statics: Palette's colours live in
// another translation unit (static initialisation order).
inline juce::Colour surface() { return Palette::bgPanel; }
inline juce::Colour surfaceRaised() { return Palette::bgPanel.brighter(0.12f); }
inline juce::Colour track() { return Palette::bgPanel.brighter(0.35f); }
inline juce::Colour hairline() { return Palette::textPrimary.withAlpha(0.10f); }
inline juce::Colour neutralFill() { return Palette::textPrimary.withAlpha(0.55f); }

/// A dB-like value as text with `decimals` places, "-inf" below `floor`.
inline juce::String formatDb(float value, int decimals = 1, float floor = -99.0f)
{
    if (!(value > floor)) return juce::CharPointer_UTF8("-\xe2\x88\x9e"); // utf8-exempt(minus infinity glyph)
    return juce::String(value, decimals);
}

} // namespace khensu::ui::studio
