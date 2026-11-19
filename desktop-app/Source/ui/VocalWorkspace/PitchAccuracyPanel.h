// desktop-app/Source/ui/VocalWorkspace/PitchAccuracyPanel.h
// Pitch-Accuracy Map: per-note cents-deviation tuning analysis (pYIN pitch
// tracking) - answers "which notes are flat/sharp, and by how much?" rather
// than a single broad pitch score.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include "MixTuningReference.h"
#include <vector>

namespace khensu {

class IAppController;

class PitchAccuracyPanel final : public PanelCard {
  public:
    explicit PitchAccuracyPanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Intake auto-run (spec §1.2): same as pressing the scan button, but
    /// skipped while an analysis is already in flight.
    void autoRun()
    {
        if (!run_.isBusy()) runAnalysis();
    }

  private:
    void runAnalysis();

    struct NoteRow {
        juce::String name;
        float centsDeviation = 0.0f;
        float stability = 0.0f;
    };

    juce::TextButton scanButton_{TRANS("Analyze Pitch")};
    /// Status line, error state and the run button's busy state (UX-8).
    PanelRunState run_{this, &scanButton_};

    std::vector<NoteRow> notes_;
    float meanAbsCents_ = 0.0f;
    float inTunePct_ = 0.0f;
    TuningReading tuning_; ///< grid the cents are measured on + off-grid caveat (AUDIT-016)
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchAccuracyPanel)
};

} // namespace khensu
