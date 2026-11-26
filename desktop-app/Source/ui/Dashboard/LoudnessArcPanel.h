// desktop-app/Source/ui/Dashboard/LoudnessArcPanel.h
// Section-level loudness arc bar chart panel. VIS-1: a ChartKit bar chart
// (one bar per section on the LUFS axis, the genre's range shaded, its
// target marked); clicking a section plays from its start.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include "../shared/ChartKitDesktop.h"
#include "../shared/PanelCard.h"
#include "SectionContrast.h"
#include <optional>
#include <vector>

namespace khensu {

class IAppController;

/// Displays per-section integrated LUFS as a horizontal bar chart (loudness arc),
/// revealing chorus lift, verse headroom, and structural energy flow.
/// Populated from AnalysisResult::section_analysis after full analysis completes.
class LoudnessArcPanel : public PanelCard {
  public:
    explicit LoudnessArcPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Tests: the chart.
    const ui::chart::BarChart& chart() const noexcept { return chart_; }

  private:
    struct SectionBar {
        juce::String label;
        float lufs = 0.0f;
        float startSec = 0.0f;
        bool valid = false;
    };
    std::vector<SectionBar> bars_;

    /// The bars with the genre's LUFS range shaded and its target marked.
    struct Chart final : ThemedBarChart {
        std::optional<std::pair<double, double>> range;
        std::optional<double> target;
        void paintData(juce::Graphics& g, juce::Rectangle<float> plot) override;
    };

    // --- Macrodynamics contrast (Katz: macrodynamics is what over-compression
    // destroys). Measured verse->chorus LUFS lift vs. the genre's expected
    // contrast (GenreEntry::chorus_contrast_norm mapped to dB) - drawn as a
    // footer readout under the bars.
    void computeContrast();
    void rebuildChart();
    float measuredContrastDb_ = 0.0f;
    float expectedContrastDb_ = 0.0f;
    bool contrastValid_ = false;
    section_contrast::Result contrast_; ///< how the lift was measured (role names, repeats, or approx)

    Chart chart_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoudnessArcPanel)
};

} // namespace khensu
