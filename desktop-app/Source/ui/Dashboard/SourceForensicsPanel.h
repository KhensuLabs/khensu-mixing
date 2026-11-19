// desktop-app/Source/ui/Dashboard/SourceForensicsPanel.h
// Source-quality forensics: brickwall (lossy-origin), fake hi-res, effective
// bit depth. THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "AutoScanCompletion.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"

namespace khensu {
class IAppController;

class SourceForensicsPanel final : public PanelCard {
  public:
    explicit SourceForensicsPanel(IAppController& c);
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
        header_.clear();
        findings_.clear();
        run_.reset();
        resized();
        repaint();
    }

  private:
    AutoScanCompletion autoScan_;
    void runAnalysis();
    juce::TextButton analyzeButton_{TRANS("Analyze Source")};
    juce::String header_;
    struct Finding {
        juce::String label, detail;
        bool flag = false;
    };
    std::vector<Finding> findings_;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SourceForensicsPanel)
};
} // namespace khensu
