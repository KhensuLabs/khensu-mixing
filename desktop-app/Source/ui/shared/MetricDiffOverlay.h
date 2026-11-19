// desktop-app/Source/ui/shared/MetricDiffOverlay.h
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h" // ThemeListener
#include "MetricDiffTable.h"
#include "MetricDiffChart.h"
#include "ModalCardHost.h"
#include <vector>

namespace khensu {

/// Modal host for MetricDiffTable - the shared comparison UI used by both
/// the Dashboard's two-slot A/B Snapshot Compare and Snapshot History's
/// Compare-to-current action.
class MetricDiffOverlay final : public ModalCardHost {
  public:
    MetricDiffOverlay(const juce::String& title, std::vector<MetricDiffTable::Row> rows, const juce::String& labelA,
                      const juce::String& labelB);

    void resized() override;

    std::function<void()> onDismiss;

  protected:
    juce::Rectangle<int> cardBounds() const override { return card_.getBounds(); }
    void dismiss() override
    {
        if (onDismiss) onDismiss();
    }

  private:
    juce::Component card_;
    juce::Label titleLabel_;
    juce::TextButton closeButton_{TRANS("Close")};
    MetricDiffTable table_;
    MetricDiffChart chart_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetricDiffOverlay)
};

} // namespace khensu
