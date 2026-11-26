// desktop-app/Source/ui/shared/KhensuErrorState.h
// Shared error-state view for panels whose analyzer failed. Shows a concise
// headline, the error message, and a retry action tied to the panel's own
// retry lambda.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"

namespace khensu {

class KhensuErrorState final : public juce::Component {
  public:
    KhensuErrorState()
    {
        retryButton_.setButtonText(TRANS("Retry"));
        applyThemeColours();
        addAndMakeVisible(retryButton_);
    }

    void setError(const juce::String& headline, const juce::String& detail)
    {
        headline_ = headline;
        detail_ = detail;
        repaint();
    }

    void onRetry(std::function<void()> cb)
    {
        retryButton_.onClick = [cb = std::move(cb)] {
            if (cb) cb();
        };
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(KhensuTheme::bgSurface.withAlpha(0.4f));
        g.fillRoundedRectangle(b, 6.0f);

        g.setColour(KhensuTheme::accentRed.withAlpha(0.12f));
        g.drawRoundedRectangle(b.reduced(2.0f), 6.0f, 1.0f);

        auto inner = getLocalBounds().reduced(20);
        inner.removeFromBottom(40);

        g.setFont(KhensuTheme::metricFont(type::headline));
        g.setColour(KhensuTheme::accentRed);
        g.drawText("!", inner.removeFromTop(inner.getHeight() / 2), juce::Justification::centred, false);

        g.setFont(KhensuTheme::labelFont(type::secondary).boldened());
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(headline_, inner.removeFromTop(20), juce::Justification::centred, false);

        g.setFont(KhensuTheme::bodyFont(type::caption));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(detail_, inner, juce::Justification::centredTop, true);
    }

    void resized() override
    {
        retryButton_.setBounds(getLocalBounds().removeFromBottom(36).withSizeKeepingCentre(100, 28));
    }

  private:
    /// Every cached palette colour on this widget, in one place (UI-01).
    void applyThemeColours()
    {
        retryButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentRed.withAlpha(0.18f));
        retryButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentRed);
    }

    juce::String headline_ = "Analysis failed";
    juce::String detail_ = "Something went wrong.";
    juce::TextButton retryButton_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KhensuErrorState)
};

} // namespace khensu
