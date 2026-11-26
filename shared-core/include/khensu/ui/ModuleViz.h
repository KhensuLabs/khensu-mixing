// shared-core/include/khensu/ui/ModuleViz.h
// VIZ-6: what each signal-chain module does, drawn beside its knobs - one
// set of drawings for the desktop's Mix / Master rack and the plugin's DSP
// tab (so both read the same):
//   EQ / filters - the module's own magnitude response (the RBJ biquads the
//                  render applies, EqCurveComponent::bandMagnitudeDb);
//   compressor   - the static transfer curve (input -> output level) with the
//                  threshold, ratio and knee, the current input level as a
//                  dot on it, and a gain-reduction meter;
//   limiter      - gain-reduction history (newest on the right) under the
//                  ceiling.
// Plain paint functions on a Style (colours), no state: the owner keeps the
// live values (the plugin's meters) or estimates (the desktop, from the
// analysed track) and calls these from paint().
// THREAD_CONTEXT(UIThread)
#pragma once
#include <juce_graphics/juce_graphics.h>
#include <optional>
#include <vector>
#include <khensu/ui/EqCurveComponent.h>

namespace khensu::ui::module_viz {

struct Style {
    juce::Colour background, grid, curve, accent, text, warn;
    static Style fromPalette(); ///< the current ui::Palette theme (STU-6)
};

/// The summed magnitude response of `bands` from 20 Hz to 20 kHz, with a 0 dB
/// line; `rangeDb` is the +/- span drawn.
void paintEqResponse(juce::Graphics& g, juce::Rectangle<float> r, const std::vector<EqBand>& bands,
                     double sampleRate, const Style& s, float rangeDb = 18.0f);

struct CompressorState {
    float thresholdDb = -18.0f;
    float ratio = 4.0f;
    float kneeDb = 6.0f;
    std::optional<float> inputDb;       ///< the current (or typical) input level
    std::optional<float> reductionDb;   ///< gain reduction now (>= 0)
};

/// Output level for an input level through a soft-knee compressor (dB).
float compressorOutputDb(float inputDb, float thresholdDb, float ratio, float kneeDb) noexcept;

/// The transfer curve (-60..0 dB both axes) and, at the right, a GR meter.
void paintCompressor(juce::Graphics& g, juce::Rectangle<float> r, const CompressorState& c, const Style& s);

/// Gain-reduction history: `historyDb` oldest .. newest (>= 0 dB of reduction),
/// drawn downward from the top line, scaled to `maxDb`.
void paintReductionHistory(juce::Graphics& g, juce::Rectangle<float> r, const std::vector<float>& historyDb,
                           const Style& s, float maxDb = 6.0f);

/// A vertical gain-reduction bar (0 at the top, maxDb at the bottom).
void paintReductionMeter(juce::Graphics& g, juce::Rectangle<float> r, float reductionDb, const Style& s,
                         float maxDb = 12.0f);

} // namespace khensu::ui::module_viz
