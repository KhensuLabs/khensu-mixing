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

// Editor-wide (UX-4, UI-6): the studio look for every control the editor
// owns - flat outlined secondary buttons, one filled accent primary
// (copilot::button_role), text tabs with an accent underline, 13 px text
// with a 12 px floor. Set on the editor, so it reaches every child that has
// no look-and-feel of its own (sliders in DspControlGridPanel keep theirs).
class PluginEditorLookAndFeel final : public juce::LookAndFeel_V4 {
  public:
    PluginEditorLookAndFeel();
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                              bool highlighted, bool down) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    int getTabButtonBestWidth(juce::TabBarButton&, int tabDepth) override;
    void drawTabButton(juce::TabBarButton&, juce::Graphics&, bool isMouseOver, bool isMouseDown) override;
    void drawTabbedButtonBarBackground(juce::TabbedButtonBar&, juce::Graphics&) override;
    void drawTabAreaBehindFrontButton(juce::TabbedButtonBar&, juce::Graphics&, int w, int h) override;
    juce::Font getPopupMenuFont() override;
};

} // namespace khensu
