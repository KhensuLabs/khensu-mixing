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
        // A screen reader reads the empty state like the label it replaces (UXR-6).
        setTitle(headline);
        setDescription(body);
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
        // UXR-6: a short host (a chart strip, a sidebar chart) keeps the text
        // and drops the glyph row, instead of losing the text to the padding.
        if (inner.getHeight() < 60) {
            inner = bounds.reduced(8, 4);
            const int textH = body.isEmpty() ? 20 : 36; // headline row (+ one body line)
            inner.removeFromTop(juce::jmax(0, (inner.getHeight() - textH) / 2));
        } else {
            g.setFont(KhensuTheme::metricFont(type::display));
            g.setColour(KhensuTheme::textTertiary);
            g.drawText("-", inner.removeFromTop(juce::jmax(1, inner.getHeight() / 2)), juce::Justification::centred,
                       false);
        }

        g.setFont(KhensuTheme::labelFont(type::secondary).boldened());
        g.setColour(KhensuTheme::textSecondary);
        // Ellipsize: headlines are caller-supplied and a long one was silently
        // clipped mid-word in a narrow panel (kimi audit P1 #18).
        g.drawText(headline, inner.removeFromTop(20), juce::Justification::centred, true);

        g.setFont(KhensuTheme::bodyFont(type::caption));
        g.setColour(KhensuTheme::textTertiary);
        // Wraps (up to 3 lines) so a sentence-long body is not cut to one line (UXR-6).
        g.drawFittedText(body, inner, juce::Justification::centredTop, 3, 1.0f);
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
