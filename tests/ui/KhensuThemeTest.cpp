// tests/ui/KhensuThemeTest.cpp
#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include "../../desktop-app/Source/ui/shared/KhensuTheme.h"

namespace {
double relativeLuminance(juce::Colour colour)
{
    const auto linear = [](float channel) {
        return channel <= 0.04045f ? channel / 12.92 : std::pow((channel + 0.055f) / 1.055f, 2.4);
    };
    return 0.2126 * linear(colour.getFloatRed())
           + 0.7152 * linear(colour.getFloatGreen())
           + 0.0722 * linear(colour.getFloatBlue());
}

double contrastRatio(juce::Colour first, juce::Colour second)
{
    const double a = relativeLuminance(first);
    const double b = relativeLuminance(second);
    return (juce::jmax(a, b) + 0.05) / (juce::jmin(a, b) + 0.05);
}
} // namespace

TEST_CASE("KhensuTheme colour tokens", "[theme]")
{
    SECTION("bgVoid is near-black graphite (UI-3)") { REQUIRE(khensu::KhensuTheme::bgVoid == juce::Colour(0xff0b0c0e)); }
    SECTION("accentTeal is correct (Aurora Glass cyan)")
    {
        REQUIRE(khensu::KhensuTheme::accentTeal == juce::Colour(0xff22d3c5));
    }
    SECTION("accentTealDim has 20% alpha") { REQUIRE(khensu::KhensuTheme::accentTealDim.getAlpha() == 0x33); }
    SECTION("accentMagenta stays a categorical data colour")
    {
        REQUIRE(khensu::KhensuTheme::accentMagenta == juce::Colour(0xfff0308f));
    }
    SECTION("borderSubtle has ~8% alpha") { REQUIRE(khensu::KhensuTheme::borderSubtle.getAlpha() == 0x14); }
    SECTION("spacing base unit is 8")
    {
        REQUIRE(khensu::KhensuTheme::sp2 == 8);
        REQUIRE(khensu::KhensuTheme::sp3 == 12);
        REQUIRE(khensu::KhensuTheme::panelPadding == 24); // UI-4: generous padding
        REQUIRE(khensu::KhensuTheme::cardGap == 16);
        REQUIRE(khensu::KhensuTheme::sectionGap == 32);
    }
}

// Theme-change notification (kimi audit P0 #1). Surfaces that CACHE a palette
// colour - a Label's setColour, a LookAndFeel's JUCE colour IDs - can't see a
// palette reassignment on their own; they rely on these callbacks firing.
TEST_CASE("KhensuTheme theme listeners", "[theme]")
{
    const auto original = khensu::KhensuTheme::currentTheme();

    SECTION("a registered listener fires on every setTheme")
    {
        int calls = 0;
        const int token = khensu::KhensuTheme::addThemeListener([&calls] { ++calls; });

        khensu::KhensuTheme::setTheme(khensu::KhensuTheme::Theme::Light);
        REQUIRE(calls == 1);
        khensu::KhensuTheme::setTheme(khensu::KhensuTheme::Theme::HighContrast);
        REQUIRE(calls == 2);

        khensu::KhensuTheme::removeThemeListener(token);
    }

    SECTION("the palette is already updated when the listener runs")
    {
        // Ordering matters: a handler re-applies colours FROM the statics, so
        // firing before the reassignment would just re-cache the old palette.
        juce::Colour seen;
        const int token =
            khensu::KhensuTheme::addThemeListener([&seen] { seen = khensu::KhensuTheme::textPrimary; });

        khensu::KhensuTheme::setTheme(khensu::KhensuTheme::Theme::Light);
        REQUIRE(seen == khensu::KhensuTheme::textPrimary);
        REQUIRE(seen != juce::Colour(0xffeaf0f4)); // not the Dark-theme value

        khensu::KhensuTheme::removeThemeListener(token);
    }

    SECTION("a removed listener stops firing")
    {
        int calls = 0;
        const int token = khensu::KhensuTheme::addThemeListener([&calls] { ++calls; });
        khensu::KhensuTheme::setTheme(khensu::KhensuTheme::Theme::Light);
        REQUIRE(calls == 1);

        khensu::KhensuTheme::removeThemeListener(token);
        khensu::KhensuTheme::setTheme(khensu::KhensuTheme::Theme::Dark);
        REQUIRE(calls == 1); // unchanged - a dangling handler here would use freed state
    }

    SECTION("removing an unknown or already-removed token is harmless")
    {
        int removedCalls = 0;
        int liveCalls = 0;
        const int live = khensu::KhensuTheme::addThemeListener([&liveCalls] { ++liveCalls; });
        khensu::KhensuTheme::removeThemeListener(0);
        khensu::KhensuTheme::removeThemeListener(999999);
        const int token = khensu::KhensuTheme::addThemeListener([&removedCalls] { ++removedCalls; });
        khensu::KhensuTheme::removeThemeListener(token);
        khensu::KhensuTheme::removeThemeListener(token);

        // The bogus/double removals neither crashed nor disturbed the
        // remaining registration: the live listener still fires exactly once
        // per change and the removed one never does.
        khensu::KhensuTheme::setTheme(khensu::KhensuTheme::Theme::Light);
        REQUIRE(liveCalls == 1);
        REQUIRE(removedCalls == 0);

        khensu::KhensuTheme::removeThemeListener(live);
        khensu::KhensuTheme::setTheme(khensu::KhensuTheme::Theme::Dark);
        REQUIRE(liveCalls == 1);
    }

    SECTION("a listener may deregister itself while the callbacks are running")
    {
        // setTheme() iterates a COPY for exactly this case - a panel rebuilding
        // its children mid-callback mutates the listener map.
        int calls = 0;
        int self = 0;
        self = khensu::KhensuTheme::addThemeListener([&calls, &self] {
            ++calls;
            khensu::KhensuTheme::removeThemeListener(self);
        });

        khensu::KhensuTheme::setTheme(khensu::KhensuTheme::Theme::Light);
        REQUIRE(calls == 1);
        khensu::KhensuTheme::setTheme(khensu::KhensuTheme::Theme::Dark);
        REQUIRE(calls == 1); // it removed itself, so the second switch is a no-op
    }

    khensu::KhensuTheme::setTheme(original);
}

TEST_CASE("KhensuTheme light theme layer separation", "[theme]")
{
    const auto original = khensu::KhensuTheme::currentTheme();
    khensu::KhensuTheme::setTheme(khensu::KhensuTheme::Theme::Light);

    // bgBase and bgSurface were both pure white, so a panel had no edge against
    // its parent (kimi audit P2 #39). Every adjacent layer must stay distinct.
    REQUIRE(khensu::KhensuTheme::bgBase != khensu::KhensuTheme::bgSurface);
    REQUIRE(khensu::KhensuTheme::bgVoid != khensu::KhensuTheme::bgBase);
    REQUIRE(khensu::KhensuTheme::bgSurface != khensu::KhensuTheme::bgRaised);

    khensu::KhensuTheme::setTheme(original);
}

TEST_CASE("KhensuTheme tertiary text meets WCAG AA contrast", "[theme][accessibility]")
{
    const auto original = khensu::KhensuTheme::currentTheme();
    const auto requireAaTertiaryContrast = [](khensu::KhensuTheme::Theme theme) {
        khensu::KhensuTheme::setTheme(theme);
        REQUIRE(contrastRatio(khensu::KhensuTheme::textTertiary,
                              khensu::KhensuTheme::bgBase) >= 4.5);
    };

    requireAaTertiaryContrast(khensu::KhensuTheme::Theme::Dark);
    requireAaTertiaryContrast(khensu::KhensuTheme::Theme::HighContrast);
    requireAaTertiaryContrast(khensu::KhensuTheme::Theme::Light);

    khensu::KhensuTheme::setTheme(original);
}

TEST_CASE("KhensuTheme zone colour for status", "[theme]")
{
    SECTION("pass returns green")
    {
        REQUIRE(khensu::KhensuTheme::colourForStatus(khensu::BadgeStatus::Pass) == khensu::KhensuTheme::accentGreen);
    }
    SECTION("warn returns amber")
    {
        REQUIRE(khensu::KhensuTheme::colourForStatus(khensu::BadgeStatus::Warn) == khensu::KhensuTheme::accentAmber);
    }
    SECTION("fail returns red")
    {
        REQUIRE(khensu::KhensuTheme::colourForStatus(khensu::BadgeStatus::Fail) == khensu::KhensuTheme::accentRed);
    }
}

TEST_CASE("KhensuTheme type scale is four steps with an 11 px floor (UI-4)", "[theme]")
{
    using khensu::KhensuTheme;
    CHECK(KhensuTheme::kFontMetric == 28.0f);
    CHECK(KhensuTheme::kFontH1 == 17.0f);
    CHECK(KhensuTheme::kFontBody == 13.0f);
    CHECK(KhensuTheme::kFontLabel == 11.0f);
    CHECK(KhensuTheme::kMinTextPx == 11.0f);
    CHECK(KhensuTheme::numericFont(8.0f).getHeight() >= KhensuTheme::kMinTextPx);
}

TEST_CASE("KhensuTheme numeric font draws every digit at one width (UI-4)", "[theme]")
{
    const auto font = khensu::KhensuTheme::numericFont(13.0f);
    const float one = juce::GlyphArrangement::getStringWidth(font, "1111");
    const float eight = juce::GlyphArrangement::getStringWidth(font, "8888");
    CHECK(std::abs(one - eight) < 0.5f);
}
