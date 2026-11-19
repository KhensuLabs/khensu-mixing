// desktop-app/Source/ui/Dashboard/ReadinessBreakdownBar.h
#pragma once
#include <JuceHeader.h>
#include <khensu/recommendations/MixReport.h>
#include <vector>

namespace khensu {

/// Compact stacked mini-bar showing which of the five readiness-penalty
/// terms is dragging the Mastering Readiness score down - answers "why is
/// my score 62" in one glance instead of requiring the full export report.
/// Hovering a segment shows its exact point deduction.
class ReadinessBreakdownBar final : public juce::Component {
  public:
    ReadinessBreakdownBar() { setInterceptsMouseClicks(true, false); }
    ~ReadinessBreakdownBar() override = default;

    void setBreakdown(const std::vector<ReadinessPenaltyTerm>& terms)
    {
        terms_ = terms;
        hasData_ = true;
        repaint();
    }
    void clear()
    {
        hasData_ = false;
        repaint();
    }

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent&) override
    {
        hoveredIndex_ = -1;
        repaint();
    }
    void mouseDown(const juce::MouseEvent& e) override;

    std::function<void(const std::string&)> onSegmentClicked;

  private:
    std::vector<ReadinessPenaltyTerm> terms_;
    bool hasData_ = false;
    int hoveredIndex_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReadinessBreakdownBar)
};

} // namespace khensu
