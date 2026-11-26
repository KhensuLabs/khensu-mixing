// desktop-app/Source/ui/Dashboard/StreamingNormalizationPanel.h
// Per-platform streaming loudness-normalization preview: how Spotify/Apple/
// YouTube/Tidal will play the master (gain applied + true-peak headroom).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "AutoScanCompletion.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include <khensu/ui/ChartKit.h>
#include "../shared/PanelRunState.h"
#include <vector>

namespace khensu {
class IAppController;

class StreamingNormalizationPanel final : public PanelCard {
  public:
    explicit StreamingNormalizationPanel(IAppController& c);
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
        measurable_ = true;
        note_.clear();
        rows_.clear();
        run_.reset();
        resized();
        repaint();
    }

  private:
    AutoScanCompletion autoScan_;
    void runAnalysis();

    struct Row {
        juce::String name;
        float gain = 0, lufs = 0, headroom = 0;
        bool limiter = false;
    };
    juce::TextButton analyzeButton_{TRANS("Preview Platforms")};
    /// Status line, error state, the run button's busy state (UX-8).
    PanelRunState run_{this, &analyzeButton_};
    /// False when the service could not measure a meaningful loudness (e.g.
    /// silence): `note_` explains why and no per-platform gains are shown.
    bool measurable_ = true;
    juce::String note_;
    float integratedLufs_ = 0.0f, truePeak_ = 0.0f;
    std::vector<Row> rows_;
    ui::chart::ChartSummary summary_; ///< VIS-8
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StreamingNormalizationPanel)
};
} // namespace khensu
