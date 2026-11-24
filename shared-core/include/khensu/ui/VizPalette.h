// shared-core/include/khensu/ui/VizPalette.h
// VIZ-F2: the data-visualisation palette - one source for the desktop app
// (KhensuTheme's viz* tokens are assigned from here in setTheme) and the
// plugin (which reads it directly). Colour does one job per chart:
//   categorical - identity (stems, versions, tracks): fixed slot order, never
//                 cycled; a ninth series folds into "Other";
//   sequential  - magnitude (heat maps): one hue, light -> dark;
//   diverging   - polarity (a difference spectrum, "above / below the genre"):
//                 blue <-> red through a neutral grey, never a hue at zero.
// Severity keeps its own colours (red / amber / blue / green, KhensuTheme),
// which are never reused for a series.
// The categorical order is the one that clears every adjacent-pair colour-
// blindness gate (CVD delta-E >= 8, normal-vision >= 15, OKLab x100) on each
// theme's surface; tests/ui/VizPaletteTest.cpp re-checks it (protan, deutan,
// tritan) so a changed value cannot ship unchecked. On the Light theme three
// slots sit under 3:1 against white, so light charts label their series.
// THREAD_CONTEXT(Any)
#pragma once
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <cmath>
#include <cstdint>

namespace khensu::ui::viz {

enum class Mode { Dark, Light }; ///< High-contrast uses the Dark steps

inline constexpr int kCategoricalCount = 8;

/// Slots: blue, orange, aqua, yellow, magenta, green, violet, red.
inline constexpr std::array<std::uint32_t, kCategoricalCount> kCategoricalDark{
    0xff3987e5, 0xffd95926, 0xff199e70, 0xffc98500, 0xffd55181, 0xff0ca30c, 0xff9085e9, 0xffe66767};
inline constexpr std::array<std::uint32_t, kCategoricalCount> kCategoricalLight{
    0xff2a78d6, 0xffeb6834, 0xff1baf7a, 0xffeda100, 0xffe87ba4, 0xff008300, 0xff6250d6, 0xffe34948};

/// The sequential ramp (blue, steps 100 .. 700): index 0 is near zero.
inline constexpr std::array<std::uint32_t, 13> kSequential{
    0xffcde2fb, 0xffb7d3f6, 0xff9ec5f4, 0xff86b6ef, 0xff6da7ec, 0xff5598e7, 0xff3987e5,
    0xff2a78d6, 0xff256abf, 0xff1c5cab, 0xff184f95, 0xff104281, 0xff0d366b};

/// Diverging poles and the neutral midpoint per mode.
inline constexpr std::uint32_t kDivergingCool = 0xff2a78d6; ///< below / less (blue)
inline constexpr std::uint32_t kDivergingWarm = 0xffd03b3b; ///< above / more (red)
inline constexpr std::uint32_t kNeutralDark = 0xff383835;
inline constexpr std::uint32_t kNeutralLight = 0xffe0dfdb;

inline juce::Colour categorical(int slot, Mode mode = Mode::Dark)
{
    const auto& p = mode == Mode::Light ? kCategoricalLight : kCategoricalDark;
    const int i = ((slot % kCategoricalCount) + kCategoricalCount) % kCategoricalCount;
    return juce::Colour(p[static_cast<size_t>(i)]);
}

/// Sequential colour for t in [0, 1]. On a dark surface the ramp runs the
/// other way (near zero recedes into the dark surface, a high value is light),
/// which keeps "more = more contrast" on both themes.
inline juce::Colour sequential(float t, Mode mode = Mode::Dark)
{
    t = std::isfinite(t) ? juce::jlimit(0.0f, 1.0f, t) : 0.0f;
    if (mode == Mode::Dark) t = 1.0f - t;
    const float x = t * static_cast<float>(kSequential.size() - 1);
    const auto i = static_cast<size_t>(std::floor(x));
    const auto j = std::min(i + 1, kSequential.size() - 1);
    return juce::Colour(kSequential[i]).interpolatedWith(juce::Colour(kSequential[j]), x - static_cast<float>(i));
}

/// Spectrograms: a perceptually uniform dark-to-bright map (magma), the
/// convention for level over time and frequency - brightness rises evenly with
/// level, it reads in greyscale and for colour-blind viewers, and silence is
/// near-black on both themes.
inline constexpr std::array<std::uint32_t, 9> kSpectrogram{
    0xff000004, 0xff1c1044, 0xff4f127b, 0xff812581, 0xffb5367a, 0xffe55064, 0xfffb8761, 0xfffec287, 0xfffcfdbf};

inline juce::Colour spectrogram(float t)
{
    t = std::isfinite(t) ? juce::jlimit(0.0f, 1.0f, t) : 0.0f;
    const float x = t * static_cast<float>(kSpectrogram.size() - 1);
    const auto i = static_cast<size_t>(std::floor(x));
    const auto j = std::min(i + 1, kSpectrogram.size() - 1);
    return juce::Colour(kSpectrogram[i]).interpolatedWith(juce::Colour(kSpectrogram[j]), x - static_cast<float>(i));
}

/// Diverging colour for t in [-1, 1]: -1 cool, 0 neutral, +1 warm.
inline juce::Colour diverging(float t, Mode mode = Mode::Dark)
{
    t = std::isfinite(t) ? juce::jlimit(-1.0f, 1.0f, t) : 0.0f;
    const juce::Colour mid(mode == Mode::Light ? kNeutralLight : kNeutralDark);
    return t < 0.0f ? mid.interpolatedWith(juce::Colour(kDivergingCool), -t)
                    : mid.interpolatedWith(juce::Colour(kDivergingWarm), t);
}

} // namespace khensu::ui::viz
