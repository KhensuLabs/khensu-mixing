// desktop-app/Source/ui/Dashboard/ReferenceSpectralDeltaPanel.h
// Time-varying reference spectral-delta heatmap. THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "AutoScanCompletion.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include <memory>

namespace khensu {
class IAppController;

class ReferenceSpectralDeltaPanel final : public PanelCard {
  public:
    explicit ReferenceSpectralDeltaPanel(IAppController& c);
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
        img_ = {};
        refPath_.clear(); // the reference is per session
        run_.reset();
        resized();
        repaint();
    }

  private:
    AutoScanCompletion autoScan_;
    void runAnalysis();
    juce::TextButton loadButton_{TRANS("Load Reference")};
    juce::Image img_;
    juce::String refPath_;
    std::unique_ptr<juce::FileChooser> fileChooser_;
    /// Status line, error state and the run button's busy state (UX-8).
    PanelRunState run_{this, &loadButton_};
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReferenceSpectralDeltaPanel)
};
} // namespace khensu
