// UX-16: text is at least 11 px and tertiary text is readable in every theme.
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/KhensuTheme.h"
#include <cmath>

using khensu::KhensuTheme;

namespace {
double luminance(juce::Colour c)
{
    auto lin = [](float v) { return v <= 0.03928f ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.getFloatRed()) + 0.7152 * lin(c.getFloatGreen()) + 0.0722 * lin(c.getFloatBlue());
}
double contrast(juce::Colour a, juce::Colour b)
{
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}
} // namespace

TEST_CASE("No theme font is smaller than 11 px (UX-16)", "[theme][ux16]")
{
    juce::ScopedJuceInitialiser_GUI init;
    CHECK(KhensuTheme::labelFont(9.0f).getHeight() >= KhensuTheme::kMinTextPx - 0.01f);
    CHECK(KhensuTheme::uiFont(9.5f, juce::Font::bold).getHeight() >= KhensuTheme::kMinTextPx - 0.01f);
    CHECK(KhensuTheme::monoFont(8.0f).getHeight() >= KhensuTheme::kMinTextPx - 0.01f);
    CHECK(KhensuTheme::bodyFont(14.0f).getHeight() > 13.9f); // larger sizes are untouched
}

TEST_CASE("Tertiary text reaches 4.5:1 on every surface it sits on, in all themes (UX-16)", "[theme][ux16]")
{
    juce::ScopedJuceInitialiser_GUI init;
    for (auto theme : {KhensuTheme::Theme::Dark, KhensuTheme::Theme::HighContrast, KhensuTheme::Theme::Light}) {
        KhensuTheme::setTheme(theme);
        INFO("theme " << (int)theme);
        CHECK(contrast(KhensuTheme::textTertiary, KhensuTheme::bgBase) >= 4.5);
        CHECK(contrast(KhensuTheme::textTertiary, KhensuTheme::bgSurface) >= 4.5);
        CHECK(contrast(KhensuTheme::textTertiary, KhensuTheme::bgRaised) >= 4.5);
    }
    KhensuTheme::setTheme(KhensuTheme::Theme::Dark);
}
