// desktop-app/Source/ui/shared/MetricTileComponent.h
#pragma once
#include <JuceHeader.h>
#include "BadgeComponent.h"
#include "KhensuTheme.h"

namespace khensu {

class MetricTileComponent final : public juce::Component {
  public:
    MetricTileComponent() = default;
    void setMetric(const juce::String& label, const juce::String& value, const juce::String& unit, BadgeStatus status);

    // Testable getters
    BadgeStatus currentStatus() const { return status_; }
    juce::String currentLabel() const { return label_; }

    void paint(juce::Graphics& g) override;
    void mouseEnter(const juce::MouseEvent&) override
    {
        hovered_ = true;
        repaint();
    }
    void mouseExit(const juce::MouseEvent&) override
    {
        hovered_ = false;
        repaint();
    }

    /// Right-click power features. onCompareToLastSession may be left unset
    /// for metrics with no historical per-session data (that menu item is
    /// then greyed out rather than shown as if it worked).
    void mouseDown(const juce::MouseEvent& e) override;
    std::function<void()> onExplain;
    std::function<void()> onCompareToLastSession;
    std::function<void()> onAddToReadinessWeighting;

  private:
    juce::String label_, value_, unit_;
    BadgeStatus status_ = BadgeStatus::Neutral;
    bool hovered_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetricTileComponent)
};

} // namespace khensu
