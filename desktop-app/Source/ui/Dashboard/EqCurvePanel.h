// desktop-app/Source/ui/Dashboard/EqCurvePanel.h
#pragma once
#include "../shared/PanelCard.h"
#include <khensu/ui/EqCurveComponent.h>
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

class EqCurvePanel final : public PanelCard {
  public:
    explicit EqCurvePanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;

  private:
    EqCurveComponent eqCurve_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqCurvePanel)
};

} // namespace khensu
