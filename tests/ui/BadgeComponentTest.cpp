// tests/ui/BadgeComponentTest.cpp
#include <catch2/catch_test_macros.hpp>
#include "../../desktop-app/Source/ui/shared/BadgeComponent.h"
#include "../../desktop-app/Source/ui/shared/KhensuTheme.h"

TEST_CASE("BadgeComponent colour selection", "[badge]")
{
    SECTION("Pass status uses green")
    {
        REQUIRE(khensu::KhensuTheme::colourForStatus(khensu::BadgeStatus::Pass) == khensu::KhensuTheme::accentGreen);
    }
    SECTION("Warn status uses amber")
    {
        REQUIRE(khensu::KhensuTheme::colourForStatus(khensu::BadgeStatus::Warn) == khensu::KhensuTheme::accentAmber);
    }
    SECTION("Fail status uses red")
    {
        REQUIRE(khensu::KhensuTheme::colourForStatus(khensu::BadgeStatus::Fail) == khensu::KhensuTheme::accentRed);
    }
}
