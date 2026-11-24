// desktop-app/Source/ui/shared/FixDifferenceView.h
// VIZ-3a: what a fix actually changed, MEASURED from the two files (not the
// expected curve the hero draws):
//   - the difference spectrum: the current version's long-term band power
//     minus the original's, per log band - red where it added energy, blue
//     where it took some away (the diverging palette), zero line neutral;
//   - the difference loudness curve: short-term loudness of the current
//     version minus the original's, over time.
// Both are chart-kit charts (hover readout, wheel zoom on time).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <memory>
#include <vector>
#include <khensu/analysis/TrackTimeline.h>
#include "ChartStyle.h"

namespace khensu {

namespace fix_difference {

struct Result {
    std::vector<double> hz, spectrumDb; ///< current - original, per band
    std::vector<double> timeSec, loudnessDb; ///< current - original short-term, per hop (NaN in silence)
    bool empty() const noexcept { return hz.empty(); }
};

/// The long-term band power of a timeline's spectrogram (dB, power mean).
std::vector<double> longTermBandDb(const TrackTimeline& tl);

/// The measured difference of `current` against `original` (pure; tested).
Result compute(const TrackTimeline& original, const TrackTimeline& current);

} // namespace fix_difference

class FixDifferenceView final : public juce::Component {
  public:
    FixDifferenceView();
    void setDifference(fix_difference::Result r, const juce::String& caption);
    void clear();
    bool hasData() const noexcept { return !result_.empty(); }
    void paint(juce::Graphics& g) override;
    void resized() override;

    struct SpectrumChart final : ThemedChart {
        const fix_difference::Result* r = nullptr;
        void paintData(juce::Graphics& g, juce::Rectangle<float> plot) override;
        ui::chart::Readout readoutAt(double x, double y) override;
    };
    struct LoudnessChart final : ThemedChart {
        const fix_difference::Result* r = nullptr;
        void paintData(juce::Graphics& g, juce::Rectangle<float> plot) override;
        ui::chart::Readout readoutAt(double x, double y) override;
    };
    SpectrumChart& spectrum() noexcept { return spectrum_; }
    LoudnessChart& loudness() noexcept { return loudness_; }

  private:
    fix_difference::Result result_;
    juce::String caption_;
    SpectrumChart spectrum_;
    LoudnessChart loudness_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FixDifferenceView)
};

} // namespace khensu
