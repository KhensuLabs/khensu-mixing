// plugin-copilot/Source/ui/HookIndicatorPanel.cpp
#include "HookIndicatorPanel.h"
#include <khensu/ui/KhensuPalette.h>
#include "PluginLookAndFeel.h"

namespace khensu {

HookIndicatorPanel::HookIndicatorPanel() {}

void HookIndicatorPanel::setData(float hookStrength, int hookCount, float hookDensity) noexcept
{
    hookStrength_ = hookStrength;
    hookCount_ = hookCount;
    hookDensity_ = hookDensity;
    hasData_ = true;
    repaint();
}

void HookIndicatorPanel::clearData() noexcept
{
    hasData_ = false;
    repaint();
}

void HookIndicatorPanel::drawBadge(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& label,
                                   const juce::String& value, juce::Colour accent)
{
    g.setColour(PluginLookAndFeel::bgPanel);
    g.fillRoundedRectangle(bounds.toFloat(), 4.0f);
    g.setColour(accent.withAlpha(0.4f));
    g.drawRoundedRectangle(bounds.toFloat(), 4.0f, 1.0f);

    auto top = bounds.removeFromTop(bounds.getHeight() / 2);
    g.setColour(PluginLookAndFeel::textMuted);
    g.setFont(khensu::ui::Palette::uiFont(9.0f, juce::Font::plain));
    g.drawText(label, top, juce::Justification::centred);

    g.setColour(accent);
    g.setFont(khensu::ui::Palette::uiFont(13.0f, juce::Font::bold));
    g.drawText(value, bounds, juce::Justification::centred);
}

void HookIndicatorPanel::paint(juce::Graphics& g)
{
    g.setColour(PluginLookAndFeel::bgPanel);
    g.fillRect(getLocalBounds());

    auto b = getLocalBounds().reduced(8, 4);

    // Title
    auto titleArea = b.removeFromTop(16);
    g.setColour(PluginLookAndFeel::accentTeal);
    g.setFont(khensu::ui::Palette::uiFont(10.0f, juce::Font::bold));
    g.drawText("HOOK INTELLIGENCE", titleArea, juce::Justification::centredLeft);
    b.removeFromTop(4);

    if (!hasData_) {
        g.setColour(PluginLookAndFeel::textMuted);
        g.setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::plain));
        g.drawText("Load analysis to see hook data", b, juce::Justification::centred);
        return;
    }

    // Strength bar (left ~50%)
    auto left = b.removeFromLeft(b.getWidth() / 2 - 4);
    b.removeFromLeft(8);
    auto right = b;

    auto barLabel = left.removeFromTop(14);
    g.setColour(PluginLookAndFeel::textMuted);
    g.setFont(khensu::ui::Palette::uiFont(9.0f, juce::Font::plain));
    g.drawText("STRENGTH", barLabel, juce::Justification::centredLeft);

    auto barBg = left;
    g.setColour(PluginLookAndFeel::bgPanel);
    g.fillRoundedRectangle(barBg.toFloat(), 3.0f);
    const float frac = juce::jlimit(0.0f, 1.0f, hookStrength_);
    if (frac > 0.0f) {
        auto fill = barBg.withWidth(static_cast<int>(frac * barBg.getWidth()));
        const juce::Colour barCol = frac >= 0.75f   ? PluginLookAndFeel::statusGood
                                    : frac >= 0.45f ? PluginLookAndFeel::statusWarn
                                                    : PluginLookAndFeel::statusBad;
        g.setColour(barCol);
        g.fillRoundedRectangle(fill.toFloat(), 3.0f);
    }
    g.setColour(PluginLookAndFeel::controlOutline);
    g.drawRoundedRectangle(barBg.toFloat(), 3.0f, 1.0f);

    // Strength percentage below bar
    g.setColour(PluginLookAndFeel::textPrimary);
    g.setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::bold));
    // (no extra space - barBg already consumed)

    // Badges (right side)
    const int badgeH = right.getHeight() / 2 - 2;
    auto countBounds = right.removeFromTop(badgeH);
    right.removeFromTop(4);
    auto densityBounds = right.removeFromTop(badgeH);

    drawBadge(g, countBounds, "HOOKS", juce::String(hookCount_), PluginLookAndFeel::accentTeal);
    drawBadge(g, densityBounds, "DENSITY", juce::String(static_cast<int>(hookDensity_ * 100)) + "%",
              PluginLookAndFeel::accentBlue);
}

void HookIndicatorPanel::resized() {}

} // namespace khensu
