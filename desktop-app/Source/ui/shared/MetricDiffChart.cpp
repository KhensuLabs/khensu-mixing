// desktop-app/Source/ui/shared/MetricDiffChart.cpp
#include "MetricDiffChart.h"
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"

namespace khensu {

void MetricDiffChart::paint(juce::Graphics& g)
{
    std::vector<const MetricDiffTable::Row*> numeric;
    for (const auto& r : rows_)
        if (r.hasNumeric) numeric.push_back(&r);

    if (numeric.empty()) {
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.5f));
        g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::italic));
        g.drawText("No numeric metrics to chart", getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    auto bounds = getLocalBounds();
    const juce::Colour colA = KhensuTheme::textSecondary;
    const juce::Colour colB = KhensuTheme::accentBlue;

    auto legendArea = bounds.removeFromTop(14);
    g.setFont(khensu::KhensuTheme::uiFont(8.0f, juce::Font::plain));
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

    for (size_t i = 0; i < numeric.size(); ++i) {
        const auto& row = *numeric[i];
        auto rowArea =
            juce::Rectangle<int>(bounds.getX(), bounds.getY() + (int)i * rowH, bounds.getWidth(), rowH).reduced(0, 3);

        g.setColour(KhensuTheme::textSecondary);
        g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::plain));
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
        g.setFont(khensu::KhensuTheme::uiFont(8.0f, juce::Font::plain));
        g.drawText(row.valueA, barA.toNearestInt(), juce::Justification::centredRight, false);
        g.drawText(row.valueB, barB.toNearestInt(), juce::Justification::centredRight, false);
    }
}

} // namespace khensu
