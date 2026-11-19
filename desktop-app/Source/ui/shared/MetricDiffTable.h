// desktop-app/Source/ui/shared/MetricDiffTable.h
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"
#include <vector>
#include "KhensuTheme.h"

namespace khensu {

/// Reusable row-by-row metric diff table (name / value A / value B / delta).
/// Shared by the Dashboard's two-slot A/B Snapshot Compare and Snapshot
/// History's Compare-to-current action, so both present the same diff UI
/// instead of each rendering their own bespoke table.
class MetricDiffTable final : public juce::Component {
  public:
    struct Row {
        juce::String name;
        juce::String valueA, valueB;
        juce::String delta;
        juce::Colour deltaColour = khensu::KhensuTheme::textPrimary;

        // Optional raw values + a plausible display range, for MetricDiffChart's
        // bar-diff visualization (the table above is numbers-only; this makes
        // "what changed" readable at a glance without scanning the columns).
        // hasNumeric stays false for rows built without these (e.g. legacy
        // string-only callers), so the chart just skips that row.
        bool hasNumeric = false;
        double numA = 0.0, numB = 0.0;
        double rangeMin = 0.0, rangeMax = 1.0;
    };

    void setColumnLabels(const juce::String& labelA, const juce::String& labelB)
    {
        labelA_ = labelA;
        labelB_ = labelB;
        repaint();
    }
    void setRows(std::vector<Row> rows)
    {
        rows_ = std::move(rows);
        repaint();
    }

    static int preferredHeight(int rowCount) { return 20 + 8 + rowCount * 24; }

    void paint(juce::Graphics& g) override;

  private:
    std::vector<Row> rows_;
    juce::String labelA_ = "A", labelB_ = "B";
};

} // namespace khensu
