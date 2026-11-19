// desktop-app/Source/ui/Copilot/GainStagingWaterfallChart.cpp
#include "GainStagingWaterfallChart.h"
#include "../shared/KhensuLookAndFeel.h"

namespace khensu {

void GainStagingWaterfallChart::paint(juce::Graphics& g)
{
    auto b = getLocalBounds();

    g.setColour(KhensuTheme::textSecondary);
    g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::bold));
    g.drawText("GAIN STAGING", b.removeFromTop(14), juce::Justification::centredLeft, false);

    if (stages_.empty()) {
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.5f));
        g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::italic));
        g.drawText("No render yet", b, juce::Justification::centred, false);
        return;
    }

    const int n = (int)stages_.size();
    const int rowH = juce::jmax(12, b.getHeight() / n);
    const int labelW = 70;

    for (int i = 0; i < n; ++i) {
        const auto& s = stages_[(size_t)i];
        auto row = juce::Rectangle<int>(b.getX(), b.getY() + i * rowH, b.getWidth(), rowH).reduced(0, 1);
        auto labelArea = row.removeFromLeft(labelW);
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(khensu::KhensuTheme::uiFont(8.0f, juce::Font::plain));
        g.drawText(s.label, labelArea, juce::Justification::centredLeft, false);

        auto barArea = row.toFloat();
        auto fracFor = [](float db) { return juce::jlimit(0.0f, 1.0f, (db - kMinDb) / (kMaxDb - kMinDb)); };
        const float fracIn = fracFor(s.inRmsDb);
        const float fracOut = fracFor(s.outRmsDb);

        g.setColour(KhensuTheme::bgSurface.brighter(0.05f));
        g.fillRect(barArea);
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.7f));
        g.fillRect(barArea.withWidth(barArea.getWidth() * fracIn).withHeight(barArea.getHeight() * 0.4f));
        // Amber = stage added level, blue = stage removed it.
        g.setColour((s.outRmsDb >= s.inRmsDb ? KhensuTheme::accentAmber : KhensuTheme::accentBlue).withAlpha(0.85f));
        auto outBar = barArea.withHeight(barArea.getHeight() * 0.4f);
        outBar.setY(barArea.getBottom() - outBar.getHeight());
        g.fillRect(outBar.withWidth(barArea.getWidth() * fracOut));

        g.setColour(KhensuTheme::textPrimary.withAlpha(0.8f));
        g.setFont(khensu::KhensuTheme::uiFont(7.5f, juce::Font::plain));
        const juce::String delta = juce::String(s.outRmsDb - s.inRmsDb, 1) + " dB";
        g.drawText(delta, barArea, juce::Justification::centredRight, false);
    }
}

} // namespace khensu
