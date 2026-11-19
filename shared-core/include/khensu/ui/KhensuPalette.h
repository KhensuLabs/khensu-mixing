#pragma once
#include <juce_graphics/juce_graphics.h>

namespace khensu::ui {

/// Minimal shared colour palette for components used by both the desktop
/// app and the VST3 plugin editor (which cannot link desktop-app's full
/// KhensuLookAndFeel). Values must stay in sync with the Dark-theme tokens in
/// desktop-app/Source/ui/shared/KhensuTheme.cpp — enforced by
/// tests/ui/PaletteThemeSyncTest.cpp (audit F5).
struct Palette {
    static const juce::Colour bgVoid; ///< deepest background (KhensuTheme::bgVoid)
    static const juce::Colour bgBase;
    static const juce::Colour bgPanel;
    static const juce::Colour accentTeal;
    static const juce::Colour textPrimary;
    static const juce::Colour textMuted;
    static const juce::Colour statusWarn; ///< amber - moderate severity
    static const juce::Colour statusBad;  ///< red - severe severity
    static const juce::Colour statusGood; ///< green - strength / completed state
    static const juce::Colour accentBlue; ///< blue - minor severity / category
    static const juce::Colour borderActive; ///< hairline border (KhensuTheme::borderActive)

    /// A concrete UI sans at `size`, mirroring KhensuTheme::uiFont()'s fallback
    /// path for the shared widgets that cannot link the desktop theme.
    ///
    /// Use this instead of FontOptions{"Sans-Serif", ...}: that placeholder
    /// resolves on some Windows setups to a byte-mode fallback typeface which
    /// renders a string's UTF-8 bytes as individual Latin-1 glyphs (an em-dash
    /// shows as "a€"), so any non-ASCII label turns to mojibake.
    static juce::Font uiFont(float size, int style = juce::Font::plain);
};

} // namespace khensu::ui
