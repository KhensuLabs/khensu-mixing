// desktop-app/Source/ui/Dashboard/SectionReadinessPanel.h
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include "../shared/PanelCard.h"

namespace khensu {

/// Per-section readiness bars, one per detected song section, using the
/// per-section metrics /analyze/sections already returns (lufs_integrated,
/// crest_db, harshness_score, sibilance_score in SectionData) but that
/// were previously only consumed as a single loudness-arc delta
/// (LoudnessArcPanel) -- this breaks the global 0-100 readiness concept
/// down per section so a dragging verse/bridge is visible instead of
/// hiding inside one whole-track score.
///
/// THREAD_CONTEXT(UIThread)
class SectionReadinessPanel final : public PanelCard {
  public:
    explicit SectionReadinessPanel(IAppController& controller);
    void setData(const AnalysisResult& r);
    void resized() override;
    void paint(juce::Graphics& g) override;

  private:
    struct SectionScore {
        juce::String label;
        float startSec = 0.0f, endSec = 0.0f;
        float score01 = 0.0f; // 0..1 composite readiness proxy
    };
    static float computeScore(const SectionData& s);

    std::vector<SectionScore> sections_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SectionReadinessPanel)
};

} // namespace khensu
