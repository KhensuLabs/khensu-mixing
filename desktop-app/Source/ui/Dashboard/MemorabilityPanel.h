// desktop-app/Source/ui/Dashboard/MemorabilityPanel.h
#pragma once
#include "../shared/HoverCells.h"
#include "../shared/PanelCard.h"
#include "../shared/MeterBarComponent.h"
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/session/SessionManager.h>
#include <vector>

namespace khensu {

// Plots memorability_score across recent sessions (already stored in
// SessionManager::AnalysisSummary for the Session Intake trend chart, but
// unused here) so Memorability isn't just three meters over a blank card.
class MemorabilityTrendComponent final : public juce::Component, public juce::SettableTooltipClient {
  public:
    /// VIZ-U3: the readout under the pointer.
    void mouseMove(const juce::MouseEvent& e) override { hover_.update(*this, e.position); }
    void mouseExit(const juce::MouseEvent&) override { setTooltip({}); }
    void setHistory(std::vector<float> scoresOldestFirst);
    void paint(juce::Graphics& g) override;

  private:
    std::vector<float> scores_;
    mutable HoverCells hover_; ///< VIZ-U3, filled in paint()
};

class MemorabilityPanel final : public PanelCard {
  public:
    explicit MemorabilityPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;

  private:
    MeterBarComponent scoreMeter_, earwormMeter_, salienceMeter_;
    MemorabilityTrendComponent trend_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MemorabilityPanel)
};

} // namespace khensu
