// desktop-app/Source/ui/shared/ExperimentalBadge.h
// VER-8: the "Experimental" badge - a small outlined label whose tooltip names
// the real-world check a feature still lacks (ui/shared/FeatureMaturity.h).
// PanelCard shows one beside its title; any other surface places one beside
// its own title with ExperimentalBadge::forFeature().
#pragma once

#include <JuceHeader.h>

#include "FeatureMaturity.h"
#include "KhensuTheme.h"

namespace khensu {

class ExperimentalBadge final : public juce::Component, public juce::SettableTooltipClient {
  public:
    ExperimentalBadge()
    {
        setTitle(TRANS("Experimental"));
        setInterceptsMouseClicks(true, false); // for the tooltip
    }

    /// Names the outstanding check in the tooltip and the accessible description.
    void setOutstandingCheck(const juce::String& check)
    {
        setDescription(check);
        setTooltip(TRANS("Experimental: built, but not yet checked on real audio.") + " " + check);
    }

    /// Configures the badge from a FeatureMaturity id; false (and hidden)
    /// when the id is not experimental.
    bool forFeature(std::string_view id)
    {
        const auto* f = maturity::findExperimentalFeature(id);
        if (f != nullptr) setOutstandingCheck(juce::translate(f->outstandingCheck));
        setVisible(f != nullptr);
        return f != nullptr;
    }

    static juce::Font font() { return KhensuTheme::labelFont(10.0f); }
    static juce::String text() { return TRANS("Experimental"); }
    static int preferredWidth() { return juce::GlyphArrangement::getStringWidthInt(font(), text()) + 14; }
    static constexpr int kHeight = 16;

    void paint(juce::Graphics& g) override
    {
        // Neutral, not a severity colour: experimental says nothing about the mix.
        const auto r = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.7f));
        g.drawRoundedRectangle(r, r.getHeight() * 0.5f, 1.0f);
        g.setFont(font());
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(text(), getLocalBounds(), juce::Justification::centred, false);
    }

    /// Places the badge at the right end of `row`, vertically centred, and
    /// returns what is left of the row.
    juce::Rectangle<int> placeAtRight(juce::Rectangle<int> row, int gap = 6)
    {
        if (!isVisible()) return row;
        auto slot = row.removeFromRight(preferredWidth());
        setBounds(slot.withSizeKeepingCentre(slot.getWidth(), kHeight));
        row.removeFromRight(gap);
        return row;
    }
};

} // namespace khensu
