#pragma once
#include <juce_graphics/juce_graphics.h>
#include <khensu/ui/TypeScale.h> // UXR-8: type:: font sizes
#include <cstdint>
#include <functional>

namespace khensu::ui {

/// Minimal shared colour palette for components used by both the desktop
/// app and the VST3 plugin editor (which cannot link desktop-app's full
/// KhensuLookAndFeel). Each theme's values must stay in sync with the matching
/// KhensuTheme tokens in desktop-app/Source/ui/shared/KhensuTheme.cpp -
/// enforced by tests/ui/PaletteThemeSyncTest.cpp (audit F5, STU-6).
///
/// STU-6: the tokens are the CURRENT theme's, reassigned by setTheme() (the
/// desktop's KhensuTheme::setTheme and the plugin's PluginLookAndFeel::
/// setPalette call it). Read them live in paint(); a component that caches
/// one (setColour, a stored Style) re-reads it from a PaletteListener or by
/// comparing version(). Message thread only - the audio thread never reads
/// these.
struct Palette {
    static juce::Colour bgVoid; ///< deepest background (KhensuTheme::bgVoid)
    static juce::Colour bgBase;
    static juce::Colour bgPanel;
    static juce::Colour accentTeal;
    static juce::Colour textPrimary;
    static juce::Colour textMuted;
    static juce::Colour statusWarn;   ///< amber - moderate severity
    static juce::Colour statusBad;    ///< red - severe severity
    static juce::Colour statusGood;   ///< green - strength / completed state
    static juce::Colour accentBlue;   ///< blue - minor severity / category
    static juce::Colour borderActive; ///< hairline border (KhensuTheme::borderActive)

    /// Mirrors KhensuTheme::Theme (same order).
    enum class Theme { Dark = 0, HighContrast = 1, Light = 2 };

    /// One theme's token set, in the order of the statics above.
    struct Tokens {
        juce::Colour bgVoid, bgBase, bgPanel, accentTeal, textPrimary, textMuted, statusWarn, statusBad, statusGood,
            accentBlue, borderActive;
    };
    static Tokens tokensFor(Theme theme);

    /// Reassigns every token to `theme`'s values and, when the theme changed,
    /// bumps version() and calls the listeners. THREAD_CONTEXT(UIThread).
    static void setTheme(Theme theme);
    static Theme currentTheme() noexcept;
    /// Increases on every theme change - a cheap "did my cached colours go
    /// stale?" check for components that would rather not listen.
    static std::uint32_t version() noexcept;

    /// Called after setTheme() has reassigned the tokens. Returns a token for
    /// removeListener(); prefer the RAII PaletteListener below.
    static int addListener(std::function<void()> handler);
    static void removeListener(int token);

    /// A concrete UI sans at `size`, mirroring KhensuTheme::uiFont()'s fallback
    /// path for the shared widgets that cannot link the desktop theme. Sizes
    /// come from khensu::type (TypeScale.h) and are floored at type::caption.
    ///
    /// Use this instead of FontOptions{"Sans-Serif", ...}: that placeholder
    /// resolves on some Windows setups to a byte-mode fallback typeface which
    /// renders a string's UTF-8 bytes as individual Latin-1 glyphs (an em-dash
    /// shows as "a€"), so any non-ASCII label turns to mojibake.
    static juce::Font uiFont(float size, int style = juce::Font::plain);
};

/// RAII Palette listener - the shared-core twin of the desktop's
/// ThemeListener. Declare it as the LAST member of the component it
/// re-tints, so it deregisters before the members its handler touches go.
class PaletteListener final {
  public:
    explicit PaletteListener(std::function<void()> handler) : token_(Palette::addListener(std::move(handler))) {}
    ~PaletteListener() { Palette::removeListener(token_); }
    PaletteListener(const PaletteListener&) = delete;
    PaletteListener& operator=(const PaletteListener&) = delete;
    PaletteListener(PaletteListener&&) = delete;
    PaletteListener& operator=(PaletteListener&&) = delete;

  private:
    int token_;
};

} // namespace khensu::ui
