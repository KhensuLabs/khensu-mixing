// desktop-app/Source/ui/Dashboard/PanBalancePanel.h
// Pan Energy Balance: per-band L/R bias over time - a drifting guitar bus
// or lopsided hi-hat becomes a visible line instead of a hunch, and a wide
// sub gets called out before it breaks mono playback.
// THREAD_CONTEXT(UIThread)
#pragma once
#include "../shared/HoverCells.h"
#include <JuceHeader.h>
#include "AutoScanCompletion.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include <map>
#include <vector>

namespace khensu {

class IAppController;

class PanBalancePanel final : public PanelCard, public juce::SettableTooltipClient {
  public:
    /// VIZ-U3: the readout under the pointer.
    void mouseMove(const juce::MouseEvent& e) override { hover_.update(*this, e.position); }
    void mouseExit(const juce::MouseEvent&) override { setTooltip({}); }
    explicit PanBalancePanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Intake auto-run (spec §1.2): same as pressing the analyze button, but
    /// skipped while an analysis is already in flight (button disabled).
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
        bands_.clear();
        warningsLabel_.setText({}, juce::dontSendNotification);
        run_.reset();
        resized();
        repaint();
    }

  private:
    AutoScanCompletion autoScan_;
    void runAnalysis();

    struct Band {
        juce::String name;
        std::vector<float> biasCurve; // -1..+1 per window
        float bias = 0.0f, width = 0.0f;
        /// S/(M+S), 0..0.5+ - the service's side_share (falls back to width/2
        /// for older services; width is 2S/(M+S) and saturates at 1).
        float sideShare = 0.0f;
    };

    juce::TextButton analyzeButton_{TRANS("Analyze Pan Balance")};
    juce::Label warningsLabel_;

    std::vector<Band> bands_;

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

    mutable HoverCells hover_; ///< VIZ-U3, filled in paint()
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PanBalancePanel)
};

} // namespace khensu
