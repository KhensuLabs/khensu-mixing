// desktop-app/Source/ui/Dashboard/PhaseHeatmapPanel.h
// Full-spectrum phase-correlation heatmap over time. THREAD_CONTEXT(UIThread)
#pragma once
#include "../shared/HoverCells.h"
#include <JuceHeader.h>
#include "AutoScanCompletion.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"

namespace khensu {
class IAppController;

class PhaseHeatmapPanel final : public PanelCard, public juce::SettableTooltipClient {
  public:
    /// VIZ-U3: the value under the pointer (time, frequency, value).
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent&) override { setTooltip({}); }
    explicit PhaseHeatmapPanel(IAppController& c);
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
        run_.reset();
        resized();
        repaint();
    }

  private:
    AutoScanCompletion autoScan_;
    void runAnalysis();
    juce::TextButton analyzeButton_{TRANS("Analyze Phase")};
    bool stereo_ = true;
    juce::Image img_;
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

    std::vector<std::vector<float>> grid_, grid2_; ///< VIZ-U3: [frame][band] behind the image(s)
    std::vector<float> edgesHz_;
    float durationSec_ = 0.0f;
    juce::Rectangle<float> gridArea_, grid2Area_; ///< where they were drawn
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhaseHeatmapPanel)
};
} // namespace khensu
