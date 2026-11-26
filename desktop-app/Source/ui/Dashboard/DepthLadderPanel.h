// desktop-app/Source/ui/Dashboard/DepthLadderPanel.h
// Depth Ladder: front-to-back staging of the separated stems, measured
// from the cues that create depth perception (reverb tail, HF rolloff,
// transient bluntness). Level has meters and panning has goniometers;
// this gives the third dimension a display of its own.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ChartKitDesktop.h"
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include <vector>

namespace khensu {

class IAppController;

class DepthLadderPanel final : public PanelCard {
  public:
    explicit DepthLadderPanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Intake auto-run (spec §1.2): same as pressing the analyze button, but
    /// skipped while an analysis is already in flight (button disabled).
    void autoRun()
    {
        if (!run_.isBusy()) runAnalysis();
    }

    void resetForSession() override
    {
        stems_.clear();
        rebuildChart();
        suggestionLabel_.setText({}, juce::dontSendNotification);
        run_.reset();
        resized();
        repaint();
    }

  private:
    void runAnalysis();

    struct StemDepth {
        juce::String name;
        float depth = 0.0f, wetness = 0.0f, darkness = 0.0f, softness = 0.0f;
    };

    juce::TextButton analyzeButton_{TRANS("Map Depth")};
    juce::Label suggestionLabel_;

    std::vector<StemDepth> stems_;

    /// VIS-1: the ladder as a ChartKit dot chart.
    struct Chart final : ThemedLineChart {
        DepthLadderPanel* owner = nullptr;
        void extendReadout(ui::chart::Readout& r, double x) override;
    };
    void rebuildChart();
    Chart chart_;

    /// Status line, error state and the run button's busy state (UX-8).
    PanelRunState run_{this, &analyzeButton_};
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DepthLadderPanel)
};

} // namespace khensu
