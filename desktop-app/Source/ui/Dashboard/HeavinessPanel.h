// desktop-app/Source/ui/Dashboard/HeavinessPanel.h
#pragma once
#include "../shared/PanelCard.h"
#include "../shared/RadarChartComponent.h"
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

class HeavinessPanel final : public PanelCard {
  public:
    explicit HeavinessPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;

  private:
    RadarChartComponent radar_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeavinessPanel)
};

} // namespace khensu
