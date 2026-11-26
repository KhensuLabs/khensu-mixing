// desktop-app/Source/ui/Dashboard/TaxonomyEnergyPanel.h
// Stem Energy Balance over the canonical 9-bucket taxonomy (spec §3):
// Vocals, Bass, Synth, Kick, Snare, Percussion, Drums (rollup),
// Atmosphere/FX, Other - energy share bars from /analyze/stem_taxonomy.
// Absent buckets are greyed out, not zeroed; derived buckets carry a
// confidence dim so estimates never read as measurements.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ChartKitDesktop.h"
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include <vector>

namespace khensu {

class IAppController;

class TaxonomyEnergyPanel final : public PanelCard {
  public:
    explicit TaxonomyEnergyPanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Intake auto-run (spec §1.2): same as pressing the analyze button, but
    /// skipped while an analysis is already in flight (button disabled).
    void autoRun()
    {
        if (!run_.isBusy()) runAnalysis();
    }

    void resetForSession() override
    {
        buckets_.clear();
        provisional_ = false;
        run_.reset();
        rebuildChart();
        resized();
        repaint();
    }

    /// Provisional estimate-then-refine state (spec §3): before Demucs stems
    /// exist, show the full-mix tonal tilt as a coarse proxy with a
    /// "refining when stems separate" banner instead of a bare "unavailable".
    /// Replaced by runAnalysis() once stems land. `low/mid/high` are 0..1
    /// spectral balance fractions from the main analysis.
    void setProvisionalEstimate(float low, float mid, float high);

  private:
    void runAnalysis();
    void rebuildChart();

    struct Bucket {
        juce::String id, label;
        float sharePct = 0.0f;
        float targetPct = -1.0f; ///< genre target share; <0 = no target (§8)
        float confidence = 0.0f;
        bool present = false;
        bool rollup = false;
    };

    juce::TextButton analyzeButton_{TRANS("Analyze Stem Energy")};
    /// Status line, error state, the run button's busy state (UX-8).
    PanelRunState run_{this, &analyzeButton_};

    std::vector<Bucket> buckets_;
    bool provisional_ = false; ///< §3: buckets_ hold a full-mix estimate
    ThemedBarChart chart_;     ///< VIS-1: the shares as a ChartKit bar chart

    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TaxonomyEnergyPanel)
};

} // namespace khensu
