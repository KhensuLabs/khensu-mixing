// shared-core/include/khensu/ui/ExperimentalTag.h
// VER-8: the "Experimental" badge for surfaces that cannot use the desktop's
// ExperimentalBadge (the plugin links no KhensuTheme): the same outlined
// label, in the shared palette, whose tooltip names the real-world check the
// feature still lacks (docs/experimental-features.md).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <khensu/ui/KhensuPalette.h>

namespace khensu::ui {

class ExperimentalTag final : public juce::Component, public juce::SettableTooltipClient {
  public:
    ExperimentalTag()
    {
        setTitle("Experimental");
        setInterceptsMouseClicks(true, false); // for the tooltip
    }

    void setOutstandingCheck(const juce::String& check)
    {
        setDescription(check);
        setTooltip("Experimental: built, but not yet checked in real use. " + check);
    }

    static juce::Font font() { return Palette::uiFont(10.0f); }
    static int preferredWidth() { return juce::GlyphArrangement::getStringWidthInt(font(), "Experimental") + 14; }
    static constexpr int kHeight = 16;

    void paint(juce::Graphics& g) override
    {
        // Neutral, not a severity colour: experimental says nothing about the mix.
        const auto r = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(Palette::textMuted.withAlpha(0.8f));
        g.drawRoundedRectangle(r, r.getHeight() * 0.5f, 1.0f);
        g.setFont(font());
        g.setColour(Palette::textMuted);
        g.drawText("Experimental", getLocalBounds(), juce::Justification::centred, false);
    }
};

} // namespace khensu::ui
