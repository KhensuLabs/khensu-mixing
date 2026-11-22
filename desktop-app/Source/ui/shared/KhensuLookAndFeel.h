#pragma once
#include <JuceHeader.h>
#include <khensu/common/ThreadContext.h>
#include "KhensuTheme.h" // provides GlassPanelVariant

namespace khensu {

// THREAD_CONTEXT(khensu::ThreadContext::UIThread)
class KhensuLookAndFeel final : public juce::LookAndFeel_V4 {
  public:
    KhensuLookAndFeel();
    ~KhensuLookAndFeel() override;

    /// Installs the current KhensuTheme palette onto the stock JUCE colour IDs.
    /// Called at construction AND on every theme change - the IDs are a cached
    /// copy of the palette, so without the re-apply every stock JUCE widget
    /// (ComboBox, Slider, TextButton) keeps the previous theme's colours after
    /// a switch (kimi audit P0 #1).
    void applyThemeColours();

    static void drawGlassmorphicBackground(juce::Graphics& g, juce::Rectangle<int> bounds,
                                           const std::vector<float>& waveformPeaks = {},
                                           double playbackProgress = -1.0);

    /// Aurora Glass app backdrop: a themed radial base wash plus subtle, static
    /// (non-animated) teal + magenta aurora orbs. Painted once by the root
    /// component so every screen's glass panels read against a shared glow.
    /// Alpha is deliberately low so panel/text contrast is unaffected.
    static void drawAppBackground(juce::Graphics& g, juce::Rectangle<int> bounds);

    static void drawGlassPanel(juce::Graphics& g, juce::Rectangle<int> bounds,
                               GlassPanelVariant variant = GlassPanelVariant::Default,
                               juce::Colour accentColour = juce::Colours::transparentBlack,
                               const juce::Image& blurredBackdrop = {});

    /// Real-time backdrop blur for glass panels.
    /// JUCE has no CSS-style `backdrop-filter` primitive, so this renders the
    /// SAME drawAppBackground() math cropped+translated to `regionInAppSpace`
    /// (a panel's bounds expressed in the root component's coordinate space),
    /// at a small fraction of real size, then box-blurs it. The downscale does
    /// most of the "blur" work cheaply; the box-blur pass smooths the residual
    /// block structure. Callers should cache the result and only recompute when
    /// `regionInAppSpace` actually changes (resize/scroll), not on every paint.
    static juce::Image renderBlurredBackdrop(juce::Rectangle<int> regionInAppSpace, juce::Rectangle<int> appBounds);

    /// The whole app background, blurred as above, scaled back to full size
    /// and with `tint` composited over it: cached per app size, background
    /// palette (so a theme switch re-renders it) and tint.
    static juce::Image blurredAppBackdrop(juce::Rectangle<int> appBounds,
                                          juce::Colour tint = juce::Colours::transparentBlack);

    /// blurredAppBackdrop() tinted with `variant`'s glass fill - what the
    /// drawGlassPanel() overload below expects. Empty for Void.
    static juce::Image glassBackdrop(juce::Rectangle<int> appBounds, GlassPanelVariant variant);

    /// drawGlassPanel() over a region of glassBackdrop(appBounds, variant):
    /// `backdropSource` is the panel's bounds in the app coordinate space. The
    /// region is a 1:1 copy that already includes the fill, so moving or
    /// re-laying-out a panel (scroll, collapse, screen layout) never
    /// re-renders, re-scales or re-tints a blur.
    static void drawGlassPanel(juce::Graphics& g, juce::Rectangle<int> bounds, GlassPanelVariant variant,
                               const juce::Image& appBackdrop, juce::Rectangle<int> backdropSource);

    /// Pseudo-3D extruded bar (front/top/side faces via flat 2D paths - no
    /// OpenGL) for small heatmap/ridge strips like the loudness and dynamics
    /// section charts. `depth` is the isometric offset in pixels.
    static void draw3DHeatBar(juce::Graphics& g, juce::Rectangle<float> frontBounds, juce::Colour heatColour,
                              float depth = 5.0f);

    // Custom typography
    juce::Font getLabelFont(juce::Label& label) override;
    juce::Font getTextButtonFont(juce::TextButton& button, int buttonHeight) override;

    // Overrides for modern flat drawing
    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override;

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPosProportional,
                          float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider) override;

    // Colour palette lives in KhensuTheme (single source of truth). The
    // former KhensuLookAndFeel::* colour statics were retired onto it.

  private:
    /// renderBlurredBackdrop() as a software image (what the blur and the
    /// app-wide backdrop work on before one conversion to native).
    static juce::Image renderBlurredBackdropSoftware(juce::Rectangle<int> regionInAppSpace,
                                                     juce::Rectangle<int> appBounds);

    ThemeListener themeListener_{[this] { applyThemeColours(); }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KhensuLookAndFeel)
};

} // namespace khensu
