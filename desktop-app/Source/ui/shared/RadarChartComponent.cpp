// desktop-app/Source/ui/shared/RadarChartComponent.cpp
#include "RadarChartComponent.h"
#include "ChartStyle.h"

namespace khensu {

RadarChartComponent::RadarChartComponent()
{
    applyThemeColours();
    setSeriesName(TRANS("Track"));
    setEmptyText(TRANS("Deep analysis pending..."));
}

void RadarChartComponent::applyThemeColours()
{
    setStyle(desktopChartStyle());
    setColours(KhensuTheme::vizCategory(0), KhensuTheme::vizMedian);
}

void RadarChartComponent::setAxes(std::vector<RadarAxis> axes)
{
    std::vector<ui::chart::Spoke> spokes;
    spokes.reserve(axes.size());
    for (auto& a : axes)
        spokes.push_back({a.label, static_cast<double>(juce::jlimit(0.0f, 1.0f, a.value)), a.valueText});
    setSpokes(std::move(spokes));
}

void RadarChartComponent::setReference(std::vector<float> values, const juce::String& name)
{
    std::vector<double> v(values.begin(), values.end());
    ui::chart::RadarChart::setReference(std::move(v), name.isNotEmpty() ? name : (values.empty() ? juce::String() : TRANS("Genre")));
}

} // namespace khensu
