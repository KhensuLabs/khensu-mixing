// tests/ui/ZoneMeterTest.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../../desktop-app/Source/ui/shared/ZoneMeterComponent.h"

using namespace khensu;

TEST_CASE("ZoneMeterComponent normalisation", "[meter]")
{
    ZoneMeterComponent m;
    m.setRange(-24.0f, 0.0f);

    SECTION("min value normalises to 0") { REQUIRE(m.normalise(-24.0f) == Catch::Approx(0.0f)); }
    SECTION("max value normalises to 1") { REQUIRE(m.normalise(0.0f) == Catch::Approx(1.0f)); }
    SECTION("mid value normalises correctly") { REQUIRE(m.normalise(-12.0f) == Catch::Approx(0.5f)); }
    SECTION("value clamped below min") { REQUIRE(m.normalise(-30.0f) == Catch::Approx(0.0f)); }
    SECTION("value clamped above max") { REQUIRE(m.normalise(3.0f) == Catch::Approx(1.0f)); }
}

TEST_CASE("ZoneMeterComponent fill colour by zone", "[meter]")
{
    ZoneMeterComponent m;
    m.setRange(-24.0f, 0.0f);
    m.setTargetZone(-16.0f, -9.0f); // pass zone

    SECTION("value in target zone returns teal") { REQUIRE(m.fillColourForValue(-12.0f) == KhensuTheme::accentTeal); }
    SECTION("value below target zone returns amber")
    {
        REQUIRE(m.fillColourForValue(-20.0f) == KhensuTheme::accentAmber);
    }
    SECTION("value above target zone returns red") { REQUIRE(m.fillColourForValue(-2.0f) == KhensuTheme::accentRed); }
}
