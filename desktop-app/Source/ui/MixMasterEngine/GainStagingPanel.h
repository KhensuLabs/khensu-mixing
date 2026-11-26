// desktop-app/Source/ui/MixMasterEngine/GainStagingPanel.h
// Gain staging (UX2-2): integrated loudness, loudness range, crest factor,
// dynamic range and true peak as tiles - the measured number largest, in
// tabular figures, over a neutral bar with the target band and a marker.
// Colour only when a value is out of its target (amber near, red far): the
// severity colours mean severity. Then what streaming playback does to the
// level and peak, the dynamics verdict, the spectral balance and the width.
#pragma once
#include <JuceHeader.h>
#include "../shared/PanelCard.h"
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/ui/ChartKit.h>
#include <array>

namespace khensu {

namespace gain_staging {

enum class Fit { InTarget, Near, Far };

/// Where `value` sits against [lo, hi]: inside, out by up to half the band's
/// width (Near), or further (Far).
inline Fit fit(float value, float lo, float hi) noexcept
{
    if (value >= lo && value <= hi) return Fit::InTarget;
    const float band = juce::jmax(0.5f, hi - lo);
    const float out = value < lo ? lo - value : value - hi;
    return out <= band * 0.5f ? Fit::Near : Fit::Far;
}

/// x of `value` along a bar spanning [min, max], clamped.
inline float proportion(float value, float min, float max) noexcept
{
    return max > min ? juce::jlimit(0.0f, 1.0f, (value - min) / (max - min)) : 0.0f;
}

} // namespace gain_staging

// THREAD_CONTEXT(UIThread)
class GainStagingPanel final : public PanelCard {
  public:
    explicit GainStagingPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    /// VIS-8: what a screen reader reads for the gauges.
    juce::String summary() const;

    struct Gauge {
        juce::String label;
        juce::String unit;
        float value = 0.0f;
        float min = 0.0f, max = 1.0f; ///< the bar's span
        float lo = 0.0f, hi = 1.0f;   ///< the target band
        bool known = false;
    };
    /// Tests: the five gauges (LUFS, LRA, crest, DR, true peak).
    const std::array<Gauge, 5>& gauges() const noexcept { return gauges_; }

  private:
    void paintGauge(juce::Graphics& g, const Gauge& gauge, juce::Rectangle<int> area) const;
    void paintBody(juce::Graphics& g);
    int gaugeAt(juce::Point<int> pos) const;

    std::array<Gauge, 5> gauges_;
    juce::String playbackLine_;
    gain_staging::Fit playbackFit_ = gain_staging::Fit::InTarget;
    juce::String dynamicsLine_;
    gain_staging::Fit dynamicsFit_ = gain_staging::Fit::InTarget;
    bool hasData_ = false;

    // stored for paint()
    float balanceLow_ = 0.0f;
    float balanceMid_ = 0.0f;
    float balanceHigh_ = 0.0f;
    float stereoWidth_ = 0.0f;
    float stereoCorr_ = 0.0f;
    std::array<juce::Rectangle<int>, 5> gaugeAreas_;
    juce::Rectangle<int> statusArea_;
    juce::Rectangle<int> specBalanceArea_;
    juce::Rectangle<int> stereoBarArea_;
    int hoveredGauge_ = -1;
    juce::Point<float> hoverPos_;
    ui::chart::ChartSummary gaugesSummary_; ///< VIS-8

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GainStagingPanel)
};

} // namespace khensu
