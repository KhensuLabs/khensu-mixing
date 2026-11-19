// desktop-app/Source/ui/Dashboard/GenreDistributionPanel.h
// Loudness/dynamics vs genre distribution: box plots of the genre reference
// library's LUFS / LRA / crest with the track's value marked and its percentile
// position called out. Honest about provenance — labels bootstrap/synthetic
// reference data so a directional percentile isn't read as a chart claim.
// THREAD_CONTEXT(UIThread)
#pragma once
#include "../shared/PanelCard.h"
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/ipc/ILocalServiceBridge.h>
#include <vector>

namespace khensu {

class GenreDistributionPanel final : public PanelCard {
  public:
    explicit GenreDistributionPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void paint(juce::Graphics& g) override;

  private:
    struct MetricRow {
        juce::String label;
        float trackValue;
        bool higherBetterUnknown = true;
    };

    juce::String genreId_;
    bool bootstrap_ = false;
    bool loading_ = false;
    juce::String fetchError_;
    std::vector<ipc::GenreReferencePoint> points_;
    float trackLufs_ = 0.0f, trackLra_ = 0.0f, trackCrest_ = 0.0f;

    void drawBox(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& label, std::vector<float> values,
                 float trackValue);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GenreDistributionPanel)
};

} // namespace khensu
