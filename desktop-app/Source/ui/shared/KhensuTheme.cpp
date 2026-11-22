// desktop-app/Source/ui/shared/KhensuTheme.cpp
#include "KhensuTheme.h"
#include "../../BundledResources.h"
#include <map>
#include <utility>

namespace khensu {

namespace {

// Aurora Glass design fonts (2026-07-16): Space Grotesk (UI) + JetBrains Mono
// (numeric/mono), both SIL Open Font License 1.1 - see resources/fonts/*/OFL.txt.
// Loaded as loose files (matching the resources/system_brain and
// resources/genre_norms convention - see AppController's exe-dir-then-cwd
// resolution) rather than compiled-in binary data, so no CMake binary-data
// step is needed. Bundling was blocked under JUCE 7 (custom/Unicode fonts
// rendered as per-byte mojibake); FontOptions fixes that.
juce::File findFontsDir()
{
    return bundled_resources::find("resources/fonts");
}

// Returns nullptr (never throws/asserts) if the file is absent or fails to
// parse - every caller falls back to the platform system font in that case,
// so a missing/corrupt bundle never blocks the app from rendering text.
juce::Typeface::Ptr loadBundledTypeface(const juce::String& relativePath)
{
    auto file = findFontsDir().getChildFile(relativePath);
    if (!file.existsAsFile()) return nullptr;
    juce::MemoryBlock data;
    if (!file.loadFileAsData(data) || data.getSize() == 0) return nullptr;
    return juce::Typeface::createSystemTypefaceFor(data.getData(), data.getSize());
}

} // namespace

// Aurora Glass defaults (2026-07-16) - mirror the Dark branch of setTheme().
juce::Colour KhensuTheme::bgVoid(0xff050608);
juce::Colour KhensuTheme::bgBase(0xff080b0d);
juce::Colour KhensuTheme::bgSurface(0xff0e1518);
juce::Colour KhensuTheme::bgRaised(0xff141d21);
juce::Colour KhensuTheme::bgOverlay(0xff1a2429);

juce::Colour KhensuTheme::borderSubtle(0x12ffffff);
juce::Colour KhensuTheme::borderActive(0x22ffffff);

juce::Colour KhensuTheme::accentTeal(0xff22d3c5);
juce::Colour KhensuTheme::accentTealDim(0x3322d3c5);
juce::Colour KhensuTheme::accentAmber(0xfff5a623);
juce::Colour KhensuTheme::accentAmberDim(0x33f5a623);
juce::Colour KhensuTheme::accentRed(0xffff4d6a);
juce::Colour KhensuTheme::accentRedDim(0x33ff4d6a);
// accentPurple is deprecated as a primary UI accent (see KhensuTheme.h).
juce::Colour KhensuTheme::accentPurple(0xffa78bfa);
juce::Colour KhensuTheme::accentPurpleDim(0x33a78bfa);
juce::Colour KhensuTheme::accentMagenta(0xfff0308f);
juce::Colour KhensuTheme::accentMagentaDim(0x33f0308f);
juce::Colour KhensuTheme::accentBlue(0xff4da3ff);
juce::Colour KhensuTheme::accentBlueDim(0x334da3ff);
juce::Colour KhensuTheme::accentGreen(0xff34d399);
juce::Colour KhensuTheme::accentGreenDim(0x3334d399);
juce::Colour KhensuTheme::scrim(0xb3000000);
juce::Colour KhensuTheme::shadowColour(0x80000000);
juce::Colour KhensuTheme::highlight(0x0dffffff);
juce::Colour KhensuTheme::focusRing(0xff22d3c5);

juce::Colour KhensuTheme::textPrimary(0xffeaf0f4);
juce::Colour KhensuTheme::textSecondary(0xff8a94a0);
juce::Colour KhensuTheme::textTertiary(0xff7d8c96);
juce::Colour KhensuTheme::textAccent(0xff22d3c5);

const juce::String& KhensuTheme::uiTypefaceName()
{
    // The platform's real UI sans, resolved once. IMPORTANT: name a real font
    // family here, NOT juce::Font::getDefaultSansSerifFontName() - that returns
    // the "<Sans-Serif>" placeholder token, which on some Windows setups resolves
    // to a byte-mode fallback typeface that renders a string's UTF-8 bytes as
    // individual Latin-1 glyphs (an em-dash U+2014 shows as "â€"", an ellipsis as
    // "â€¦"). A concrete Unicode family avoids that entirely.
    static const juce::String name = []() -> juce::String {
#if JUCE_WINDOWS
        return "Segoe UI"; // ships on Windows 7+; full Unicode punctuation
#elif JUCE_MAC
        return "Helvetica Neue";
#else
        return juce::Font::getDefaultSansSerifFontName();
#endif
    }();
    return name;
}
juce::Font KhensuTheme::uiFont(float size, int style)
{
    size = std::max(size, kMinTextPx); // UX-16: nothing smaller than 11 px
    // Real bold/regular faces (not synthetic) from the bundled Space Grotesk;
    // italic stays synthetic (.italicised()) since no italic face is bundled -
    // matches how the pre-bundling system-font path already synthesized italic.
    static const juce::Typeface::Ptr regular = loadBundledTypeface("SpaceGrotesk/SpaceGrotesk-Regular.ttf");
    static const juce::Typeface::Ptr bold = loadBundledTypeface("SpaceGrotesk/SpaceGrotesk-Bold.ttf");

    const bool wantsBold = (style & juce::Font::bold) != 0;
    if (const juce::Typeface::Ptr tf = (wantsBold && bold != nullptr) ? bold : regular) {
        juce::Font font(juce::FontOptions(tf).withHeight(size));
        if ((style & juce::Font::italic) != 0) font = font.italicised();
        return font;
    }
    // Bundle missing/failed to load - fall back to the platform system font,
    // exactly as before bundling existed.
    return juce::Font(juce::FontOptions{uiTypefaceName(), size, style});
}
juce::Font KhensuTheme::bodyFont(float size) { return uiFont(size); }
juce::Font KhensuTheme::labelFont(float size) { return uiFont(size); }
juce::Font KhensuTheme::metricFont(float size)
{
    // Light weight for large numeric readouts
    return uiFont(size);
}
juce::Font KhensuTheme::monoFont(float size)
{
    size = std::max(size, kMinTextPx); // UX-16
    static const juce::Typeface::Ptr regular = loadBundledTypeface("JetBrainsMono/JetBrainsMono-Regular.ttf");
    if (regular != nullptr) return juce::Font(juce::FontOptions(regular).withHeight(size));
    // Bundle missing/failed to load - fall back to the platform system mono font.
    return juce::Font(juce::FontOptions{juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain});
}
juce::Font KhensuTheme::sectionHeaderFont() { return uiFont(kFontLabel); }

juce::Colour KhensuTheme::colourForStatus(BadgeStatus s)
{
    switch (s) {
    case BadgeStatus::Pass:
        return accentGreen;
    case BadgeStatus::Warn:
        return accentAmber;
    case BadgeStatus::Fail:
        return accentRed;
    case BadgeStatus::Neutral:
        return textSecondary;
    }
    return textSecondary;
}

juce::Colour KhensuTheme::dimColourForStatus(BadgeStatus s)
{
    switch (s) {
    case BadgeStatus::Pass:
        return accentGreenDim;
    case BadgeStatus::Warn:
        return accentAmberDim;
    case BadgeStatus::Fail:
        return accentRedDim;
    case BadgeStatus::Neutral:
        return borderSubtle;
    }
    return borderSubtle;
}

// ── Runtime theming ─────────────────────────────────────────────────────────
namespace {
KhensuTheme::Theme g_currentTheme = KhensuTheme::Theme::Dark;

// Theme-change handlers, keyed by the token handed back to the registrant.
// A std::map (not a vector) so removeThemeListener() during teardown can't
// invalidate another entry's token, and so iteration order stays registration
// order. Function-local statics: these are touched during static init of
// long-lived UI objects, so a namespace-scope container would risk the static
// initialisation order fiasco.
std::map<int, std::function<void()>>& themeListeners()
{
    static std::map<int, std::function<void()>> listeners;
    return listeners;
}

int& nextThemeListenerToken()
{
    static int token = 1;
    return token;
}
} // namespace

int KhensuTheme::addThemeListener(std::function<void()> handler)
{
    if (!handler) return 0;
    const int token = nextThemeListenerToken()++;
    themeListeners().emplace(token, std::move(handler));
    return token;
}

void KhensuTheme::removeThemeListener(int token) { themeListeners().erase(token); }

KhensuTheme::Theme KhensuTheme::currentTheme() noexcept { return g_currentTheme; }

const char* KhensuTheme::themeName(Theme t) noexcept
{
    switch (t) {
    case Theme::Dark:
        return "Dark";
    case Theme::HighContrast:
        return "High Contrast";
    case Theme::Light:
        return "Light";
    }
    return "Dark";
}

KhensuTheme::Theme KhensuTheme::cycleTheme()
{
    const Theme next = g_currentTheme == Theme::Dark           ? Theme::HighContrast
                       : g_currentTheme == Theme::HighContrast ? Theme::Light
                                                               : Theme::Dark;
    setTheme(next);
    return next;
}

void KhensuTheme::setTheme(Theme t)
{
    g_currentTheme = t;

    // Single source of truth for the whole app's palette - every UI surface
    // reads these statics live in paint(), so reassigning them here re-tints
    // the entire app on the next repaint. (KhensuLookAndFeel's old parallel
    // palette has been retired onto these.)
    switch (t) {
    case Theme::Dark:
        // Aurora Glass: near-black teal-tinted surfaces, teal + magenta accents.
        bgVoid = juce::Colour(0xff050608);
        bgBase = juce::Colour(0xff080b0d);
        bgSurface = juce::Colour(0xff0e1518);
        bgRaised = juce::Colour(0xff141d21);
        bgOverlay = juce::Colour(0xff1a2429);
        borderSubtle = juce::Colour(0x12ffffff);
        borderActive = juce::Colour(0x22ffffff);
        accentTeal = juce::Colour(0xff22d3c5);
        accentTealDim = juce::Colour(0x3322d3c5);
        accentAmber = juce::Colour(0xfff5a623);
        accentAmberDim = juce::Colour(0x33f5a623);
        accentRed = juce::Colour(0xffff4d6a);
        accentRedDim = juce::Colour(0x33ff4d6a);
        accentPurple = juce::Colour(0xffa78bfa);
        accentPurpleDim = juce::Colour(0x33a78bfa);
        accentMagenta = juce::Colour(0xfff0308f);
        accentMagentaDim = juce::Colour(0x33f0308f);
        accentBlue = juce::Colour(0xff4da3ff);
        accentBlueDim = juce::Colour(0x334da3ff);
        accentGreen = juce::Colour(0xff34d399);
        accentGreenDim = juce::Colour(0x3334d399);
        scrim = juce::Colour(0xb3000000);
        shadowColour = juce::Colour(0x80000000);
        highlight = juce::Colour(0x0dffffff);
        focusRing = juce::Colour(0xff22d3c5);
        textPrimary = juce::Colour(0xffeaf0f4);
        textSecondary = juce::Colour(0xff8a94a0);
        textTertiary = juce::Colour(0xff7d8c96); // UX-16: 4.9:1 on bgRaised (was 4.2)
        textAccent = juce::Colour(0xff22d3c5);
        break;

    case Theme::HighContrast:
        // Aurora Glass, high-contrast: pure-black base, brighter teal/magenta.
        bgVoid = juce::Colour(0xff000000);
        bgBase = juce::Colour(0xff000406);
        bgSurface = juce::Colour(0xff03090b);
        bgRaised = juce::Colour(0xff0a1416);
        bgOverlay = juce::Colour(0xff14201f);
        borderSubtle = juce::Colour(0x40ffffff);
        borderActive = juce::Colour(0x66ffffff);
        accentTeal = juce::Colour(0xff2ff5e4);
        accentTealDim = juce::Colour(0x552ff5e4);
        accentAmber = juce::Colour(0xffffc94d);
        accentAmberDim = juce::Colour(0x55ffc94d);
        accentRed = juce::Colour(0xffff466b);
        accentRedDim = juce::Colour(0x55ff466b);
        accentPurple = juce::Colour(0xffc4a7ff);
        accentPurpleDim = juce::Colour(0x55c4a7ff);
        accentMagenta = juce::Colour(0xffff4da3);
        accentMagentaDim = juce::Colour(0x55ff4da3);
        accentBlue = juce::Colour(0xff66b8ff);
        accentBlueDim = juce::Colour(0x5566b8ff);
        accentGreen = juce::Colour(0xff4dffb0);
        accentGreenDim = juce::Colour(0x554dffb0);
        // High-contrast: brighter focus ring, opaque scrim, stronger sheen.
        scrim = juce::Colour(0xd9000000);
        shadowColour = juce::Colour(0x99000000);
        highlight = juce::Colour(0x1affffff);
        focusRing = juce::Colour(0xff2ff5e4);
        textPrimary = juce::Colour(0xffffffff);
        textSecondary = juce::Colour(0xffd0dadf);
        textTertiary = juce::Colour(0xff96a2ac);
        textAccent = juce::Colour(0xff2ff5e4);
        break;

    case Theme::Light:
        // Aurora Glass, light: cool off-white surfaces, deepened teal/magenta
        // so the accents stay legible on white.
        // The four background layers must stay tonally DISTINCT here as they
        // are in the dark themes: panels read as panels because bgSurface sits
        // above bgBase. bgBase and bgSurface were both pure white, which
        // flattened every panel edge into its parent (kimi audit P2 #39).
        bgVoid = juce::Colour(0xffe7edf0);
        bgBase = juce::Colour(0xfff3f7f9);
        bgSurface = juce::Colour(0xffffffff);
        bgRaised = juce::Colour(0xfff7fafb);
        bgOverlay = juce::Colour(0xffe8eff2);
        borderSubtle = juce::Colour(0x14000000);
        borderActive = juce::Colour(0x28000000);
        accentTeal = juce::Colour(0xff065b54);
        accentTealDim = juce::Colour(0x33065b54);
        accentAmber = juce::Colour(0xff724900);
        accentAmberDim = juce::Colour(0x33724900);
        accentRed = juce::Colour(0xff971e34);
        accentRedDim = juce::Colour(0x33971e34);
        accentPurple = juce::Colour(0xff7c5cff);
        accentPurpleDim = juce::Colour(0x337c5cff);
        accentMagenta = juce::Colour(0xff951858);
        accentMagentaDim = juce::Colour(0x33951858);
        accentBlue = juce::Colour(0xff195390);
        accentBlueDim = juce::Colour(0x33195390);
        accentGreen = juce::Colour(0xff125d34);
        accentGreenDim = juce::Colour(0x33125d34);
        // Light theme: dark-on-light scrim/shadow, subtle dark sheen.
        scrim = juce::Colour(0x66000000);
        shadowColour = juce::Colour(0x33000000);
        highlight = juce::Colour(0x14000000);
        focusRing = juce::Colour(0xff065b54);
        textPrimary = juce::Colour(0xff0d1417);
        textSecondary = juce::Colour(0xff48545c);
        textTertiary = juce::Colour(0xff65727a);
        textAccent = juce::Colour(0xff065b54);
        break;
    }

    // Palette is fully reassigned - now let cached-colour surfaces re-apply.
    // Iterate a copy: a handler is allowed to add/remove listeners (a panel
    // rebuilding its children will), which would otherwise invalidate the
    // iterator mid-loop.
    const auto handlers = themeListeners();
    for (const auto& [token, handler] : handlers) {
        juce::ignoreUnused(token);
        if (handler) handler();
    }
}

} // namespace khensu
