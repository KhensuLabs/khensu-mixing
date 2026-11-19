// desktop-app/Source/ui/Dashboard/TuningReferencePanel.h
// Tuning reference: concert-pitch A (440/432/off) + drift. THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "AutoScanCompletion.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include <vector>

namespace khensu {
class IAppController;

class TuningReferencePanel final : public PanelCard {
  public:
    explicit TuningReferencePanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;
    /// Dashboard auto-run (results gate): true if a scan started; `onDone`
    /// then fires once when it ends. See AutoScanCompletion.h.
    bool autoRun(std::function<void()> onDone)
    {
        // A scan already in flight queues one re-run instead of being
        // skipped (DSK-03) - see AutoScanCompletion::request().
        return autoScan_.request(std::move(onDone), [this] { runAnalysis(); },
                                 [this] { return run_.isBusy(); });
    }

    void resetForSession() override
    {
        autoScan_.reset();
        drifting_ = false;
        verdict_.clear();
        drift_.clear();
        run_.reset();
        resized();
        repaint();
    }

    /// Fired on the message thread whenever a tuning estimate lands, so the
    /// Song Info "Musical identity" line can show "A = 440.0 Hz".
    std::function<void(float referenceHz)> onReferenceDetected;

  private:
    AutoScanCompletion autoScan_;
    void runAnalysis();
    juce::TextButton analyzeButton_{TRANS("Detect Tuning")};
    float refHz_ = 440.0f, cents_ = 0.0f, driftRange_ = 0.0f;
    bool drifting_ = false;
    juce::String verdict_;
    std::vector<juce::Point<float>> drift_; // x = time_sec, y = cents
    /// Status line, error state, the run button's busy state (UX-8).
    PanelRunState run_{this, &analyzeButton_};
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TuningReferencePanel)
};
} // namespace khensu
