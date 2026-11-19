// desktop-app/Source/ui/Dashboard/SignalIntegrityPanel.h
// Three checks traditional peak/LUFS metering misses: inter-sample true
// peak (codec-overshoot risk), mono-bass compatibility below 120 Hz, and
// 50/60 Hz hum + hiss noise floor.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "AutoScanCompletion.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"

namespace khensu {

class IAppController;

class SignalIntegrityPanel final : public PanelCard {
  public:
    explicit SignalIntegrityPanel(IAppController& c);
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
        for (auto& c : cards_)
            c = Card{};
        run_.reset();
        resized();
        repaint();
    }

  private:
    AutoScanCompletion autoScan_;
    void runScan();

    struct Card {
        juce::String title, metric, verdict;
        juce::Colour colour = khensu::KhensuTheme::textTertiary;
        bool hasData = false;
    };

    juce::TextButton scanButton_{TRANS("Scan Signal Integrity")};
    Card cards_[3]; // true peak, mono bass, noise floor

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SignalIntegrityPanel)
};

} // namespace khensu
