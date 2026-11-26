// desktop-app/Source/ui/shared/MetricDiffChart.cpp
#include "MetricDiffChart.h"
#include "KhensuEmptyState.h"
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"
#include "ChartStyle.h"

namespace khensu {

void MetricDiffChart::paint(juce::Graphics& g)
{
    std::vector<const MetricDiffTable::Row*> numeric;
    for (const auto& r : rows_)
        if (r.hasNumeric) numeric.push_back(&r);

    if (numeric.empty()) {
        KhensuEmptyState::paintInto(g, getLocalBounds(), TRANS("No numeric metrics to chart"), {}); // UXR-6
        return;
    }

    auto bounds = getLocalBounds();
    // VIS-4: A and B are identities - categorical slots, never severity.
    const juce::Colour colA = KhensuTheme::vizCategory(0);
    const juce::Colour colB = KhensuTheme::vizCategory(1);

    auto legendArea = bounds.removeFromTop(14);
    g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
    g.setColour(colA);
    g.fillRect(legendArea.getRight() - 90, legendArea.getY() + 3, 8, 8);
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(labelA_, legendArea.getRight() - 78, legendArea.getY(), 40, 14, juce::Justification::left, false);
    g.setColour(colB);
    g.fillRect(legendArea.getRight() - 40, legendArea.getY() + 3, 8, 8);
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(labelB_, legendArea.getRight() - 28, legendArea.getY(), 28, 14, juce::Justification::left, false);

    const int rowH = bounds.getHeight() / (int)numeric.size();
    const int labelW = 110;
    ui::chart::Readout hoverReadout;

    for (size_t i = 0; i < numeric.size(); ++i) {
        const auto& row = *numeric[i];
        auto rowArea =
            juce::Rectangle<int>(bounds.getX(), bounds.getY() + (int)i * rowH, bounds.getWidth(), rowH).reduced(0, 3);

        g.setColour(KhensuTheme::textSecondary);
        g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
        g.drawText(row.name, rowArea.removeFromLeft(labelW), juce::Justification::centredLeft, false);

        auto barArea = rowArea.toFloat();
        const float rangeSpan = (float)(row.rangeMax - row.rangeMin);
        auto fracFor = [&](double v) {
            if (rangeSpan <= 0.0f) return 0.0f;
            return juce::jlimit(0.0f, 1.0f, (float)((v - row.rangeMin) / rangeSpan));
        };
        const float fracA = fracFor(row.numA);
        const float fracB = fracFor(row.numB);

        const float barH = barArea.getHeight() * 0.4f;
        auto barA = barArea.removeFromTop(barH);
        auto barB = barArea.removeFromBottom(barH);

        g.setColour(KhensuTheme::bgSurface.brighter(0.05f));
        g.fillRect(barA);
        g.fillRect(barB);

        g.setColour(colA);
        g.fillRect(barA.withWidth(barA.getWidth() * fracA));
        g.setColour(colB);
        g.fillRect(barB.withWidth(barB.getWidth() * fracB));

        g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.8f));
        g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
        g.drawText(row.valueA, barA.toNearestInt(), juce::Justification::centredRight, false);
        g.drawText(row.valueB, barB.toNearestInt(), juce::Justification::centredRight, false);

        // VIS-1: the ChartKit readout for the row under the pointer.
        if (juce::Rectangle<int>(bounds.getX(), bounds.getY() + (int)i * rowH, bounds.getWidth(), rowH).toFloat().contains(hoverPos_)) {
            ui::chart::Readout r;
            r.title = row.name;
            r.lines.push_back({labelA_ + " " + row.valueA, colA});
            r.lines.push_back({labelB_ + " " + row.valueB, colB});
            hoverReadout = r;
        }
    }
    if (!hoverReadout.lines.empty())
        ui::chart::ChartView::paintReadout(g, hoverReadout, hoverPos_, getLocalBounds().toFloat(), desktopChartStyle());
}

void MetricDiffChart::mouseMove(const juce::MouseEvent& e)
{
    hoverPos_ = e.position;
    repaint();
}

void MetricDiffChart::mouseExit(const juce::MouseEvent&)
{
    hoverPos_ = {-1.0f, -1.0f};
    repaint();
}

juce::String MetricDiffChart::summary() const
{
    juce::StringArray parts;
    for (const auto& r : rows_)
        if (r.hasNumeric) parts.add(r.name + " " + labelA_ + " " + r.valueA + ", " + labelB_ + " " + r.valueB);
    return parts.isEmpty() ? TRANS("Metric comparison: no numeric metrics.")
                           : TRANS("Metric comparison") + ": " + parts.joinIntoString("; ") + ".";
}

std::unique_ptr<juce::AccessibilityHandler> MetricDiffChart::createAccessibilityHandler()
{
    return ui::chart::makeChartAccessibilityHandler(*this, [this] { return summary(); });
}

} // namespace khensu
