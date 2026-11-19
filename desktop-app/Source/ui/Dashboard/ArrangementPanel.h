// desktop-app/Source/ui/Dashboard/ArrangementPanel.h
// Arrangement Element Analyzer: Owsinski's five arrangement elements
// (foundation, rhythm, pad, lead, fills) classified per time window from
// the separated stems, drawn as an element-presence grid with gap /
// overcrowding flags.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include <vector>

namespace khensu {

class IAppController;

class ArrangementPanel final : public PanelCard {
  public:
    explicit ArrangementPanel(IAppController& c);
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
        windows_.clear();
        flags_.clear();
        totalDuration_ = 0.0f;
        flagLabel_.setText({}, juce::dontSendNotification);
        run_.reset();
        resized();
        repaint();
    }

  private:
    void runAnalysis();

    struct Window {
        float start = 0.0f, end = 0.0f;
        float shares[5] = {}; // foundation, rhythm, pad, lead, fills
        bool silent = false;
    };
    struct Flag {
        float start = 0.0f, end = 0.0f;
        juce::String code, message;
    };

    juce::TextButton analyzeButton_{TRANS("Analyze Arrangement")};
    juce::Label flagLabel_;

    std::vector<Window> windows_;
    std::vector<Flag> flags_;
    float totalDuration_ = 0.0f;

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ArrangementPanel)
};

} // namespace khensu
