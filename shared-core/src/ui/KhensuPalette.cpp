#include <khensu/ui/KhensuPalette.h>

namespace khensu::ui {

// Values mirror the Dark-theme KhensuTheme tokens (desktop-app/Source/ui/
// shared/KhensuTheme.cpp). Kept in sync by tests/ui/PaletteThemeSyncTest.cpp
// (audit F5) — update both together. The semantic mapping is: bgPanel↔bgSurface,
// textMuted↔textSecondary, statusWarn↔accentAmber, statusBad↔accentRed,
// statusGood↔accentGreen.
// Aurora Glass (2026-07-16): re-synced to the Dark-theme tokens.
const juce::Colour Palette::bgVoid(0xff050608);
const juce::Colour Palette::bgBase(0xff080b0d);
const juce::Colour Palette::bgPanel(0xff0e1518);
const juce::Colour Palette::accentTeal(0xff22d3c5);
const juce::Colour Palette::textPrimary(0xffeaf0f4);
const juce::Colour Palette::textMuted(0xff8a94a0);
const juce::Colour Palette::statusWarn(0xfff5a623);
const juce::Colour Palette::statusBad(0xffff4d6a);
const juce::Colour Palette::statusGood(0xff34d399);
const juce::Colour Palette::accentBlue(0xff4da3ff);
const juce::Colour Palette::borderActive(0x22ffffff);

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
