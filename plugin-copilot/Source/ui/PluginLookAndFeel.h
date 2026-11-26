#pragma once
#include <JuceHeader.h>
#include <array>

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

    // The palette's tokens. Reassigned by setPalette() (VIS-9), so read them
    // live in paint(); a colour cached with setColour() is moved over by
    // remapColours().
    static juce::Colour bgDark;
    static juce::Colour bgPanel;
    static juce::Colour accentTeal;
    static juce::Colour textPrimary;
    static juce::Colour textMuted;
    static juce::Colour statusWarn;
    static juce::Colour statusBad;
    static juce::Colour statusGood;
    static juce::Colour accentBlue;
    static juce::Colour controlOutline;

    /// VIS-9: Dark (the desktop's Dark theme) or High contrast (the desktop's
    /// High-Contrast theme values: black base, white text, brighter accents).
    enum class Palette { Dark = 0, HighContrast = 1 };
    static constexpr int kNumTokens = 10;
    /// The tokens of `palette`, in the order of the statics above.
    static std::array<juce::Colour, kNumTokens> tokensFor(Palette palette);
    static Palette currentPalette() noexcept;
    /// Reassigns the statics and moves every registered editor over
    /// (remapColours + repaint). THREAD_CONTEXT(UIThread).
    static void setPalette(Palette palette);
    /// Every colour cached on `root` and its children (setColour) that is one
    /// of `from`'s tokens becomes the matching token of `to`, alpha kept.
    static void remapColours(juce::Component& root, Palette from, Palette to);
    /// An editor registers its root so a palette switch reaches it.
    static void registerRoot(juce::Component& root);
    static void unregisterRoot(juce::Component& root);

    /// Re-reads the token colours this look-and-feel caches.
    void applyPalette();
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
    /// Re-reads the token colours this look-and-feel caches (VIS-9).
    void applyPalette();
};

} // namespace khensu
