#pragma once
#include "../shared/PanelCard.h"
#include "../shared/RadarChartComponent.h"
#include "../shared/SonicOrbitComponent.h"
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

class SonicFingerprintPanel final : public PanelCard {
  public:
    explicit SonicFingerprintPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;

  private:
    RadarChartComponent radar_;
    SonicOrbitComponent orbit_;
    juce::TextButton orbitModeToggle_{TRANS("Render History")};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SonicFingerprintPanel)
};

} // namespace khensu
