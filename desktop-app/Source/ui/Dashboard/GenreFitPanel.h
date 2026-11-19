// desktop-app/Source/ui/Dashboard/GenreFitPanel.h
// NEW-3: where the mix sits in its genre, against what released tracks in that
// genre measure (resources/genre_norms/genre_corpus_stats.json, MEAS-1): for
// each graded metric the middle 80% of the genre's tracks (10th-90th
// percentile) as a band, the median as a tick, the mix as a marker, and how
// many tracks the distribution rests on. A genre with no measured tracks says
// so instead of drawing a typed target as if it were measured.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/PanelCard.h"
#include <optional>
#include <vector>

namespace khensu {

class GenreFitPanel final : public PanelCard {
  public:
    explicit GenreFitPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void paint(juce::Graphics& g) override;

    struct Row {
        juce::String label, unit;
        double p10 = 0, median = 0, p90 = 0;
        std::optional<double> value; ///< the mix; empty when not measured
    };
    /// Rows for `genreId` from a genre_corpus_stats.json document and the
    /// mix's metrics (tests drive this directly). `tracks` receives n.
    static std::vector<Row> rowsFor(const juce::var& stats, const juce::String& genreId, const AnalysisResult& r,
                                    int& tracks);
    /// 0..1 position of `v` on a scale that holds the band and the value.
    static double position(double v, double lo, double hi) noexcept;

  private:
    juce::var stats_;
    juce::String genreId_;
    std::vector<Row> rows_;
    int tracks_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GenreFitPanel)
};

} // namespace khensu
