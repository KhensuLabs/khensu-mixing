// desktop-app/Source/ui/Dashboard/TaxonomyMaskingPanel.h
// Masking & Frequency Collision over the canonical 9-bucket taxonomy
// (spec §3): hottest colliding bucket pairs from /analyze/stem_taxonomy,
// scored 0-1 by band-energy overlap. Lives on the Analysis page; the
// Vocal Workspace keeps its vocal-centric 4-stem matrix.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include <vector>

namespace khensu {

class IAppController;

class TaxonomyMaskingPanel final : public PanelCard {
  public:
    explicit TaxonomyMaskingPanel(IAppController& c);
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
        pairs_.clear();
        noteLabel_.setText({}, juce::dontSendNotification);
        fixLabel_.setText({}, juce::dontSendNotification);
        run_.reset();
        resized();
        repaint();
    }

  private:
    void runAnalysis();

    struct Pair {
        juce::String a, b;
        float score = 0.0f;
    };

    /// Concrete remedy for the hottest colliding pair (spec §3): EQ pocket /
    /// sidechain / HPF keyed on which two buckets fight. Bucket-identity
    /// based, order-independent.
    static juce::String suggestFix(const juce::String& a, const juce::String& b, float score);

    juce::TextButton analyzeButton_{TRANS("Analyze Collisions")};
    /// Status line, error state, the run button's busy state (UX-8).
    PanelRunState run_{this, &analyzeButton_};
    juce::Label noteLabel_;
    juce::Label fixLabel_; ///< suggested fix for the top pair

    std::vector<Pair> pairs_; ///< sorted hottest-first, capped for display

    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TaxonomyMaskingPanel)
};

} // namespace khensu
