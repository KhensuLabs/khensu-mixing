// desktop-app/Source/ui/shared/KhensuTheme.cpp
#include "KhensuTheme.h"
#include "../../BundledResources.h"
#include <khensu/ui/KhensuPalette.h>
#include <khensu/ui/VizPalette.h>
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

// Defaults mirror the Dark branch of setTheme().
juce::Colour KhensuTheme::bgVoid(0xff0b0c0e);
juce::Colour KhensuTheme::bgBase(0xff101113);
juce::Colour KhensuTheme::bgSurface(0xff16181b);
juce::Colour KhensuTheme::bgRaised(0xff1c1f23);
juce::Colour KhensuTheme::bgOverlay(0xff24282d);

juce::Colour KhensuTheme::borderSubtle(0x14ffffff);
juce::Colour KhensuTheme::borderActive(0x26ffffff);

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
juce::Colour KhensuTheme::shadowColour(0x66000000);
juce::Colour KhensuTheme::highlight(0x08ffffff);
juce::Colour KhensuTheme::focusRing(0xff22d3c5);

juce::Colour KhensuTheme::textPrimary(0xffe8eaed);
juce::Colour KhensuTheme::textSecondary(0xff9aa1a9);
juce::Colour KhensuTheme::textTertiary(0xff878e97);
juce::Colour KhensuTheme::textAccent(0xff22d3c5);
// VIZ-F2: assigned per theme in setTheme(); these are the Dark values.
juce::Colour KhensuTheme::vizSpreadOuter(0x14e8eaed);
juce::Colour KhensuTheme::vizSpreadInner(0x26e8eaed);
juce::Colour KhensuTheme::vizMedian(0xcc9aa1a9);
juce::Colour KhensuTheme::vizTrack(0xffe8eaed);
juce::Colour KhensuTheme::vizReference(0xff9085e9);
juce::Colour KhensuTheme::vizGrid(0x10e8eaed);
juce::Colour KhensuTheme::vizAxis(0x2ee8eaed);

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
juce::Font KhensuTheme::metricFont(float size) { return numericFont(size); }
juce::Font KhensuTheme::numericFont(float size, int style)
{
    // UI-4: the bundled Space Grotesk draws proportional digits, so a value
    // that changes (1.1 -> 8.8) shifted sideways and columns did not line up.
    // The platform UI face (Segoe UI / Helvetica Neue) draws tabular lining
    // figures by default; a concrete family, never the Sans-Serif placeholder.
    size = std::max(size, kMinTextPx);
    return juce::Font(juce::FontOptions{uiTypefaceName(), size, style});
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

namespace {
khensu::ui::viz::Mode vizMode() noexcept
{
    return KhensuTheme::currentTheme() == KhensuTheme::Theme::Light ? khensu::ui::viz::Mode::Light
                                                                    : khensu::ui::viz::Mode::Dark;
}
} // namespace

juce::Colour KhensuTheme::vizCategory(int slot) { return khensu::ui::viz::categorical(slot, vizMode()); }
juce::Colour KhensuTheme::vizSequential(float t) { return khensu::ui::viz::sequential(t, vizMode()); }
juce::Colour KhensuTheme::vizDiverging(float t) { return khensu::ui::viz::diverging(t, vizMode()); }

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
        // Studio (UI-3): near-monochrome graphite surfaces and ONE accent, teal.
        // Magenta stays only as a categorical data colour, never decoration.
        bgVoid = juce::Colour(0xff0b0c0e);
        bgBase = juce::Colour(0xff101113);
        bgSurface = juce::Colour(0xff16181b);
        bgRaised = juce::Colour(0xff1c1f23);
        bgOverlay = juce::Colour(0xff24282d);
        borderSubtle = juce::Colour(0x14ffffff);
        borderActive = juce::Colour(0x26ffffff);
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
        shadowColour = juce::Colour(0x66000000);
        highlight = juce::Colour(0x08ffffff);
        focusRing = juce::Colour(0xff22d3c5);
        textPrimary = juce::Colour(0xffe8eaed);
        textSecondary = juce::Colour(0xff9aa1a9);
        textTertiary = juce::Colour(0xff878e97); // UX-16: 4.9:1 on bgRaised (was 4.2)
        textAccent = juce::Colour(0xff22d3c5);
        break;

    case Theme::HighContrast:
        // Aurora Glass, high-contrast: pure-black base, brighter teal/magenta.
        bgVoid = juce::Colour(0xff000000);
        bgBase = juce::Colour(0xff050505);
        bgSurface = juce::Colour(0xff0a0a0b);
        bgRaised = juce::Colour(0xff121314);
        bgOverlay = juce::Colour(0xff1b1c1e);
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
        bgVoid = juce::Colour(0xffe8eaed);
        bgBase = juce::Colour(0xfff3f4f6);
        bgSurface = juce::Colour(0xffffffff);
        bgRaised = juce::Colour(0xfff8f9fa);
        bgOverlay = juce::Colour(0xffeceef1);
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
        textTertiary = juce::Colour(0xff63707a);
        textAccent = juce::Colour(0xff065b54);
        break;
    }

    // VIZ-F2: the data-visualisation tokens, from the shared viz palette.
    {
        const bool light = t == Theme::Light;
        vizSpreadOuter = textPrimary.withAlpha(light ? 0.07f : 0.08f);
        vizSpreadInner = textPrimary.withAlpha(light ? 0.13f : 0.15f);
        vizMedian = textSecondary.withAlpha(0.8f);
        vizTrack = textPrimary;
        vizReference = vizCategory(6);
        vizGrid = textPrimary.withAlpha(light ? 0.08f : 0.06f);
        vizAxis = textPrimary.withAlpha(light ? 0.25f : 0.18f);
    }

    // STU-6: the shared-core widgets' palette (StudioMeters, KhensuKnob,
    // ChartKit, EqCurveComponent ...) follows the same theme, before any
    // listener below reads it.
    khensu::ui::Palette::setTheme(t == Theme::HighContrast ? khensu::ui::Palette::Theme::HighContrast
                                  : t == Theme::Light      ? khensu::ui::Palette::Theme::Light
                                                           : khensu::ui::Palette::Theme::Dark);

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
