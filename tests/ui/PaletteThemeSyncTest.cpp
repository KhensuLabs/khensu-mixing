// tests/ui/PaletteThemeSyncTest.cpp
// Enforces CLAUDE.md's sync contract between the minimal plugin-shared
// palette (khensu::ui::Palette, shared-core) and the desktop theme's Dark
// tokens (KhensuTheme). Audit 2026-07-16 finding F5: the values had drifted,
// so plugin-shared widgets rendered off-palette inside the DAW.
//
// Milestone 3 (2026-07-16) re-synced KhensuPalette.cpp to the Dark theme
// values, so this test is now enforcing: any future drift fails the build.
#include <catch2/catch_test_macros.hpp>
#include <khensu/ui/KhensuPalette.h>
#include <khensu/ui/ChartKitPlots.h>
#include <khensu/ui/KhensuKnob.h>
#include "KhensuTheme.h"

namespace {

// Semantic mapping between the two palettes (Palette has 11 tokens; the
// theme names differ). Settled during the 2026-07-16 audit:
//   bgBase     -> bgBase        (editor background)
//   bgPanel    -> bgSurface     (panel bodies)
//   accentTeal -> accentTeal
//   textPrimary -> textPrimary  (primary control text)
//   textMuted  -> textSecondary (muted-but-readable label text)
//   statusWarn -> accentAmber   (moderate severity)
//   statusBad  -> accentRed     (severe severity)
//   statusGood -> accentGreen   (strength / completed state)
//   accentBlue -> accentBlue    (minor severity / category)
struct Mapping {
    const char* name;
    const juce::Colour& palette;
    const juce::Colour& theme;
};

} // namespace

namespace {
void checkPaletteMatchesTheme()
{
    using khensu::KhensuTheme;
    using khensu::ui::Palette;
    const Mapping mappings[] = {
        {"bgBase", Palette::bgBase, KhensuTheme::bgBase},
        {"bgVoid", Palette::bgVoid, KhensuTheme::bgVoid},
        {"borderActive", Palette::borderActive, KhensuTheme::borderActive},
        {"bgPanel/bgSurface", Palette::bgPanel, KhensuTheme::bgSurface},
        {"accentTeal", Palette::accentTeal, KhensuTheme::accentTeal},
        {"textPrimary", Palette::textPrimary, KhensuTheme::textPrimary},
        {"textMuted/textSecondary", Palette::textMuted, KhensuTheme::textSecondary},
        {"statusWarn/accentAmber", Palette::statusWarn, KhensuTheme::accentAmber},
        {"statusBad/accentRed", Palette::statusBad, KhensuTheme::accentRed},
        {"statusGood/accentGreen", Palette::statusGood, KhensuTheme::accentGreen},
        {"accentBlue", Palette::accentBlue, KhensuTheme::accentBlue},
    };
    for (const auto& m : mappings) {
        INFO(KhensuTheme::themeName(KhensuTheme::currentTheme())
             << " " << m.name << ": Palette=" << m.palette.toDisplayString(true).toStdString()
             << " Theme=" << m.theme.toDisplayString(true).toStdString());
        CHECK(m.palette == m.theme);
    }
}
} // namespace

// STU-6: KhensuTheme::setTheme carries the shared palette with it, so every
// theme's shared tokens must equal the desktop's.
TEST_CASE("shared Palette follows KhensuTheme in every theme", "[palette]")
{
    using khensu::KhensuTheme;
    using khensu::ui::Palette;

    struct Pair {
        KhensuTheme::Theme desktop;
        Palette::Theme shared;
    };
    for (const auto& pair : {Pair{KhensuTheme::Theme::Dark, Palette::Theme::Dark},
                             Pair{KhensuTheme::Theme::HighContrast, Palette::Theme::HighContrast},
                             Pair{KhensuTheme::Theme::Light, Palette::Theme::Light}}) {
        KhensuTheme::setTheme(pair.desktop);
        CHECK(Palette::currentTheme() == pair.shared);
        checkPaletteMatchesTheme();
        // tokensFor() is the same table setTheme() applies.
        const auto t = Palette::tokensFor(pair.shared);
        CHECK(t.bgBase == Palette::bgBase);
        CHECK(t.textPrimary == Palette::textPrimary);
        CHECK(t.borderActive == Palette::borderActive);
    }
    KhensuTheme::setTheme(KhensuTheme::Theme::Dark);
    checkPaletteMatchesTheme();
}

TEST_CASE("Palette theme change notifies listeners and bumps the version", "[palette]")
{
    using khensu::KhensuTheme;
    using khensu::ui::Palette;

    KhensuTheme::setTheme(KhensuTheme::Theme::Dark);
    int calls = 0;
    juce::Colour seen;
    {
        khensu::ui::PaletteListener listener{[&] {
            ++calls;
            seen = Palette::textPrimary; // reassigned before the handlers run
        }};
        const auto before = Palette::version();
        KhensuTheme::setTheme(KhensuTheme::Theme::Light);
        CHECK(calls == 1);
        CHECK(Palette::version() != before);
        CHECK(seen == KhensuTheme::textPrimary);
        // Same theme again: no change, no call.
        Palette::setTheme(Palette::Theme::Light);
        CHECK(calls == 1);
    }
    KhensuTheme::setTheme(KhensuTheme::Theme::Dark);
    CHECK(calls == 1); // deregistered with the RAII listener
}

TEST_CASE("shared widgets re-read the palette after a theme change", "[palette]")
{
    using khensu::KhensuTheme;
    using khensu::ui::Palette;
    juce::ScopedJuceInitialiser_GUI gui;

    KhensuTheme::setTheme(KhensuTheme::Theme::Dark);
    khensu::ui::chart::RadarChart radar;   // follows the palette
    khensu::ui::chart::RadarChart fixed;   // a desktop-styled chart keeps its style
    auto custom = khensu::ui::chart::Style::fromPalette();
    custom.text = KhensuTheme::accentRed;
    fixed.setStyle(custom);
    khensu::ui::KhensuKnob knob("Amount");
    CHECK(radar.style().text == Palette::textPrimary);

    KhensuTheme::setTheme(KhensuTheme::Theme::Light);
    CHECK(radar.style().text == KhensuTheme::textPrimary);
    CHECK(radar.style().muted == KhensuTheme::textSecondary);
    CHECK(fixed.style().text == custom.text);
    CHECK(knob.getLookAndFeel().findColour(juce::Slider::rotarySliderFillColourId) == KhensuTheme::accentTeal);

    KhensuTheme::setTheme(KhensuTheme::Theme::HighContrast);
    CHECK(radar.style().text == KhensuTheme::textPrimary);
    CHECK(knob.getLookAndFeel().findColour(juce::Slider::rotarySliderFillColourId) == KhensuTheme::accentTeal);

    KhensuTheme::setTheme(KhensuTheme::Theme::Dark);
    CHECK(radar.style().text == KhensuTheme::textPrimary);
}
