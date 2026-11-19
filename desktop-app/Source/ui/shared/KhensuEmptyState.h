// desktop-app/Source/ui/shared/KhensuEmptyState.h
// Shared empty-state view for panels that have no data yet (no session, no
// analysis, or analyzer returned nothing usable). Gives a neutral icon,
// headline, and optional action button.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"

namespace khensu {

class KhensuEmptyState final : public juce::Component {
  public:
    KhensuEmptyState()
    {
        applyThemeColours();
        // Hidden until setAction(): addAndMakeVisible() here used to re-show it,
        // leaving a blank, unnamed button on every action-less empty state.
        addChildComponent(actionButton_);
    }

    void setMessage(const juce::String& headline, const juce::String& body)
    {
        headline_ = headline;
        body_ = body;
        repaint();
    }

    void setAction(const juce::String& label, std::function<void()> onClick)
    {
        actionButton_.setButtonText(label);
        actionButton_.setTitle(label);
        actionButton_.onClick = [cb = std::move(onClick)] {
            if (cb) cb();
        };
        actionButton_.setVisible(true);
        resized();
        repaint();
    }

    void clearAction()
    {
        actionButton_.setVisible(false);
        actionButton_.onClick = nullptr;
        resized();
        repaint();
    }

    /// Shared empty-state visual, reusable without a Component instance so
    /// panels can render a consistent empty state inline (PanelCard::drawEmptyState).
    /// `reserveActionRow` leaves room at the bottom for an action button.
    static void paintInto(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& headline,
                          const juce::String& body, bool reserveActionRow = false)
    {
        g.setColour(KhensuTheme::bgSurface.withAlpha(0.4f));
        g.fillRoundedRectangle(bounds.toFloat(), 6.0f);

        auto inner = bounds.reduced(20);
        if (reserveActionRow) inner.removeFromBottom(40);

        g.setFont(KhensuTheme::metricFont(28.0f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("-", inner.removeFromTop(juce::jmax(1, inner.getHeight() / 2)), juce::Justification::centred, false);

        g.setFont(KhensuTheme::labelFont(12.0f).boldened());
        g.setColour(KhensuTheme::textSecondary);
        // Ellipsize: headlines are caller-supplied and a long one was silently
        // clipped mid-word in a narrow panel (kimi audit P1 #18).
        g.drawText(headline, inner.removeFromTop(20), juce::Justification::centred, true);

        g.setFont(KhensuTheme::bodyFont(11.0f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(body, inner, juce::Justification::centredTop, true);
    }

    void paint(juce::Graphics& g) override
    {
        paintInto(g, getLocalBounds(), headline_, body_, actionButton_.isVisible());
    }

    void resized() override
    {
        if (actionButton_.isVisible()) {
            actionButton_.setBounds(getLocalBounds().removeFromBottom(36).withSizeKeepingCentre(120, 28));
        }
    }

  private:
    void applyThemeColours()
    {
        actionButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.18f));
        actionButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }

    juce::String headline_ = "No data";
    juce::String body_ = "Analysis results will appear here.";
    juce::TextButton actionButton_;

    // Cached colours - re-applied on theme change (kimi audit P1 #18/#19).
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KhensuEmptyState)
};

} // namespace khensu
