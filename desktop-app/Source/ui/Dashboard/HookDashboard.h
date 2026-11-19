// desktop-app/Source/ui/Dashboard/HookDashboard.h
#pragma once
#include "../shared/PanelCard.h"
#include "../shared/MeterBarComponent.h"
#include "../shared/TimelineComponent.h"
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

class HookDashboard final : public PanelCard {
  public:
    explicit HookDashboard(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;

  private:
    MeterBarComponent strengthMeter_, densityMeter_;
    TimelineComponent timeline_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HookDashboard)
};

} // namespace khensu
