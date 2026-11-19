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
#include "KhensuTheme.h"

namespace {

// Semantic mapping between the two palettes (Palette has 8 tokens; the
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

TEST_CASE("shared Palette matches KhensuTheme Dark tokens", "[palette]")
{
    using khensu::KhensuTheme;
    using khensu::ui::Palette;

    KhensuTheme::setTheme(KhensuTheme::Theme::Dark);

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
        INFO(m.name << ": Palette=" << m.palette.toDisplayString(true).toStdString()
                    << " Theme=" << m.theme.toDisplayString(true).toStdString());
        CHECK(m.palette == m.theme);
    }
}
