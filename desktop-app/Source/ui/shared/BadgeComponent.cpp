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
    auto b = getLocalBounds().toFloat().reduced(1.0f);
    auto colour = KhensuTheme::colourForStatus(status_);
    auto dimCol = KhensuTheme::dimColourForStatus(status_);

    // Pill fill
    g.setColour(dimCol);
    g.fillRoundedRectangle(b, b.getHeight() * 0.5f);

    // Pill border
    g.setColour(colour.withAlpha(0.5f));
    g.drawRoundedRectangle(b, b.getHeight() * 0.5f, 1.0f);

    // Status dot
    constexpr float dotR = 4.0f;
    float dotX = b.getX() + KhensuTheme::sp2;
    float dotY = b.getCentreY();
    g.setColour(colour);
    g.fillEllipse(dotX - dotR, dotY - dotR, dotR * 2.0f, dotR * 2.0f);

    // Label text
    auto textBounds = b.withLeft(dotX + dotR + KhensuTheme::sp1).withRight(b.getRight() - KhensuTheme::sp2);
    g.setFont(KhensuTheme::labelFont(10.0f));
    g.setColour(colour);
    g.drawText(text_.toUpperCase(), textBounds.toNearestInt(), juce::Justification::centredLeft, true);
}

} // namespace khensu
