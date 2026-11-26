#include <khensu/ui/KhensuPalette.h>
#include <algorithm>
#include <map>
#include <utility>

namespace khensu::ui {

// Values mirror the KhensuTheme tokens (desktop-app/Source/ui/shared/
// KhensuTheme.cpp), one set per theme. Kept in sync by tests/ui/
// PaletteThemeSyncTest.cpp (audit F5, STU-6) - update both together. The
// semantic mapping is: bgPanel<->bgSurface, textMuted<->textSecondary,
// statusWarn<->accentAmber, statusBad<->accentRed, statusGood<->accentGreen.
// The statics start on the Dark set (UI-3 studio tokens).
juce::Colour Palette::bgVoid(0xff0b0c0e);
juce::Colour Palette::bgBase(0xff101113);
juce::Colour Palette::bgPanel(0xff16181b);
juce::Colour Palette::accentTeal(0xff22d3c5);
juce::Colour Palette::textPrimary(0xffe8eaed);
juce::Colour Palette::textMuted(0xff9aa1a9);
juce::Colour Palette::statusWarn(0xfff5a623);
juce::Colour Palette::statusBad(0xffff4d6a);
juce::Colour Palette::statusGood(0xff34d399);
juce::Colour Palette::accentBlue(0xff4da3ff);
juce::Colour Palette::borderActive(0x26ffffff);

namespace {
Palette::Theme g_theme = Palette::Theme::Dark;
std::uint32_t g_version = 0;

// Function-local statics, as KhensuTheme's listeners: components register
// during static init of long-lived UI objects.
std::map<int, std::function<void()>>& listeners()
{
    static std::map<int, std::function<void()>> handlers;
    return handlers;
}

int& nextListenerToken()
{
    static int token = 1;
    return token;
}
} // namespace

Palette::Tokens Palette::tokensFor(Theme theme)
{
    switch (theme) {
    case Theme::HighContrast:
        return {juce::Colour(0xff000000), juce::Colour(0xff050505), juce::Colour(0xff0a0a0b), juce::Colour(0xff2ff5e4),
                juce::Colour(0xffffffff), juce::Colour(0xffd0dadf), juce::Colour(0xffffc94d), juce::Colour(0xffff466b),
                juce::Colour(0xff4dffb0), juce::Colour(0xff66b8ff), juce::Colour(0x66ffffff)};
    case Theme::Light:
        return {juce::Colour(0xffe8eaed), juce::Colour(0xfff3f4f6), juce::Colour(0xffffffff), juce::Colour(0xff065b54),
                juce::Colour(0xff0d1417), juce::Colour(0xff48545c), juce::Colour(0xff724900), juce::Colour(0xff971e34),
                juce::Colour(0xff125d34), juce::Colour(0xff195390), juce::Colour(0x28000000)};
    case Theme::Dark:
        break;
    }
    return {juce::Colour(0xff0b0c0e), juce::Colour(0xff101113), juce::Colour(0xff16181b), juce::Colour(0xff22d3c5),
            juce::Colour(0xffe8eaed), juce::Colour(0xff9aa1a9), juce::Colour(0xfff5a623), juce::Colour(0xffff4d6a),
            juce::Colour(0xff34d399), juce::Colour(0xff4da3ff), juce::Colour(0x26ffffff)};
}

void Palette::setTheme(Theme theme)
{
    const auto t = tokensFor(theme);
    bgVoid = t.bgVoid;
    bgBase = t.bgBase;
    bgPanel = t.bgPanel;
    accentTeal = t.accentTeal;
    textPrimary = t.textPrimary;
    textMuted = t.textMuted;
    statusWarn = t.statusWarn;
    statusBad = t.statusBad;
    statusGood = t.statusGood;
    accentBlue = t.accentBlue;
    borderActive = t.borderActive;
    if (theme == g_theme) return;
    g_theme = theme;
    ++g_version;
    // Iterate a copy: a handler may add or remove listeners (a rebuild). A
    // listener removed by an earlier handler is skipped - its owner is gone.
    const auto handlers = listeners();
    for (const auto& [token, handler] : handlers)
        if (handler && listeners().count(token) != 0) handler();
}

Palette::Theme Palette::currentTheme() noexcept { return g_theme; }

std::uint32_t Palette::version() noexcept { return g_version; }

int Palette::addListener(std::function<void()> handler)
{
    if (!handler) return 0;
    const int token = nextListenerToken()++;
    listeners().emplace(token, std::move(handler));
    return token;
}

void Palette::removeListener(int token) { listeners().erase(token); }

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
    size = std::max(size, type::caption); // UXR-8: the 11 px floor, as KhensuTheme::uiFont
    return juce::Font(juce::FontOptions{family, size, style});
}

} // namespace khensu::ui
