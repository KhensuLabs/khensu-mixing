#pragma once
#include <JuceHeader.h>

namespace khensu {

// Small local look-and-feel for DspControlGridPanel only (scoped via
// setLookAndFeel on that component, not applied plugin-wide) - visually
// consistent with desktop-app's KhensuLookAndFeel palette without pulling
// desktop-app UI code into the plugin-copilot CMake target.
class PluginLookAndFeel final : public juce::LookAndFeel_V4 {
  public:
    PluginLookAndFeel();

    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float sliderPosProportional,
                          float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;

    static const juce::Colour bgDark;
    static const juce::Colour bgPanel;
    static const juce::Colour accentTeal;
    static const juce::Colour textPrimary;
    static const juce::Colour textMuted;
    static const juce::Colour statusWarn;
    static const juce::Colour statusBad;
    static const juce::Colour statusGood;
    static const juce::Colour accentBlue;
    static const juce::Colour controlOutline;
};

// Editor-wide (UX-4): JUCE's default button font is min(16, h * 0.6), so a
// 16 px plugin button had ~9.6 px text. Only the font changes - set on the
// editor, it reaches every TextButton without restyling sliders the way
// PluginLookAndFeel would.
class PluginEditorLookAndFeel final : public juce::LookAndFeel_V4 {
  public:
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
};

} // namespace khensu
