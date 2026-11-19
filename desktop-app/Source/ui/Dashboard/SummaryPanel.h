// desktop-app/Source/ui/Dashboard/SummaryPanel.h
#pragma once
#include "../shared/PanelCard.h"
#include "../shared/BadgeComponent.h"
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

class SummaryPanel final : public PanelCard {
  public:
    explicit SummaryPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;

  private:
    juce::Label headline_;
    BadgeComponent loudnessBadge_, dynamicsBadge_, phaseBadge_, artifactBadge_;
    BadgeComponent bpmBadge_, keyBadge_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SummaryPanel)
};

} // namespace khensu
