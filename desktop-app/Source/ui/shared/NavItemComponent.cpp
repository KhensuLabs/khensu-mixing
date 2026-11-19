// desktop-app/Source/ui/shared/NavItemComponent.cpp
#include "NavItemComponent.h"
#include "KhensuLookAndFeel.h"

namespace khensu {

NavItemComponent::NavItemComponent()
{
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void NavItemComponent::setLabel(const juce::String& label)
{
    label_ = label;
    setTitle(label);
    setDescription(TRANS("Open this screen."));
    setHelpText(TRANS("Press Return or Space to open this screen."));
    repaint();
}
void NavItemComponent::setActive(bool active)
{
    active_ = active;
    repaint();
}

void NavItemComponent::mouseDown(const juce::MouseEvent& e)
{
    // Left button only, and only when enabled: a right-click (context menu) or
    // a middle-click used to navigate away from the current screen, and a
    // disabled item stayed fully clickable (kimi audit P0 #4).
    if (!isEnabled() || !e.mods.isLeftButtonDown()) return;
    if (onClick) onClick();
}

bool NavItemComponent::keyPressed(const juce::KeyPress& key)
{
    if (!isEnabled()) return false;
    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey) {
        if (onClick) onClick();
        return true;
    }
    return false;
}

std::unique_ptr<juce::AccessibilityHandler> NavItemComponent::createAccessibilityHandler()
{
    auto actions = juce::AccessibilityActions().addAction(juce::AccessibilityActionType::press, [this] {
        if (isEnabled() && onClick) onClick();
    });
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::button, std::move(actions));
}

void NavItemComponent::paint(juce::Graphics& g)
{
    auto b = getLocalBounds();

    // Hover background
    if (hovered_ && !active_) {
        g.setColour(KhensuTheme::bgRaised);
        g.fillRect(b);
    }

    // Active: Aurora Glass pill - a teal->magenta brand-gradient wash with a
    // soft teal halo and a teal(top)->magenta(bottom) left marker. This is the
    // one spot the magenta co-accent touches chrome; it stays low-alpha so the
    // label keeps contrast, and severity colours are untouched by it.
    if (active_) {
        auto rf = b.toFloat().reduced(3.0f, 2.0f);
        constexpr float radius = 7.0f;

        // Soft outer halo (mockup: box-shadow 0 0 24px -8px #22D3C5).
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.10f));
        g.fillRoundedRectangle(rf.expanded(2.0f), radius + 2.0f);

        // Teal -> magenta horizontal wash (stronger so the active row clearly
        // reads as "you are here", not just a faint tint).
        juce::ColourGradient wash(KhensuTheme::accentTeal.withAlpha(0.30f), rf.getX(), rf.getCentreY(),
                                  KhensuTheme::accentMagenta.withAlpha(0.22f), rf.getRight(), rf.getCentreY(), false);
        g.setGradientFill(wash);
        g.fillRoundedRectangle(rf, radius);

        // Accent rim.
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.55f));
        g.drawRoundedRectangle(rf, radius, 1.0f);

        // Bolder 4px left marker: teal (top) -> magenta (bottom).
        juce::ColourGradient edge(KhensuTheme::accentTeal, rf.getX(), rf.getY(), KhensuTheme::accentMagenta, rf.getX(),
                                  rf.getBottom(), false);
        g.setGradientFill(edge);
        g.fillRoundedRectangle(rf.getX(), rf.getY(), 4.0f, rf.getHeight(), 2.0f);
    }

    // Label - the active row is bold as well as teal, so the current screen is
    // legible at a glance even in the High-Contrast theme.
    auto textBounds = b.withLeft(b.getX() + KhensuTheme::sp4);
    g.setFont(active_ ? KhensuTheme::labelFont(13.0f).boldened() : KhensuTheme::labelFont(13.0f));
    const juce::Colour textColour = active_          ? KhensuTheme::accentTeal
                                    : hovered_       ? KhensuTheme::textPrimary
                                                     : KhensuTheme::textSecondary;
    g.setColour(isEnabled() ? textColour : KhensuTheme::textTertiary);
    g.drawText(label_, textBounds, juce::Justification::centredLeft, true);

    // Keyboard-focus ring, matching the button treatment in KhensuLookAndFeel
    // so tab-navigation is visible in the sidebar too.
    if (hasKeyboardFocus(false)) {
        g.setColour(KhensuTheme::focusRing.withAlpha(0.9f));
        g.drawRoundedRectangle(b.toFloat().reduced(2.0f), 7.0f, 1.6f);
    }
}

} // namespace khensu
