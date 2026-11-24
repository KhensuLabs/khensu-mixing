#include "PluginLookAndFeel.h"
#include <khensu/ui/KhensuPalette.h>
#include "ButtonRoles.h"

namespace khensu {

// Literal values, not copies of ui::Palette's statics: those live in another
// translation unit, and C++ leaves cross-TU static initialisation order
// unspecified - a Debug build initialised these first and read Palette as
// transparent black (Release only worked because the optimiser folded the
// copies). tests/plugin/PluginPaletteTest.cpp keeps the two in sync.
const juce::Colour PluginLookAndFeel::bgDark(0xff101113);
const juce::Colour PluginLookAndFeel::bgPanel(0xff16181b);
const juce::Colour PluginLookAndFeel::accentTeal(0xff22d3c5);
const juce::Colour PluginLookAndFeel::textPrimary(0xffe8eaed);
const juce::Colour PluginLookAndFeel::textMuted(0xff9aa1a9);
const juce::Colour PluginLookAndFeel::statusWarn(0xfff5a623);
const juce::Colour PluginLookAndFeel::statusBad(0xffff4d6a);
const juce::Colour PluginLookAndFeel::statusGood(0xff34d399);
const juce::Colour PluginLookAndFeel::accentBlue(0xff4da3ff);
const juce::Colour PluginLookAndFeel::controlOutline = PluginLookAndFeel::bgPanel.brighter(0.18f);

PluginLookAndFeel::PluginLookAndFeel()
{
    setColour(juce::Slider::thumbColourId, accentTeal);
    setColour(juce::Slider::rotarySliderFillColourId, accentTeal);
    setColour(juce::Slider::rotarySliderOutlineColourId, controlOutline);
    setColour(juce::ToggleButton::tickColourId, accentTeal);
}

void PluginLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                         float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                                         juce::Slider&)
{
    const auto bounds = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height).reduced(4.0f);
    const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    // Track
    g.setColour(controlOutline);
    juce::Path track;
    track.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Value arc
    g.setColour(accentTeal);
    juce::Path valueArc;
    valueArc.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, rotaryStartAngle, angle, true);
    g.strokePath(valueArc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Pointer
    juce::Path pointer;
    const float pointerLength = radius * 0.7f;
    pointer.addRectangle(-1.5f, -pointerLength, 3.0f, pointerLength);
    pointer.applyTransform(juce::AffineTransform::rotation(angle).translated(centre));
    g.setColour(textPrimary);
    g.fillPath(pointer);
}

PluginEditorLookAndFeel::PluginEditorLookAndFeel()
{
    using P = PluginLookAndFeel;
    setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    setColour(juce::TextButton::buttonOnColourId, P::accentTeal.withAlpha(0.22f));
    setColour(juce::TextButton::textColourOffId, P::textPrimary);
    setColour(juce::TextButton::textColourOnId, P::accentTeal);
    setColour(juce::ComboBox::outlineColourId, P::controlOutline);
    setColour(juce::Label::textColourId, P::textPrimary);
    setColour(juce::PopupMenu::backgroundColourId, P::bgPanel);
    setColour(juce::PopupMenu::textColourId, P::textPrimary);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, P::accentTeal.withAlpha(0.22f));
    setColour(juce::PopupMenu::highlightedTextColourId, P::textPrimary);
    setColour(juce::PopupMenu::headerTextColourId, P::textMuted);
    setColour(juce::TabbedComponent::backgroundColourId, P::bgDark);
    setColour(juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TabbedButtonBar::tabOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TabbedButtonBar::frontOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TabbedButtonBar::tabTextColourId, P::textMuted);
    setColour(juce::TabbedButtonBar::frontTextColourId, P::textPrimary);
    setColour(juce::TooltipWindow::backgroundColourId, P::bgPanel.brighter(0.1f));
    setColour(juce::TooltipWindow::textColourId, P::textPrimary);
    setColour(juce::TooltipWindow::outlineColourId, P::controlOutline);
    setColour(juce::AlertWindow::backgroundColourId, P::bgPanel);
    setColour(juce::AlertWindow::textColourId, P::textPrimary);
    setColour(juce::TextEditor::backgroundColourId, P::bgDark);
    setColour(juce::TextEditor::textColourId, P::textPrimary);
    setColour(juce::TextEditor::outlineColourId, P::controlOutline);
    setColour(juce::TextEditor::focusedOutlineColourId, P::accentTeal);
}

juce::Font PluginEditorLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    // 13 px body text, never below the 12 px floor (UX-4).
    return khensu::ui::Palette::uiFont(juce::jlimit(12.0f, 13.0f, (float)buttonHeight * 0.5f));
}

void PluginEditorLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                   const juce::Colour& backgroundColour, bool highlighted, bool down)
{
    using P = PluginLookAndFeel;
    const auto r = button.getLocalBounds().toFloat().reduced(0.5f);
    const float radius = juce::jmin(5.0f, r.getHeight() * 0.25f);
    const bool primary = copilot::button_role::isPrimary(button);
    const bool on = button.getToggleState();
    if (primary) {
        auto fill = button.isEnabled() ? P::accentTeal : P::controlOutline;
        if (down)
            fill = fill.darker(0.2f);
        else if (highlighted)
            fill = fill.brighter(0.1f);
        g.setColour(fill);
        g.fillRoundedRectangle(r, radius);
        if (button.hasKeyboardFocus(true)) {
            g.setColour(P::textPrimary.withAlpha(0.8f));
            g.drawRoundedRectangle(r.reduced(1.5f), radius, 1.0f);
        }
        return;
    }
    // Secondary: transparent with a hairline; the toggled state is filled
    // with the colour the owner set (buttonOnColourId) or the accent tint.
    auto fill = on ? button.findColour(juce::TextButton::buttonOnColourId) : backgroundColour;
    if (highlighted && button.isEnabled())
        fill = fill.isTransparent() ? P::textPrimary.withAlpha(0.06f) : fill.brighter(0.08f);
    if (down) fill = fill.isTransparent() ? P::textPrimary.withAlpha(0.10f) : fill.darker(0.1f);
    g.setColour(fill);
    g.fillRoundedRectangle(r, radius);
    g.setColour(on ? P::accentTeal.withAlpha(0.55f) : P::textPrimary.withAlpha(button.isEnabled() ? 0.16f : 0.08f));
    g.drawRoundedRectangle(r, radius, 1.0f);
    if (button.hasKeyboardFocus(true)) {
        g.setColour(P::accentTeal.withAlpha(0.7f));
        g.drawRoundedRectangle(r.reduced(1.5f), radius, 1.0f);
    }
}

void PluginEditorLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool)
{
    using P = PluginLookAndFeel;
    const bool primary = copilot::button_role::isPrimary(button);
    auto colour = primary ? P::bgDark
                          : button.findColour(button.getToggleState() ? juce::TextButton::textColourOnId
                                                                      : juce::TextButton::textColourOffId);
    if (!button.isEnabled()) colour = colour.withMultipliedAlpha(0.45f);
    g.setColour(colour);
    auto font = getTextButtonFont(button, button.getHeight());
    if (primary) font = font.boldened();
    g.setFont(font);
    g.drawFittedText(button.getButtonText(), button.getLocalBounds().reduced(6, 0), juce::Justification::centred, 1,
                     0.9f);
}

int PluginEditorLookAndFeel::getTabButtonBestWidth(juce::TabBarButton& button, int)
{
    const auto font = khensu::ui::Palette::uiFont(13.0f, juce::Font::bold);
    return juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, button.getButtonText())) + 32;
}

void PluginEditorLookAndFeel::drawTabButton(juce::TabBarButton& button, juce::Graphics& g, bool isMouseOver, bool)
{
    using P = PluginLookAndFeel;
    const auto r = button.getLocalBounds().toFloat();
    const bool front = button.isFrontTab();
    if (isMouseOver && !front) {
        g.setColour(P::textPrimary.withAlpha(0.04f));
        g.fillRoundedRectangle(r.reduced(2.0f, 4.0f), 4.0f);
    }
    g.setColour(front ? P::textPrimary : (isMouseOver ? P::textPrimary.withAlpha(0.85f) : P::textMuted));
    g.setFont(khensu::ui::Palette::uiFont(13.0f, front ? juce::Font::bold : juce::Font::plain));
    g.drawText(button.getButtonText(), r, juce::Justification::centred, false);
    if (front) {
        // One accent: a 2 px underline under the selected tab.
        g.setColour(P::accentTeal);
        g.fillRoundedRectangle(r.getX() + 10.0f, r.getBottom() - 2.0f, r.getWidth() - 20.0f, 2.0f, 1.0f);
    }
    if (button.hasKeyboardFocus(false)) {
        g.setColour(P::accentTeal.withAlpha(0.6f));
        g.drawRoundedRectangle(r.reduced(2.0f, 4.0f), 4.0f, 1.0f);
    }
}

void PluginEditorLookAndFeel::drawTabbedButtonBarBackground(juce::TabbedButtonBar&, juce::Graphics&) {}

void PluginEditorLookAndFeel::drawTabAreaBehindFrontButton(juce::TabbedButtonBar&, juce::Graphics& g, int w, int h)
{
    g.setColour(PluginLookAndFeel::textPrimary.withAlpha(0.08f));
    g.fillRect(0, h - 1, w, 1);
}

juce::Font PluginEditorLookAndFeel::getPopupMenuFont()
{
    return khensu::ui::Palette::uiFont(13.0f);
}

} // namespace khensu
