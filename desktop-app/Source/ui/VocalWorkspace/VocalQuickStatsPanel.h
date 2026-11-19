#pragma once
#include <JuceHeader.h>
#include "../shared/PanelCard.h"
#include <khensu/analysis/AnalysisResult.h>
#include <array>

namespace khensu {

class VocalQuickStatsPanel final : public PanelCard {
  public:
    explicit VocalQuickStatsPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void paint(juce::Graphics& g) override;
    void resized() override;
    /// False until a real analysis arrives (empty state, never demo values).
    bool hasData() const noexcept { return hasData_; }

  private:
    /// Fetches the /analyze/quick_stats aggregate (spec §2.2) for the active
    /// session file - the format/level numbers the fast analysis doesn't
    /// carry. Fires once per file; self-guards while a fetch is in flight.
    void fetchServiceStats();

    float bpm_ = 0.0f;
    juce::String key_;
    juce::String genre_;
    juce::String bpmDetail_; ///< meter / felt tempo / confidence beside the BPM label
    juce::String keyDetail_; ///< Camelot / confidence beside the KEY label
    float balanceLow_ = 0.0f;
    float balanceMid_ = 0.0f;
    float balanceHigh_ = 0.0f;
    bool hasData_ = false;

    // ── /analyze/quick_stats grid (label + value pairs) ────────────────
    struct Stat {
        juce::String label, value;
    };
    std::array<Stat, 9> stats_{};
    bool hasServiceStats_ = false;
    bool statsFetchInFlight_ = false;
    int clippingCount_ = 0;         ///< red highlight when > 0
    juce::String statsFetchedPath_; ///< once-per-file guard

    juce::Rectangle<int> specArea_;
    juce::Rectangle<int> gridArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocalQuickStatsPanel)
};

} // namespace khensu
