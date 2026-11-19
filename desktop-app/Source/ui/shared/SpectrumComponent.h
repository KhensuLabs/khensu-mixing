// desktop-app/Source/ui/shared/SpectrumComponent.h
#pragma once
#include <JuceHeader.h>
#include <vector>

namespace khensu {

struct SpectrumPin {
    float hz = 0.0f;
    float prominence = 0.0f;
};

// THREAD_CONTEXT(UIThread)
class SpectrumComponent final : public juce::Component {
  public:
    SpectrumComponent() = default;
    void setBands(const std::vector<float>& dbPerOctave);
    void setResonancePins(std::vector<SpectrumPin> pins);            // pass {} to clear
    void setGenreReference(float centroid_hz, float low_end_weight); // 0,0 = no reference

    /// EQ suggestion overlay - drawn on top of spectrum bars as parametric bell curves.
    /// Each point maps to one spectral recommendation that has eq_freq_hz + eq_gain_db params.
    struct EqPoint {
        float freq_hz = 0.0f;
        float gain_db = 0.0f; ///< positive = boost, negative = cut
        float q = 0.707f;
        float severity = 0.0f; ///< 0-100; used to pick colour
    };
    void setEqSuggestions(std::vector<EqPoint> pts); // pass {} to clear

    void paint(juce::Graphics& g) override;

  private:
    std::vector<float> bands_;
    std::vector<SpectrumPin> pins_;
    std::vector<EqPoint> eqPoints_;
    float genreCentroid_ = 0.0f;
    float genreLowEnd_ = 0.0f;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumComponent)
};

} // namespace khensu
