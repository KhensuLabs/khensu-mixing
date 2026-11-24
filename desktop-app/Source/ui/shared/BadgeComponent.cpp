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
    // UI-3: a neutral pill - the status lives in the small dot, never in a
    // tinted fill or coloured text (colour means severity, not decoration).
    auto b = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(KhensuTheme::bgRaised);
    g.fillRoundedRectangle(b, 6.0f);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRoundedRectangle(b, 6.0f, 1.0f);

    constexpr float dotR = 3.0f;
    const float dotX = b.getX() + KhensuTheme::sp3;
    const float dotY = b.getCentreY();
    g.setColour(status_ == BadgeStatus::Neutral ? KhensuTheme::textTertiary : KhensuTheme::colourForStatus(status_));
    g.fillEllipse(dotX - dotR, dotY - dotR, dotR * 2.0f, dotR * 2.0f);

    auto textBounds = b.withLeft(dotX + dotR + KhensuTheme::sp2).withRight(b.getRight() - KhensuTheme::sp2);
    g.setFont(KhensuTheme::numericFont(KhensuTheme::kFontLabel + 1.0f));
    g.setColour(status_ == BadgeStatus::Neutral ? KhensuTheme::textSecondary : KhensuTheme::textPrimary);
    g.drawText(text_, textBounds.toNearestInt(), juce::Justification::centredLeft, true);
}

} // namespace khensu
