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
void NavItemComponent::setIcon(nav_icons::Icon icon)
{
    icon_ = icon;
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
    // The row's own surface, inset so neighbouring rows don't touch (UX2-4).
    const auto row = b.toFloat().reduced(static_cast<float>(KhensuTheme::sp2), 2.0f);
    const float radius = juce::jmin(10.0f, row.getHeight() * 0.42f);

    // LG-9: frosted hover tint + interactive cursor-tracking sheen.
    if (hovered_ && isEnabled()) {
        if (!active_) {
            g.setColour(KhensuTheme::textPrimary.withAlpha(0.055f));
            g.fillRoundedRectangle(row, radius);
        }
        KhensuLookAndFeel::drawCursorSheen(g, row, radius, cursorPos_, active_ ? 0.65f : 0.9f);
    }

    // LG-11: when parented inside SidebarComponent, the sidebar paints the
    // spring-animated sliding liquid-glass capsule behind us. When standalone
    // (e.g. unit tests), draw the liquid capsule directly so it stays complete.
    if (active_ && getParentComponent() == nullptr) {
        juce::ColourGradient capsuleFill(KhensuTheme::accentTeal.withAlpha(0.18f), row.getX(), row.getCentreY(),
                                         KhensuTheme::bgOverlay.withAlpha(0.75f), row.getRight(), row.getCentreY(),
                                         false);
        g.setGradientFill(capsuleFill);
        g.fillRoundedRectangle(row, radius);
        g.setColour(KhensuTheme::accentTeal);
        g.fillRoundedRectangle(1.0f, row.getY() + 6.0f, 3.0f, row.getHeight() - 12.0f, 1.5f);
    }

    const juce::Colour textColour = !isEnabled() ? KhensuTheme::textTertiary
                                    : active_    ? KhensuTheme::textPrimary
                                    : hovered_   ? KhensuTheme::textPrimary
                                                 : KhensuTheme::textSecondary;

    auto content = b.withTrimmedLeft(KhensuTheme::sp2 + KhensuTheme::sp3);
    if (icon_ != nav_icons::Icon::None) {
        constexpr float kIconPx = 18.0f;
        const auto iconArea = content.removeFromLeft(juce::roundToInt(kIconPx)).toFloat()
                                  .withSizeKeepingCentre(kIconPx, kIconPx);
        nav_icons::draw(g, icon_, iconArea, active_ && isEnabled() ? KhensuTheme::accentTeal : textColour);
        content.removeFromLeft(KhensuTheme::sp3);
    }

    g.setFont(active_ ? KhensuTheme::labelFont(KhensuTheme::kFontBody).boldened()
                      : KhensuTheme::labelFont(KhensuTheme::kFontBody));
    g.setColour(textColour);
    g.drawText(label_, content, juce::Justification::centredLeft, true);

    // Keyboard-focus ring, matching the button treatment in KhensuLookAndFeel
    // so tab-navigation is visible in the sidebar too.
    if (hasKeyboardFocus(false)) {
        g.setColour(KhensuTheme::focusRing.withAlpha(0.9f));
        g.drawRoundedRectangle(row.expanded(1.0f), radius + 1.0f, 1.6f);
    }
}

} // namespace khensu
