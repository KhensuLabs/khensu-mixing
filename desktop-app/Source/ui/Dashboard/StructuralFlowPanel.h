// desktop-app/Source/ui/Dashboard/StructuralFlowPanel.h
#pragma once
#include "../shared/PanelCard.h"
#include "../shared/MeterBarComponent.h"
#include "../shared/TimelineComponent.h"
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

class StructuralFlowPanel final : public PanelCard {
  public:
    explicit StructuralFlowPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;

  private:
    MeterBarComponent arcMeter_;
    TimelineComponent timeline_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StructuralFlowPanel)
};

} // namespace khensu
