// tests/ui/MetricTileTest.cpp
#include <catch2/catch_test_macros.hpp>
#include "../../desktop-app/Source/ui/shared/MetricTileComponent.h"

TEST_CASE("MetricTileComponent stores metric correctly", "[tile]")
{
    khensu::MetricTileComponent t;
    t.setMetric("LUFS", "-13.0", "LUFS", khensu::BadgeStatus::Warn);
    REQUIRE(t.currentStatus() == khensu::BadgeStatus::Warn);
    REQUIRE(t.currentLabel() == "LUFS");
}
