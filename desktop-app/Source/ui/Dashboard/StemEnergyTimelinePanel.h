// desktop-app/Source/ui/Dashboard/StemEnergyTimelinePanel.h
// Stem contribution stacked-area timeline. Requires separated stems.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
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
        stems_.clear();
        frames_.clear();
        run_.reset();
        resized();
        repaint();
    }

  private:
    void runAnalysis();
    juce::TextButton analyzeButton_{TRANS("Analyze Stems")};
    /// Status line, error state, the run button's busy state (UX-8).
    PanelRunState run_{this, &analyzeButton_};
    std::vector<juce::String> stems_;        // ordered
    std::vector<std::vector<float>> frames_; // [frame][stem] fraction
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
