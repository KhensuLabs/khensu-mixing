// desktop-app/Source/ui/Copilot/HarmonicContentBarChart.h
// VIS-1: a ChartKit bar chart - the energy a saturation stage added per
// octave band, on a relative dB axis; added energy in the diverging palette's
// warm pole, removed in its cool pole, every bar labelled +/- dB (VIS-4).
#pragma once
#include <JuceHeader.h>
#include <vector>
#include "../shared/ChartKitDesktop.h"

namespace khensu {

/// Bar chart of energy a saturation stage (TapeSaturation) added per octave
/// band, wet vs dry, fed from analyze_saturation_aliasing()'s new
/// "band_added_db"/"octave_bands_hz" fields. Program material has no single
/// fundamental to key true 2nd/3rd/... harmonic-order numbers off of, so
/// this shows where in the spectrum the saturation stage actually added
/// energy -- the closest honest visualization of "what tape/tube warmth is
/// adding" without a test-tone reference.
///
/// THREAD_CONTEXT(UIThread)
class HarmonicContentBarChart final : public ThemedBarChart {
  public:
    HarmonicContentBarChart();

    void setReport(const juce::String& effectName, std::vector<float> bandsHz, std::vector<float> addedDb);
    void clear();

    void paint(juce::Graphics& g) override;

  private:
    juce::String effectName_;
    std::vector<float> bandsHz_;
    std::vector<float> addedDb_;

    static constexpr float kRangeDb = 12.0f; // +/- range the bars are scaled against

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HarmonicContentBarChart)
};

} // namespace khensu
