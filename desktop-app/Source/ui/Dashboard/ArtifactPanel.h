// desktop-app/Source/ui/Dashboard/ArtifactPanel.h
#pragma once
#include "../shared/PanelCard.h"
#include "../shared/BadgeComponent.h"
#include "../shared/MeterBarComponent.h"
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

class ArtifactPanel final : public PanelCard {
  public:
    explicit ArtifactPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;

  private:
    BadgeComponent clippingBadge_, aiBadge_;
    juce::Label noiseLabel_;
    // Noise floor and the (uncalibrated, informational) sample-reversal rate
    // get the same at-a-glance meter treatment every other panel uses.
    MeterBarComponent noiseFloorMeter_;
    MeterBarComponent aiConfidenceMeter_; ///< shows artifacts.sample_reversal_rate (AUDIT-010)
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ArtifactPanel)
};

} // namespace khensu
