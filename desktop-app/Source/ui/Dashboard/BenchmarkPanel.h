// desktop-app/Source/ui/Dashboard/BenchmarkPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/analysis/AnalysisResult.h>
#include "../shared/PanelCard.h"

namespace khensu {

class IAppController;

/// Percentile rank against a per-genre reference-measurement library (see
/// AnalysisResult::BenchmarkData, benchmark.py) - "where does this track
/// rank" rather than every other panel's "how far from the single target
/// number." Absent (shows an explanatory empty state) when the detected
/// genre has no library entry, which today means most tracks - see
/// BenchmarkData::is_bootstrap_data and the panel's explainer text for why.
class BenchmarkPanel final : public PanelCard {
  public:
    explicit BenchmarkPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    std::optional<AnalysisResult::BenchmarkData> benchmark_;
    juce::Label summaryLabel_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BenchmarkPanel)
};

} // namespace khensu
