// desktop-app/Source/ui/VocalWorkspace/VocalHookPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/MeterBarComponent.h"
#include "../shared/BadgeComponent.h"
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

// THREAD_CONTEXT(UIThread)
class VocalHookPanel final : public PanelCard {
  public:
    explicit VocalHookPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;

  private:
    MeterBarComponent strengthMeter_, densityMeter_;
    BadgeComponent countBadge_;
    juce::Label pendingLabel_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocalHookPanel)
};

} // namespace khensu
