// desktop-app/Source/ui/MixMasterEngine/GainStagingPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/PanelCard.h"
#include "../shared/MeterBarComponent.h"
#include "../shared/BadgeComponent.h"
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

// THREAD_CONTEXT(UIThread)
class GainStagingPanel final : public PanelCard {
  public:
    explicit GainStagingPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    MeterBarComponent lufsMeter_;
    MeterBarComponent lraMeter_;
    MeterBarComponent crestMeter_;
    MeterBarComponent drMeter_;
    BadgeComponent peakBadge_;
    BadgeComponent hyperCompBadge_; // over-processing warning (crest/PLR)
    juce::Label gainCalloutLabel_;

    float gainAdjustDb_ = 0.0f;

    // stored for paint()
    float balanceLow_ = 0.0f;
    float balanceMid_ = 0.0f;
    float balanceHigh_ = 0.0f;
    float stereoWidth_ = 0.0f;
    float stereoCorr_ = 0.0f;
    juce::Rectangle<int> specBalanceArea_;
    juce::Rectangle<int> stereoBarArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GainStagingPanel)
};

} // namespace khensu
