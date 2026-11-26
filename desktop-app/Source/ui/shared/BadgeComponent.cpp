// desktop-app/Source/ui/shared/BadgeComponent.cpp
#include "BadgeComponent.h"
#include "KhensuTheme.h"

namespace khensu {

void BadgeComponent::setBadge(const juce::String& text, BadgeStatus status)
{
    text_ = text;
    status_ = status;
    repaint();
}

void BadgeComponent::paint(juce::Graphics& g)
{
    // UI-3 / LG-8: full liquid-glass pill capsule; status lives in the luminous dot.
    auto b = getLocalBounds().toFloat().reduced(1.0f);
    const float pillR = b.getHeight() * 0.5f;
    g.setColour(KhensuTheme::bgRaised.withAlpha(0.75f));
    g.fillRoundedRectangle(b, pillR);
    juce::ColourGradient rim(KhensuTheme::borderActive.brighter(0.25f), b.getTopLeft(), KhensuTheme::borderSubtle,
                             b.getBottomRight(), false);
    g.setGradientFill(rim);
    g.drawRoundedRectangle(b, pillR, 1.0f);

    constexpr float dotR = 3.0f;
    const float dotX = b.getX() + KhensuTheme::sp3;
    const float dotY = b.getCentreY();
    const auto dotCol =
        status_ == BadgeStatus::Neutral ? KhensuTheme::textTertiary : KhensuTheme::colourForStatus(status_);
    if (status_ != BadgeStatus::Neutral) {
        g.setColour(dotCol.withAlpha(0.28f));
        g.fillEllipse(dotX - dotR - 2.0f, dotY - dotR - 2.0f, (dotR + 2.0f) * 2.0f, (dotR + 2.0f) * 2.0f);
    }
    g.setColour(dotCol);
    g.fillEllipse(dotX - dotR, dotY - dotR, dotR * 2.0f, dotR * 2.0f);

    auto textBounds = b.withLeft(dotX + dotR + KhensuTheme::sp2).withRight(b.getRight() - KhensuTheme::sp2);
    g.setFont(KhensuTheme::numericFont(KhensuTheme::kFontLabel + 1.0f));
    g.setColour(status_ == BadgeStatus::Neutral ? KhensuTheme::textSecondary : KhensuTheme::textPrimary);
    g.drawText(text_, textBounds.toNearestInt(), juce::Justification::centredLeft, true);
}

} // namespace khensu
