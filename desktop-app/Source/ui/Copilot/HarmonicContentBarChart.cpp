// desktop-app/Source/ui/Copilot/HarmonicContentBarChart.cpp
#include "HarmonicContentBarChart.h"
#include "../shared/KhensuLookAndFeel.h"

namespace khensu {

void HarmonicContentBarChart::setReport(const juce::String& effectName, std::vector<float> bandsHz,
                                        std::vector<float> addedDb)
{
    effectName_ = effectName;
    bandsHz_ = std::move(bandsHz);
    addedDb_ = std::move(addedDb);
    repaint();
}

void HarmonicContentBarChart::clear()
{
    effectName_.clear();
    bandsHz_.clear();
    addedDb_.clear();
    repaint();
}

void HarmonicContentBarChart::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();

    if (addedDb_.empty()) {
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.5f));
        g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::italic));
        g.drawText("No saturation stage rendered yet", area, juce::Justification::centred, false);
        return;
    }

    g.setColour(KhensuTheme::textSecondary);
    g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::bold));
    g.drawText(effectName_ + " -- added energy by band", area.removeFromTop(14.0f), juce::Justification::centredLeft,
               false);

    const float midY = area.getY() + area.getHeight() * 0.5f;
    g.setColour(KhensuTheme::textSecondary.withAlpha(0.25f));
    g.drawHorizontalLine(juce::roundToInt(midY), area.getX(), area.getRight());

    const int n = (int)addedDb_.size();
    if (n == 0) return;
    const float barW = area.getWidth() / (float)n;
    const float halfH = area.getHeight() * 0.5f - 12.0f;

    for (int i = 0; i < n; ++i) {
        const float db = addedDb_[(size_t)i];
        const float frac = juce::jlimit(-1.0f, 1.0f, db / kRangeDb);
        const float barH = std::abs(frac) * halfH;
        const float x = area.getX() + barW * (float)i + barW * 0.15f;
        const float w = barW * 0.7f;
        const float y = frac >= 0.0f ? midY - barH : midY;

        const juce::Colour col = frac >= 0.0f ? KhensuTheme::accentAmber : KhensuTheme::accentBlue;
        g.setColour(col.withAlpha(0.85f));
        g.fillRect(x, y, w, barH);

        if (i < (int)bandsHz_.size()) {
            const float hz = bandsHz_[(size_t)i];
            juce::String label = hz >= 1000.0f ? juce::String(hz / 1000.0f, 1) + "k" : juce::String((int)hz);
            g.setColour(KhensuTheme::textSecondary.withAlpha(0.7f));
            g.setFont(khensu::KhensuTheme::uiFont(8.0f, juce::Font::plain));
            g.drawText(label, (int)(area.getX() + barW * (float)i), (int)(area.getBottom() - 12.0f), (int)barW, 12,
                       juce::Justification::centred, false);
        }
    }
}

} // namespace khensu
