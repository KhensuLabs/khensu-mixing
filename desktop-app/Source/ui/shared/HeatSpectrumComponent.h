// desktop-app/Source/ui/shared/HeatSpectrumComponent.h
#pragma once
#include <JuceHeader.h>
#include <vector>

namespace khensu {

struct SpectrumPin {
    float hz = 0.0f;
    float prominence = 0.0f;
};

class HeatSpectrumComponent final : public juce::Component {
  public:
    HeatSpectrumComponent() = default;

    // API matches SpectrumComponent
    void setBands(const std::vector<float>& dbPerOctave);
    void setResonancePins(std::vector<SpectrumPin> pins);
    void setGenreReference(float centroid_hz, float low_end_weight);

    /// Genre spectral-tilt target - draws a dashed reference slope across the
    /// octave bands, anchored to the measured bands' mean energy so the line
    /// shows *shape* (dB/octave), not absolute level. show=false hides it.
    void setTargetTilt(float dbPerOctave, bool show);
    void setMidSideBands(const std::vector<float>& midBands, const std::vector<float>& sideBands);

    /// Sample rate used to map FFT bin index -> Hz when the supplied bands are
    /// raw FFT magnitudes rather than octave bands.
    ///
    /// This was hardcoded to 44100, which mis-places every bin on a 48k source
    /// (the analyzers' own default) - a ~9% frequency error across the whole
    /// plot (kimi audit P1 #36 / UI-13). The default stays 44100 so a caller
    /// that doesn't yet have a measured rate renders unchanged;
    /// `AnalysisResult::sample_rate` is the real value callers should pass.
    void setSampleRate(double sampleRate);

    /// EQ suggestion overlay -- bell curves colour-coded by severity.
    struct EqPoint {
        float freq_hz = 0.0f;
        float gain_db = 0.0f; ///< positive = boost, negative = cut
        float q = 0.707f;
        float severity = 0.0f; ///< 0-100; used to pick colour
    };
    void setEqSuggestions(std::vector<EqPoint> pts); // pass {} to clear

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

    // Public for testing
    static juce::Colour colourForRank(float rank); // rank 0..1
    static std::vector<float> computeRanks(const std::vector<float>& bands);

  private:
    std::vector<float> bands_;
    std::vector<float> ranks_;
    std::vector<float> midBands_;
    std::vector<float> sideBands_;
    std::vector<SpectrumPin> pins_;
    std::vector<EqPoint> eqPoints_;
    float genreCentroid_ = 0.0f;
    float genreLowEnd_ = 0.0f;
    float targetTiltDbPerOct_ = 0.0f;
    double sampleRate_ = 44100.0;
    bool showTargetTilt_ = false;
    int hoveredPin_ = -1;
    int hoveredBand_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeatSpectrumComponent)
};

} // namespace khensu
