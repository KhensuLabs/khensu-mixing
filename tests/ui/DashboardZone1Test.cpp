// tests/ui/DashboardZone1Test.cpp
#include <catch2/catch_test_macros.hpp>
#include "../../desktop-app/Source/ui/shared/MetricTileComponent.h"
#include "../../desktop-app/Source/ui/shared/BadgeComponent.h"

TEST_CASE("MetricTile status from LUFS value", "[dashboard][zone1]")
{
    // Streaming target is roughly -16 to -9; below -16 is under, above -9 is hot
    auto statusForLufs = [](float lufs) -> khensu::BadgeStatus {
        if (lufs >= -16.0f && lufs <= -9.0f) return khensu::BadgeStatus::Pass;
        if (lufs > -9.0f) return khensu::BadgeStatus::Fail;
        return khensu::BadgeStatus::Warn;
    };
    REQUIRE(statusForLufs(-13.0f) == khensu::BadgeStatus::Pass);
    REQUIRE(statusForLufs(-5.0f) == khensu::BadgeStatus::Fail);
    REQUIRE(statusForLufs(-20.0f) == khensu::BadgeStatus::Warn);
}

TEST_CASE("MetricTile status from TruePeak value", "[dashboard][zone1]")
{
    auto statusForPeak = [](float peak) -> khensu::BadgeStatus {
        return (peak > -1.0f) ? khensu::BadgeStatus::Fail : khensu::BadgeStatus::Pass;
    };
    REQUIRE(statusForPeak(-0.3f) == khensu::BadgeStatus::Fail);
    REQUIRE(statusForPeak(-1.5f) == khensu::BadgeStatus::Pass);
}
