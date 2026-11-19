// desktop-app/Source/ui/Dashboard/AiArtifactsPanel.h
// AI Artifacts: fused detection verdict + the 9-service checklist (Suno,
// Udio, BandLab, ACE-Step, MusicGen, HeartMuLa, YuE/DiffRhythm, Lyria,
// Other), the Artifacts Summary, and the clipping census - everything the
// /analyze/artifacts_scan detector registry reports (spec §5.1).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "AutoScanCompletion.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include <vector>

namespace khensu {

class IAppController;

class AiArtifactsPanel final : public PanelCard {
  public:
    explicit AiArtifactsPanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Intake auto-run (spec §1.2): same as pressing the scan button, but
    /// skipped while a scan is already in flight (button disabled).
    /// Dashboard auto-run (results gate): true if a scan started; `onDone`
    /// then fires once when it ends. See AutoScanCompletion.h.
    bool autoRun(std::function<void()> onDone)
    {
        // A scan already in flight queues one re-run instead of being
        // skipped (DSK-03) - see AutoScanCompletion::request().
        return autoScan_.request(std::move(onDone), [this] { runScan(); },
                                 [this] { return run_.isBusy(); });
    }

    void resetForSession() override
    {
        autoScan_.reset();
        scan_ = juce::var();
        notRunLine_.clear();
        findingCount_ = 0;
        clipCount_ = 0;
        clipPositions_.clear();
        services_.clear();
        summaryLabel_.setText({}, juce::dontSendNotification);
        run_.reset();
        resized();
        repaint();
    }

    /// Re-run once Demucs stems become available so the deep tier (per-stem
    /// realism) contributes to the result (spec §5.1 heavy pass).
    void rescanWithStems()
    {
        if (!run_.isBusy()) runScan();
    }

  private:
    AutoScanCompletion autoScan_;
    void runScan();

    struct ServiceRow {
        juce::String line;              // "resembles X (20%)" / "X: named by provenance (95%)"
        float likelihood = 0.0f;
        bool fromProvenance = false;
    };

    juce::TextButton scanButton_{TRANS("Scan for AI Artifacts")};
    juce::TextButton humanizeButton_{TRANS("Humanize / Deep Humanize")};
    juce::Label summaryLabel_; ///< Artifacts Summary (wrapped)

    juce::var scan_;                  // last result (verdict wording helpers)
    juce::String notRunLine_;         // detectors not run / errored
    int findingCount_ = 0;
    int clipCount_ = 0;
    juce::String clipPositions_;
    std::vector<ServiceRow> services_;

    /// Status line, error state and the run button's busy state (UX-8).
    PanelRunState run_{this, &scanButton_};
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AiArtifactsPanel)
};

} // namespace khensu
