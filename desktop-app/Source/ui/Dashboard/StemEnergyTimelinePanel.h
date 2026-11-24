// desktop-app/Source/ui/Dashboard/StemEnergyTimelinePanel.h
// VIZ-5: the stems over time - a streamgraph of each separated stem's energy
// (the arrangement as shapes) and, for a chosen pair, the masking map
// (frequency x time: where the two fight for the same band). Measured
// natively from the stem files (ui/shared/StemViz.h), so it needs no engine
// round trip once the stems exist. Requires separated stems.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <map>
#include <memory>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include "../shared/StemViz.h"
#include <vector>

namespace khensu {
class IAppController;

class StemEnergyTimelinePanel final : public PanelCard {
  public:
    explicit StemEnergyTimelinePanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void autoRun()
    {
        if (!run_.isBusy()) runAnalysis();
    }

    void resetForSession() override
    {
        timelines_.clear();
        stream_.setStream({});
        masking_.setMap({});
        pairBox_.clear(juce::dontSendNotification);
        run_.reset();
        resized();
        repaint();
    }

    StemStreamGraph& streamGraph() noexcept { return stream_; }
    MaskingHeatmapView& maskingMap() noexcept { return masking_; }

  private:
    void runAnalysis();
    void showPair(int index);
    juce::TextButton analyzeButton_{TRANS("Analyze Stems")};
    /// Status line, error state, the run button's busy state (UX-8).
    PanelRunState run_{this, &analyzeButton_};
    std::map<std::string, std::shared_ptr<const TrackTimeline>> timelines_;
    std::vector<std::pair<std::string, std::string>> pairs_;
    StemStreamGraph stream_;
    juce::ComboBox pairBox_;
    MaskingHeatmapView masking_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemEnergyTimelinePanel)
};
} // namespace khensu
