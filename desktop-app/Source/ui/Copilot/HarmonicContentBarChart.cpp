// desktop-app/Source/ui/Copilot/HarmonicContentBarChart.cpp
#include "HarmonicContentBarChart.h"
#include "../shared/KhensuEmptyState.h"

namespace khensu {

HarmonicContentBarChart::HarmonicContentBarChart()
{
    setTitle(TRANS("Added energy by band"));
    y = ui::chart::scales::relativeDb(kRangeDb);
    setBaseline(0.0);
    valueText = [](size_t, size_t, double v) { return (v >= 0.0 ? "+" : "") + juce::String(v, 1) + " dB"; };
}

void HarmonicContentBarChart::setReport(const juce::String& effectName, std::vector<float> bandsHz,
                                        std::vector<float> addedDb)
{
    effectName_ = effectName;
    bandsHz_ = std::move(bandsHz);
    addedDb_ = std::move(addedDb);
    ui::chart::Axis hz;
    hz.format = ui::chart::Format::Hz;
    std::vector<ui::chart::Bar> bars;
    juce::StringArray parts;
    for (size_t i = 0; i < addedDb_.size(); ++i) {
        ui::chart::Bar b;
        b.label = i < bandsHz_.size() ? hz.label(bandsHz_[i]) : juce::String(static_cast<int>(i) + 1);
        b.values = {addedDb_[i]};
        b.colour = KhensuTheme::vizDiverging(addedDb_[i] >= 0.0f ? 0.85f : -0.85f);
        bars.push_back(b);
        parts.add(b.label + " " + (addedDb_[i] >= 0.0f ? "+" : "") + juce::String(addedDb_[i], 1) + " dB");
    }
    setBars(std::move(bars), {{TRANS("Added"), KhensuTheme::vizDiverging(0.85f)}});
    setSummary(addedDb_.empty() ? TRANS("No saturation stage rendered yet.")
                                : effectName_ + " " + TRANS("added energy by band") + ": " + parts.joinIntoString(", ") + ".");
}

void HarmonicContentBarChart::clear() { setReport({}, {}, {}); }

void HarmonicContentBarChart::paint(juce::Graphics& g)
{
    if (addedDb_.empty()) {
        KhensuEmptyState::paintInto(g, getLocalBounds(), TRANS("No saturation stage rendered yet"), {}); // UXR-6
        return;
    }
    ThemedBarChart::paint(g);
}

} // namespace khensu
