// desktop-app/Source/ui/Dashboard/PhasePanel.h
#pragma once
#include "../shared/PanelCard.h"
#include "../shared/MeterBarComponent.h"
#include "../shared/BadgeComponent.h"
#include "../shared/KhensuErrorState.h"
#include <khensu/analysis/AnalysisResult.h>
#include <vector>
#include <utility>

namespace khensu {

// Plots r.mono_compat->per_band_correlation as a small bar-per-band strip.
// That data was already computed for every deep analysis but previously only
// reached the UI as a single "weakest band" text summary (see monoCompatLabel_
// below) - this is the "correlation-vs-frequency strip" the 2026-07-07 audit
// flagged as still being a text readout instead of a plotted visualization.
class CorrelationStripComponent final : public juce::Component {
  public:
    void setBands(std::vector<std::pair<juce::String, float>> bands);
    void paint(juce::Graphics& g) override;

  private:
    std::vector<std::pair<juce::String, float>> bands_;
};

class PhasePanel final : public PanelCard {
  public:
    explicit PhasePanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;

  private:
    MeterBarComponent corrMeter_;
    BadgeComponent polarityBadge_, combBadge_;
    MeterBarComponent harshnessMeter_;
    juce::Label harshnessPeakLabel_;
    // Extended-analysis rows (mono compatibility + M/S imbalance) - this data
    // was fetched on every deep analysis but had no UI consumer.
    juce::Label monoCompatLabel_;
    juce::Label msImbalanceLabel_;
    juce::Label symmetryLabel_;
    CorrelationStripComponent correlationStrip_;

    // Mono Fold-Down culprit finder: when a band's correlation fails and
    // stems exist, per-stem pan/width analysis names WHICH stem is carrying
    // the anti-phase/wide energy in that band - turning "midrange fails in
    // mono" into "the synths ('other' stem) are 40% side energy at 350-2k".
    juce::TextButton findCulpritButton_{TRANS("Find Mono Culprits")};
    juce::Label culpritLabel_;
    KhensuErrorState errorState_; ///< stem culprit analysis failed + Retry
    void findCulprits();
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhasePanel)
};

} // namespace khensu
