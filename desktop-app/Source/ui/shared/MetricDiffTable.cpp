// desktop-app/Source/ui/shared/MetricDiffTable.cpp
#include "MetricDiffTable.h"
#include "KhensuTheme.h"

namespace khensu {

void MetricDiffTable::paint(juce::Graphics& g)
{
    auto inner = getLocalBounds();

    auto header = inner.removeFromTop(20);
    g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::bold));
    g.setColour(KhensuTheme::textSecondary);
    g.drawText("ANALYSIS METRIC", header.removeFromLeft(150), juce::Justification::centredLeft);
    g.drawText(labelA_.toUpperCase(), header.removeFromLeft(120), juce::Justification::centred);
    g.drawText(labelB_.toUpperCase(), header.removeFromLeft(120), juce::Justification::centred);
    g.drawText("DELTA VALUE", header, juce::Justification::centredRight);

    g.setColour(KhensuTheme::borderSubtle);
    g.drawHorizontalLine(inner.getY(), (float)inner.getX(), (float)inner.getRight());
    inner.removeFromTop(8);

    for (const auto& row : rows_) {
        auto rowBounds = inner.removeFromTop(24);
        g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::bold));
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(row.name, rowBounds.removeFromLeft(150), juce::Justification::centredLeft);

        g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
        g.setColour(khensu::KhensuTheme::textPrimary);
        g.drawText(row.valueA, rowBounds.removeFromLeft(120), juce::Justification::centred);
        g.drawText(row.valueB, rowBounds.removeFromLeft(120), juce::Justification::centred);

        g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::bold));
        g.setColour(row.deltaColour);
        g.drawText(row.delta, rowBounds, juce::Justification::centredRight);
    }
}

} // namespace khensu
