// desktop-app/Source/ui/shared/ChainReasoningOverlay.cpp
#include "ChainReasoningOverlay.h"
#include "KhensuLookAndFeel.h"

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void ChainReasoningOverlay::applyThemeColours()
{
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
}

ChainReasoningOverlay::ChainReasoningOverlay(const Recommendation& rec)
{
    applyThemeColours();
    addAndMakeVisible(card_);

    titleLabel_.setText(juce::String(rec.what), juce::dontSendNotification);
    titleLabel_.setFont(khensu::KhensuTheme::uiFont(15.0f, juce::Font::bold));
    titleLabel_.setMinimumHorizontalScale(0.6f);
    card_.addAndMakeVisible(titleLabel_);

    closeButton_.setButtonText(TRANS("Close"));
    closeButton_.onClick = [this]() {
        if (onDismiss) onDismiss();
    };
    card_.addAndMakeVisible(closeButton_);

    reasoningView_.setRecommendation(rec);
    reasoningView_.onHeightChanged = [this]() {
        reasoningView_.setSize(viewport_.getWidth(), reasoningView_.getContentHeight());
    };
    viewport_.setViewedComponent(&reasoningView_, false);
    viewport_.setScrollBarsShown(true, false);
    card_.addAndMakeVisible(viewport_);

    grabModalFocus();
}

void ChainReasoningOverlay::resized()
{
    auto bounds = getLocalBounds();
    auto cardW = juce::jmin(560, bounds.getWidth() - 40);
    auto cardH = juce::jmin(480, bounds.getHeight() - 40);
    card_.setBounds((bounds.getWidth() - cardW) / 2, (bounds.getHeight() - cardH) / 2, cardW, cardH);

    auto b = card_.getLocalBounds().reduced(16);
    auto header = b.removeFromTop(28);
    closeButton_.setBounds(header.removeFromRight(70));
    titleLabel_.setBounds(header);
    b.removeFromTop(10);

    viewport_.setBounds(b);
    reasoningView_.setSize(viewport_.getWidth() - viewport_.getScrollBarThickness(), reasoningView_.getContentHeight());
}

} // namespace khensu
