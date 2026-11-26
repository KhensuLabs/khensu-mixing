// desktop-app/Source/ui/shared/VizMaps.h
// VIS-2 / VIS-6: the desktop's colour maps and the pieces every heat map and
// meter draws with them.
//   - One spectrogram map: ui::viz::spectrogram (magma, VizPalette.h) for
//     every level-over-time-and-frequency image (detailed, Live, timeline
//     lane, waveform editor, mid / side).
//   - One diverging map: KhensuTheme::vizDiverging (blue - neutral - orange,
//     VizPalette.h) for every signed quantity (phase correlation, the
//     reference delta, the hero's "above / below released tracks").
//   - A colour bar beside each heat map, so a colour can be read as a value.
//   - The "stopped" look (VIS-6): meters and scopes keep showing the last
//     analysed state, desaturated, with one caption that says so.
// The frequency axis they share is ui::viz::kFreqMinHz..kFreqMaxHz
// (khensu/ui/VizFrequencyAxis.h).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <khensu/ui/VizFrequencyAxis.h>
#include <khensu/ui/VizPalette.h>
#include <array>

namespace khensu::ui::viz {

/// The spectrogram map as 256 precomputed pixels (index = level 0..255).
const std::array<juce::PixelARGB, 256>& spectrogramLut();

/// The diverging map for the current theme, t in [-1, 1] (-1 blue, +1 orange).
juce::Colour themedDiverging(float t);

/// A vertical colour bar for the spectrogram map in `area`: the bar on the
/// left, `top` / `bottom` labels (e.g. "0 dB" / "-84") to its right and an
/// optional caption under them. Brightest at the top.
void paintSpectrogramBar(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& top,
                         const juce::String& bottom, const juce::String& caption = {});

/// A vertical colour bar for the diverging map: warm (`high`) at the top,
/// neutral (`mid`) in the middle, cool (`low`) at the bottom.
void paintDivergingBar(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& high,
                       const juce::String& mid, const juce::String& low);

/// The width a colour bar with labels needs (bar + the widest label).
inline constexpr float kColourBarW = 46.0f;

/// The frequency ticks of the standard axis down the left of `plot`, drawn
/// into the `labelW` pixels left of it, with faint grid lines across `plot`
/// when `grid` is set. `lowHz` / `highHz` limit the ticks to a sub-range.
void paintFrequencyTicks(juce::Graphics& g, juce::Rectangle<float> plot, float labelW, bool grid,
                         float lowHz = kFreqMinHz, float highHz = kFreqMaxHz);

/// y of `hz` on the standard axis spread over `plot` (bottom = 20 Hz).
inline float yForHz(juce::Rectangle<float> plot, float hz)
{
    return plot.getBottom() - freqNorm(hz) * plot.getHeight();
}

/// The frequency at `y` on the standard axis spread over `plot`.
inline float hzForY(juce::Rectangle<float> plot, float y)
{
    if (plot.getHeight() <= 0.0f) return kFreqMinHz;
    return freqAt((plot.getBottom() - y) / plot.getHeight());
}

// --- VIS-6: the stopped look -------------------------------------------------

/// A colour as it is drawn while the transport is stopped: most of its
/// saturation gone and dimmed, so the analysed state reads as "not live".
juce::Colour stoppedTint(juce::Colour c);

/// The caption over a meter or scope showing analysed values while stopped
/// ("Stopped - analysed values"), small, top-right of `area`.
void paintStoppedCaption(juce::Graphics& g, juce::Rectangle<float> area);

/// The caption text (one string for every meter).
juce::String stoppedCaptionText();

} // namespace khensu::ui::viz
