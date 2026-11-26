#include <catch2/catch_test_macros.hpp>
#include <khensu/ui/KhensuPalette.h>
#include "ui/PluginLookAndFeel.h"

TEST_CASE("PluginLookAndFeel uses shared semantic palette tokens", "[plugin][palette]")
{
    using khensu::PluginLookAndFeel;
    using khensu::ui::Palette;

    CHECK(PluginLookAndFeel::bgDark == Palette::bgBase);
    CHECK(PluginLookAndFeel::bgPanel == Palette::bgPanel);
    CHECK(PluginLookAndFeel::accentTeal == Palette::accentTeal);
    CHECK(PluginLookAndFeel::textPrimary == Palette::textPrimary);
    CHECK(PluginLookAndFeel::textMuted == Palette::textMuted);
    CHECK(PluginLookAndFeel::statusWarn == Palette::statusWarn);
    CHECK(PluginLookAndFeel::statusBad == Palette::statusBad);
    CHECK(PluginLookAndFeel::statusGood == Palette::statusGood);
    CHECK(PluginLookAndFeel::accentBlue == Palette::accentBlue);
}

// STU-6: the plugin's palette switch (VIS-9) carries the shared ui::Palette,
// so the shared-core widgets in the editor follow High contrast too.
TEST_CASE("PluginLookAndFeel::setPalette carries the shared Palette", "[plugin][palette]")
{
    using khensu::PluginLookAndFeel;
    using khensu::ui::Palette;

    PluginLookAndFeel::setPalette(PluginLookAndFeel::Palette::HighContrast);
    CHECK(Palette::currentTheme() == Palette::Theme::HighContrast);
    CHECK(PluginLookAndFeel::bgDark == Palette::bgBase);
    CHECK(PluginLookAndFeel::bgPanel == Palette::bgPanel);
    CHECK(PluginLookAndFeel::accentTeal == Palette::accentTeal);
    CHECK(PluginLookAndFeel::textPrimary == Palette::textPrimary);
    CHECK(PluginLookAndFeel::textMuted == Palette::textMuted);
    CHECK(PluginLookAndFeel::statusWarn == Palette::statusWarn);
    CHECK(PluginLookAndFeel::statusBad == Palette::statusBad);
    CHECK(PluginLookAndFeel::statusGood == Palette::statusGood);
    CHECK(PluginLookAndFeel::accentBlue == Palette::accentBlue);

    PluginLookAndFeel::setPalette(PluginLookAndFeel::Palette::Dark);
    CHECK(Palette::currentTheme() == Palette::Theme::Dark);
    CHECK(PluginLookAndFeel::textPrimary == Palette::textPrimary);
    CHECK(PluginLookAndFeel::bgDark == Palette::bgBase);
}
