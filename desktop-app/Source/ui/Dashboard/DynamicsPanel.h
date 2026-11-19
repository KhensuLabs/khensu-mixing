// desktop-app/Source/ui/Dashboard/DynamicsPanel.h
#pragma once
#include "../shared/PanelCard.h"
#include "../shared/MeterBarComponent.h"
#include "../shared/CompareDeltaChip.h"
#include <khensu/analysis/AnalysisResult.h>
#include <vector>
#include <array>

namespace khensu {

// Pseudo-3D heatmap strip showing per-section crest factor (a proxy for how
// heavily each section has been compressed/limited - low crest = squashed,
// high crest = dynamic) across the track timeline.
class SectionDynamicsHeatStrip : public juce::Component {
  public:
    SectionDynamicsHeatStrip() = default;
    void setSections(const std::vector<float>& crestDbValues)
    {
        values_ = crestDbValues;
        repaint();
    }
    void paint(juce::Graphics& g) override;

  private:
    std::vector<float> values_;
    JUCE_DECLARE_NON_COPYABLE(SectionDynamicsHeatStrip)
};

// Distribution histogram of per-section crest factor - complements the
// single-number crest/DR tiles by showing whether the whole track sits at
// one crest value or swings between "consistently squashed" and "one hot
// dynamic chorus" (a distribution single numbers can't distinguish).
class CrestHistogramStrip : public juce::Component {
  public:
    CrestHistogramStrip() = default;
    void setSections(const std::vector<float>& crestDbValues);
    void paint(juce::Graphics& g) override;

  private:
    static constexpr int kNumBins = 8;
    static constexpr float kMinDb = 0.0f, kMaxDb = 24.0f;
    std::array<int, kNumBins> counts_{};
    int maxCount_ = 0;
    bool hasData_ = false;
    JUCE_DECLARE_NON_COPYABLE(CrestHistogramStrip)
};

class DynamicsPanel final : public PanelCard {
  public:
    explicit DynamicsPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;
    void setCompareModeEnabled(bool enabled);

  private:
    MeterBarComponent drMeter_, crestMeter_;
    MeterBarComponent kickRegMeter_, snareRegMeter_;
    juce::Label peakToRms_;
    SectionDynamicsHeatStrip dynamicsHeatStrip_;
    CrestHistogramStrip crestHistogram_;

    CompareDeltaChip compareChip_;
    bool compareEnabled_ = false;
    std::optional<ReferenceDeltas> lastRefDeltas_;
    void updateCompareChip();
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DynamicsPanel)
};

} // namespace khensu
