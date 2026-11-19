#include "PluginLookAndFeel.h"
#include <khensu/ui/KhensuPalette.h>

namespace khensu {

// Literal values, not copies of ui::Palette's statics: those live in another
// translation unit, and C++ leaves cross-TU static initialisation order
// unspecified - a Debug build initialised these first and read Palette as
// transparent black (Release only worked because the optimiser folded the
// copies). tests/plugin/PluginPaletteTest.cpp keeps the two in sync.
const juce::Colour PluginLookAndFeel::bgDark(0xff080b0d);
const juce::Colour PluginLookAndFeel::bgPanel(0xff0e1518);
const juce::Colour PluginLookAndFeel::accentTeal(0xff22d3c5);
const juce::Colour PluginLookAndFeel::textPrimary(0xffeaf0f4);
const juce::Colour PluginLookAndFeel::textMuted(0xff8a94a0);
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

juce::Font PluginEditorLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return juce::Font(juce::FontOptions(juce::jlimit(12.0f, 15.0f, (float)buttonHeight * 0.6f)));
}

} // namespace khensu
