// tests/ui/VizPaletteTest.cpp
// VIZ-F2: the data-visualisation palette is colour-blind checked, not eyeballed.
// For each theme's chart surface: every adjacent categorical pair stays apart
// under protanopia and deuteranopia (Machado 2009, full severity; OKLab
// delta-E x100 >= 8; tritanopia >= 5) and for normal vision (>= 15); the Dark and
// High-contrast steps clear 3:1 against their surface; the sequential ramp is
// monotonic in lightness; the diverging midpoint is a neutral grey.
#include <catch2/catch_test_macros.hpp>
#include <khensu/ui/VizPalette.h>
#include "../../desktop-app/Source/ui/shared/KhensuTheme.h"

#include <array>
#include <cmath>

using namespace khensu;
namespace viz = khensu::ui::viz;

namespace {

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<Vec3, 3>;

double toLinear(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

Vec3 linearRgb(juce::Colour c)
{
    return {toLinear(c.getRed() / 255.0), toLinear(c.getGreen() / 255.0), toLinear(c.getBlue() / 255.0)};
}

Vec3 apply(const Mat3& m, const Vec3& v)
{
    Vec3 o{};
    for (size_t i = 0; i < 3; ++i)
        o[i] = std::clamp(m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2], 0.0, 1.0);
    return o;
}

Vec3 oklab(const Vec3& rgb)
{
    const double l = std::cbrt(0.4122214708 * rgb[0] + 0.5363325363 * rgb[1] + 0.0514459929 * rgb[2]);
    const double m = std::cbrt(0.2119034982 * rgb[0] + 0.6806995451 * rgb[1] + 0.1073969566 * rgb[2]);
    const double s = std::cbrt(0.0883024619 * rgb[0] + 0.2817188376 * rgb[1] + 0.6299787005 * rgb[2]);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

double deltaE(const Vec3& a, const Vec3& b)
{
    const auto A = oklab(a), B = oklab(b);
    return 100.0 * std::sqrt((A[0] - B[0]) * (A[0] - B[0]) + (A[1] - B[1]) * (A[1] - B[1]) +
                             (A[2] - B[2]) * (A[2] - B[2]));
}

const Mat3 kProtan{{{0.152286, 1.052583, -0.204868}, {0.114503, 0.786281, 0.099216}, {-0.003882, -0.048116, 1.051998}}};
const Mat3 kDeutan{{{0.367322, 0.860646, -0.227968}, {0.280085, 0.672501, 0.047413}, {-0.011820, 0.042940, 0.968881}}};
const Mat3 kTritan{{{1.255528, -0.076749, -0.178779}, {-0.078411, 0.930809, 0.147602}, {0.004733, 0.691367, 0.303900}}};

double luminance(juce::Colour c)
{
    const auto v = linearRgb(c);
    return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
}

double contrast(juce::Colour a, juce::Colour b)
{
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

void checkCategorical(viz::Mode mode)
{
    for (int i = 0; i + 1 < viz::kCategoricalCount; ++i) {
        const auto a = linearRgb(viz::categorical(i, mode)), b = linearRgb(viz::categorical(i + 1, mode));
        INFO("slots " << i << " / " << i + 1 << (mode == viz::Mode::Light ? " light" : " dark"));
        CHECK(deltaE(a, b) >= 15.0);
        CHECK(deltaE(apply(kProtan, a), apply(kProtan, b)) >= 8.0);
        CHECK(deltaE(apply(kDeutan, a), apply(kDeutan, b)) >= 8.0);
        // Tritanopia (about 1 in 10 000) is reported, not gated, as in the
        // reference validator; it still may not collapse a pair.
        CHECK(deltaE(apply(kTritan, a), apply(kTritan, b)) >= 5.0);
    }
}

struct ThemeRestore {
    ~ThemeRestore() { KhensuTheme::setTheme(KhensuTheme::Theme::Dark); }
};

} // namespace

TEST_CASE("VIZ-F2: categorical slots stay apart for colour-blind and normal vision", "[viz][palette]")
{
    checkCategorical(viz::Mode::Dark);
    checkCategorical(viz::Mode::Light);
}

TEST_CASE("VIZ-F2: dark and high-contrast series clear 3:1 against their chart surface", "[viz][palette]")
{
    ThemeRestore restore;
    for (const auto theme : {KhensuTheme::Theme::Dark, KhensuTheme::Theme::HighContrast}) {
        KhensuTheme::setTheme(theme);
        for (int i = 0; i < viz::kCategoricalCount; ++i) {
            INFO(KhensuTheme::themeName(theme) << " slot " << i);
            CHECK(contrast(KhensuTheme::vizCategory(i), KhensuTheme::bgSurface) >= 3.0);
            CHECK(contrast(KhensuTheme::vizCategory(i), KhensuTheme::bgRaised) >= 3.0);
        }
    }
}

TEST_CASE("VIZ-F2: the theme tokens follow the theme", "[viz][palette]")
{
    ThemeRestore restore;
    KhensuTheme::setTheme(KhensuTheme::Theme::Light);
    CHECK(KhensuTheme::vizCategory(0) == viz::categorical(0, viz::Mode::Light));
    CHECK(KhensuTheme::vizReference == viz::categorical(6, viz::Mode::Light));
    KhensuTheme::setTheme(KhensuTheme::Theme::Dark);
    CHECK(KhensuTheme::vizCategory(0) == viz::categorical(0, viz::Mode::Dark));
    CHECK(KhensuTheme::vizReference == viz::categorical(6, viz::Mode::Dark));
    // Severity keeps its own colours: no series slot is a severity colour.
    for (int i = 0; i < viz::kCategoricalCount; ++i) {
        CHECK(KhensuTheme::vizCategory(i) != KhensuTheme::accentRed);
        CHECK(KhensuTheme::vizCategory(i) != KhensuTheme::accentAmber);
    }
}

TEST_CASE("VIZ-F2: sequential is monotonic and diverging is neutral at zero", "[viz][palette]")
{
    for (const auto mode : {viz::Mode::Dark, viz::Mode::Light}) {
        // "More" is more contrast against the surface on both themes.
        const juce::Colour surface = mode == viz::Mode::Light ? juce::Colours::white : juce::Colour(0xff16181b);
        double prev = 0.0;
        for (int i = 0; i <= 10; ++i) {
            const double c = contrast(viz::sequential(static_cast<float>(i) / 10.0f, mode), surface);
            CHECK(c >= prev - 1e-6);
            prev = c;
        }
        const auto mid = oklab(linearRgb(viz::diverging(0.0f, mode)));
        CHECK(std::hypot(mid[1], mid[2]) < 0.02); // grey: no hue at the midpoint
        CHECK(viz::diverging(-1.0f, mode) == juce::Colour(viz::kDivergingCool));
        CHECK(viz::diverging(1.0f, mode) == juce::Colour(viz::kDivergingWarm));
    }
}
