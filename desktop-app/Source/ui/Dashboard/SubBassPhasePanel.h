// desktop-app/Source/ui/Dashboard/SubBassPhasePanel.h
// Sub-bass phase alignment between the kick and the bass fundamental (Gemini
// audit). A goniometer/Lissajous of the two stems' low band plus a
// phase-correlation history, so a low end that cancels on mono/club systems
// is visible instead of only audible on the big rig. Requires separated stems.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include <vector>

namespace khensu {

class IAppController;

class SubBassPhasePanel final : public PanelCard {
  public:
    explicit SubBassPhasePanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Intake auto-run (spec §1.2): fires once stems exist; skipped while a
    /// pass is already in flight (button disabled).
    void autoRun()
    {
        if (!run_.isBusy()) runAnalysis();
    }

    void resetForSession() override
    {
        verdict_.clear();
        gonio_.clear();
        history_.clear();
        run_.reset();
        resized();
        repaint();
    }

  private:
    void runAnalysis();
    void drawGoniometer(juce::Graphics& g, juce::Rectangle<int> area);
    void drawHistory(juce::Graphics& g, juce::Rectangle<int> area);
    static juce::Colour phaseColour(float correlation);

    juce::TextButton analyzeButton_{TRANS("Analyze Phase")};
    /// Status line, error state, the run button's busy state (UX-8).
    PanelRunState run_{this, &analyzeButton_};

    float correlation_ = 0.0f;
    float phaseDeg_ = 0.0f;
    juce::String verdict_;
    std::vector<juce::Point<float>> gonio_;   ///< [kick, bass] points, -1..1
    std::vector<juce::Point<float>> history_; ///< x = time_sec, y = correlation

    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubBassPhasePanel)
};

} // namespace khensu
