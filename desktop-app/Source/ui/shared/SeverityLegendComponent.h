// desktop-app/Source/ui/shared/SeverityLegendComponent.h
// Compact one-line legend for the mix-note colour scheme (spec §4):
// red = severe, yellow = moderate, blue = minor, green = strength.
// Dropped into the Mix Notes panel (Analysis page) and the Mix/Master
// screen so the colours are never left to guesswork.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"

namespace khensu {

class SeverityLegendComponent final : public juce::Component {
  public:
    static constexpr int kHeight = 18;

    SeverityLegendComponent() { setInterceptsMouseClicks(false, false); }

    void paint(juce::Graphics& g) override
    {
        struct Entry {
            juce::Colour colour;
            const char* label;
        };
        const Entry entries[] = {
            {KhensuTheme::accentRed, "severe"},
            {KhensuTheme::accentAmber, "moderate"},
            {KhensuTheme::accentBlue, "minor"},
            {KhensuTheme::accentGreen, "strength"},
        };

        const auto font = KhensuTheme::labelFont(type::caption);
        g.setFont(font);
        int x = 0;
        const int dotSize = 7;
        const int cy = getHeight() / 2;
        for (const auto& e : entries) {
            g.setColour(e.colour);
            g.fillEllipse((float)x, (float)(cy - dotSize / 2), (float)dotSize, (float)dotSize);
            x += dotSize + 4;
            const int labelW = 8 + juce::GlyphArrangement::getStringWidthInt(font, e.label);
            g.setColour(KhensuTheme::textSecondary);
            g.drawText(e.label, x, 0, labelW, getHeight(), juce::Justification::centredLeft, false);
            x += labelW + 10;
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SeverityLegendComponent)
};

} // namespace khensu
