// desktop-app/Source/ui/Dashboard/SourceTriagePanel.h
// Fix-at-Source Triage: scans the session's source file for recording
// defects mixing can't fully repair (clipping, dropouts, comb filtering,
// noise floor, proximity boom, hum, DC offset) and classifies each as
// fixable / mitigable / re-record - the "80% recording / 20% mixing"
// principle surfaced before the user spends hours mixing around a flawed
// capture.
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

class SourceTriagePanel final : public PanelCard {
  public:
    explicit SourceTriagePanel(IAppController& c);
    void resized() override;

    /// Intake auto-run (spec §1.2): same as pressing the scan button, but
    /// skipped while a scan is already in flight (button disabled).
    /// Dashboard auto-run (results gate): true if a scan started; `onDone`
    /// then fires once when it ends. See AutoScanCompletion.h.
    bool autoRun(std::function<void()> onDone)
    {
        // A scan already in flight queues one re-run instead of being
        // skipped (DSK-03) - see AutoScanCompletion::request().
        return autoScan_.request(std::move(onDone), [this] { runTriage(); },
                                 [this] { return run_.isBusy(); });
    }

    void resetForSession() override
    {
        autoScan_.reset();
        rows_.clear();
        content_.removeAllChildren();
        verdictLabel_.setText({}, juce::dontSendNotification);
        viewport_.setVisible(true);
        run_.reset();
        resized();
        repaint();
    }

  private:
    AutoScanCompletion autoScan_;
    void runTriage();

    struct FindingRow final : public juce::Component {
        void paint(juce::Graphics& g) override;
        juce::String title, detail, category;
        float severity = 0.0f;
    };

    juce::TextButton scanButton_{TRANS("Scan Source Quality")};
    /// Status line, error state, the run button's busy state (UX-8).
    PanelRunState run_{this, &scanButton_};
    juce::Label verdictLabel_;

    juce::Viewport viewport_;
    juce::Component content_;
    std::vector<std::unique_ptr<FindingRow>> rows_;

    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SourceTriagePanel)
};

} // namespace khensu
