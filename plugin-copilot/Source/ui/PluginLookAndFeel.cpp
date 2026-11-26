#include "PluginLookAndFeel.h"
#include <khensu/ui/KhensuPalette.h>
#include "ButtonRoles.h"

namespace khensu {

// Literal values, not copies of ui::Palette's statics: those live in another
// translation unit, and C++ leaves cross-TU static initialisation order
// unspecified - a Debug build initialised these first and read Palette as
// transparent black (Release only worked because the optimiser folded the
// copies). tests/plugin/PluginPaletteTest.cpp keeps the two in sync.
juce::Colour PluginLookAndFeel::bgDark(0xff101113);
juce::Colour PluginLookAndFeel::bgPanel(0xff16181b);
juce::Colour PluginLookAndFeel::accentTeal(0xff22d3c5);
juce::Colour PluginLookAndFeel::textPrimary(0xffe8eaed);
juce::Colour PluginLookAndFeel::textMuted(0xff9aa1a9);
juce::Colour PluginLookAndFeel::statusWarn(0xfff5a623);
juce::Colour PluginLookAndFeel::statusBad(0xffff4d6a);
juce::Colour PluginLookAndFeel::statusGood(0xff34d399);
juce::Colour PluginLookAndFeel::accentBlue(0xff4da3ff);
juce::Colour PluginLookAndFeel::controlOutline = juce::Colour(0xff16181b).brighter(0.18f);

namespace {
PluginLookAndFeel::Palette g_palette = PluginLookAndFeel::Palette::Dark;

juce::Array<juce::Component::SafePointer<juce::Component>>& registeredRoots()
{
    static juce::Array<juce::Component::SafePointer<juce::Component>> roots;
    return roots;
}

void remapOne(juce::Component& c, const std::array<juce::Colour, PluginLookAndFeel::kNumTokens>& from,
              const std::array<juce::Colour, PluginLookAndFeel::kNumTokens>& to)
{
    // JUCE keeps a component's explicit colours as "jcclr_<hex id>" properties.
    struct Change {
        int id;
        juce::Colour colour;
    };
    juce::Array<Change> changes;
    const auto& props = c.getProperties();
    for (int i = 0; i < props.size(); ++i) {
        const auto name = props.getName(i).toString();
        if (!name.startsWith("jcclr_")) continue;
        const auto colour = juce::Colour(static_cast<juce::uint32>(static_cast<int>(props.getValueAt(i))));
        for (size_t t = 0; t < from.size(); ++t)
            if ((colour.getARGB() & 0x00ffffffu) == (from[t].getARGB() & 0x00ffffffu)) {
                changes.add({name.substring(6).getHexValue32(), to[t].withAlpha(colour.getAlpha())});
                break;
            }
    }
    for (const auto& change : changes) c.setColour(change.id, change.colour);
    if (auto* laf = dynamic_cast<PluginLookAndFeel*>(&c.getLookAndFeel())) laf->applyPalette();
    if (auto* laf = dynamic_cast<PluginEditorLookAndFeel*>(&c.getLookAndFeel())) laf->applyPalette();
    for (auto* child : c.getChildren()) remapOne(*child, from, to);
}
} // namespace

std::array<juce::Colour, PluginLookAndFeel::kNumTokens> PluginLookAndFeel::tokensFor(Palette palette)
{
    if (palette == Palette::HighContrast)
        // KhensuTheme High-Contrast: bgBase, bgSurface, accentTeal,
        // textPrimary, textSecondary, accentAmber, accentRed, accentGreen,
        // accentBlue, and borderActive over black as the control outline.
        return {juce::Colour(0xff050505), juce::Colour(0xff0a0a0b), juce::Colour(0xff2ff5e4),
                juce::Colour(0xffffffff), juce::Colour(0xffd0dadf), juce::Colour(0xffffc94d),
                juce::Colour(0xffff466b), juce::Colour(0xff4dffb0), juce::Colour(0xff66b8ff),
                juce::Colour(0xff666666)};
    return {juce::Colour(0xff101113), juce::Colour(0xff16181b), juce::Colour(0xff22d3c5),
            juce::Colour(0xffe8eaed), juce::Colour(0xff9aa1a9), juce::Colour(0xfff5a623),
            juce::Colour(0xffff4d6a), juce::Colour(0xff34d399), juce::Colour(0xff4da3ff),
            juce::Colour(0xff16181b).brighter(0.18f)};
}

PluginLookAndFeel::Palette PluginLookAndFeel::currentPalette() noexcept { return g_palette; }

void PluginLookAndFeel::setPalette(Palette palette)
{
    const auto from = g_palette;
    const auto t = tokensFor(palette);
    bgDark = t[0];
    bgPanel = t[1];
    accentTeal = t[2];
    textPrimary = t[3];
    textMuted = t[4];
    statusWarn = t[5];
    statusBad = t[6];
    statusGood = t[7];
    accentBlue = t[8];
    controlOutline = t[9];
    g_palette = palette;
    // STU-6: the shared-core widgets in the editor (StudioMeters, KhensuKnob,
    // ChartKit, EqCurveComponent ...) read ui::Palette - keep it in step.
    ui::Palette::setTheme(palette == Palette::HighContrast ? ui::Palette::Theme::HighContrast
                                                           : ui::Palette::Theme::Dark);
    if (from == palette) return;
    auto& roots = registeredRoots();
    for (int i = roots.size(); --i >= 0;)
        if (roots.getReference(i).getComponent() == nullptr) roots.remove(i);
    for (auto& root : roots)
        if (auto* c = root.getComponent()) {
            remapColours(*c, from, palette);
            c->sendLookAndFeelChange();
            c->repaint();
        }
}

void PluginLookAndFeel::remapColours(juce::Component& root, Palette from, Palette to)
{
    if (from == to) return;
    remapOne(root, tokensFor(from), tokensFor(to));
}

void PluginLookAndFeel::registerRoot(juce::Component& root)
{
    auto& roots = registeredRoots();
    for (auto& existing : roots)
        if (existing.getComponent() == &root) return;
    roots.add(juce::Component::SafePointer<juce::Component>(&root));
}

void PluginLookAndFeel::unregisterRoot(juce::Component& root)
{
    auto& roots = registeredRoots();
    for (int i = roots.size(); --i >= 0;)
        if (roots.getReference(i).getComponent() == &root || roots.getReference(i).getComponent() == nullptr)
            roots.remove(i);
}

PluginLookAndFeel::PluginLookAndFeel() { applyPalette(); }

void PluginLookAndFeel::applyPalette()
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

    // LG-16: two-pass glowing value arc (soft halo + crisp core).
    juce::Path valueArc;
    valueArc.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, rotaryStartAngle, angle, true);
    g.setColour(accentTeal.withAlpha(0.24f));
    g.strokePath(valueArc, juce::PathStrokeType(6.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(accentTeal);
    g.strokePath(valueArc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // LG-16: beveled glass center cap with specular rim.
    const float capR = radius * 0.52f;
    const auto capRect = juce::Rectangle<float>(centre.x - capR, centre.y - capR, capR * 2.0f, capR * 2.0f);
    juce::ColourGradient capGrad(bgPanel.brighter(0.22f), centre.x - capR * 0.4f, centre.y - capR * 0.4f,
                                 bgDark, centre.x + capR, centre.y + capR, true);
    g.setGradientFill(capGrad);
    g.fillEllipse(capRect);
    juce::ColourGradient capRim(textPrimary.withAlpha(0.26f), capRect.getTopLeft(), textPrimary.withAlpha(0.05f),
                                capRect.getBottomRight(), false);
    g.setGradientFill(capRim);
    g.drawEllipse(capRect.reduced(0.5f), 1.0f);

    // Luminous pointer
    juce::Path pointer;
    const float pointerLength = radius * 0.72f;
    pointer.addRoundedRectangle(-1.25f, -pointerLength, 2.5f, pointerLength * 0.65f, 1.25f);
    pointer.applyTransform(juce::AffineTransform::rotation(angle).translated(centre));
    g.setColour(textPrimary);
    g.fillPath(pointer);
}

PluginEditorLookAndFeel::PluginEditorLookAndFeel() { applyPalette(); }

void PluginEditorLookAndFeel::applyPalette()
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
    const float radius = juce::jmin(7.0f, r.getHeight() * 0.35f);
    const bool primary = copilot::button_role::isPrimary(button);
    const bool on = button.getToggleState();
    if (primary) {
        auto fill = button.isEnabled() ? P::accentTeal : P::controlOutline;
        if (down)
            fill = fill.darker(0.2f);
        else if (highlighted)
            fill = fill.brighter(0.1f);
        if (button.isEnabled()) {
            // LG-16: vertical luminance gradient + top specular lip matching desktop.
            juce::ColourGradient grad(fill.brighter(0.14f), r.getCentreX(), r.getY(), fill.darker(0.12f),
                                      r.getCentreX(), r.getBottom(), false);
            g.setGradientFill(grad);
        } else {
            g.setColour(fill);
        }
        g.fillRoundedRectangle(r, radius);
        if (button.isEnabled()) {
            juce::ColourGradient rim(juce::Colours::white.withAlpha(0.40f), r.getTopLeft(),
                                     juce::Colours::white.withAlpha(0.08f), r.getBottomRight(), false);
            g.setGradientFill(rim);
            g.drawRoundedRectangle(r, radius, 1.0f);
        }
        if (button.hasKeyboardFocus(true)) {
            g.setColour(P::textPrimary.withAlpha(0.8f));
            g.drawRoundedRectangle(r.reduced(1.5f), radius, 1.0f);
        }
        return;
    }
    // Secondary: frosted glass plate with curved Fresnel specular rim.
    auto fill = on ? button.findColour(juce::TextButton::buttonOnColourId) : backgroundColour;
    if (highlighted && button.isEnabled())
        fill = fill.isTransparent() ? P::textPrimary.withAlpha(0.07f) : fill.brighter(0.08f);
    if (down) fill = fill.isTransparent() ? P::textPrimary.withAlpha(0.11f) : fill.darker(0.1f);
    g.setColour(fill);
    g.fillRoundedRectangle(r, radius);
    const auto rimHi = on ? P::accentTeal.withAlpha(0.65f) : P::textPrimary.withAlpha(button.isEnabled() ? 0.22f : 0.08f);
    const auto rimLo = on ? P::accentTeal.withAlpha(0.25f) : P::textPrimary.withAlpha(button.isEnabled() ? 0.07f : 0.04f);
    juce::ColourGradient rim(rimHi, r.getTopLeft(), rimLo, r.getBottomRight(), false);
    g.setGradientFill(rim);
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
    const auto font = khensu::ui::Palette::uiFont(khensu::type::body, juce::Font::bold);
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
    g.setFont(khensu::ui::Palette::uiFont(khensu::type::body, front ? juce::Font::bold : juce::Font::plain));
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
    return khensu::ui::Palette::uiFont(khensu::type::body);
}

} // namespace khensu
