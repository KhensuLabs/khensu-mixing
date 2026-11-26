// desktop-app/Source/ui/shared/MetricDiffChart.h
#pragma once
#include <JuceHeader.h>
#include "MetricDiffTable.h"
#include <vector>

namespace khensu {

/// Visual bar-diff companion to MetricDiffTable: one horizontal pair of bars
/// per numeric metric (rangeMin..rangeMax normalized), so "what changed"
/// between two snapshots/renders reads at a glance instead of scanning the
/// table's numbers. Rows without hasNumeric set are skipped -- callers that
/// only populate the string columns just don't get a bar for that row.
class MetricDiffChart final : public juce::Component {
  public:
    MetricDiffChart() = default;

    void setRows(std::vector<MetricDiffTable::Row> rows)
    {
        rows_ = std::move(rows);
        repaint();
    }
    void setColumnLabels(const juce::String& labelA, const juce::String& labelB)
    {
        labelA_ = labelA;
        labelB_ = labelB;
        repaint();
    }

    static int preferredHeight(int numericRowCount) { return 8 + numericRowCount * 30; }

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    /// VIS-8: what a screen reader reads.
    juce::String summary() const;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

  private:
    juce::Point<float> hoverPos_{-1.0f, -1.0f};
    std::vector<MetricDiffTable::Row> rows_;
    juce::String labelA_ = "A", labelB_ = "B";

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetricDiffChart)
};

} // namespace khensu
