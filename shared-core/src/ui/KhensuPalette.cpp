#include <khensu/ui/KhensuPalette.h>

namespace khensu::ui {

// Values mirror the Dark-theme KhensuTheme tokens (desktop-app/Source/ui/
// shared/KhensuTheme.cpp). Kept in sync by tests/ui/PaletteThemeSyncTest.cpp
// (audit F5) — update both together. The semantic mapping is: bgPanel↔bgSurface,
// textMuted↔textSecondary, statusWarn↔accentAmber, statusBad↔accentRed,
// statusGood↔accentGreen.
// Re-synced to the studio Dark tokens (UI-3, 2026-11-23).
const juce::Colour Palette::bgVoid(0xff0b0c0e);
const juce::Colour Palette::bgBase(0xff101113);
const juce::Colour Palette::bgPanel(0xff16181b);
const juce::Colour Palette::accentTeal(0xff22d3c5);
const juce::Colour Palette::textPrimary(0xffe8eaed);
const juce::Colour Palette::textMuted(0xff9aa1a9);
const juce::Colour Palette::statusWarn(0xfff5a623);
const juce::Colour Palette::statusBad(0xffff4d6a);
const juce::Colour Palette::statusGood(0xff34d399);
const juce::Colour Palette::accentBlue(0xff4da3ff);
const juce::Colour Palette::borderActive(0x26ffffff);

juce::Font Palette::uiFont(float size, int style)
{
    // Name a real family, never the "<Sans-Serif>" placeholder - see the header
    // note. Mirrors KhensuTheme::uiTypefaceName()'s per-platform choice.
    static const juce::String family = []() -> juce::String {
#if JUCE_WINDOWS
        return "Segoe UI"; // ships on Windows 7+; full Unicode punctuation
#elif JUCE_MAC
        return "Helvetica Neue";
#else
        return juce::Font::getDefaultSansSerifFontName();
#endif
    }();
    return juce::Font(juce::FontOptions{family, size, style});
}

} // namespace khensu::ui
