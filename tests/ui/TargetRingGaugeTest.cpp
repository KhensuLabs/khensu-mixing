#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/TargetRingGauge.h"

using namespace khensu;

TEST_CASE("target ring score expresses distance from target", "[target][gauge]")
{
    CHECK(TargetRingGauge::matchScore(-14.0f, -14.0f, 1.0f) == Catch::Approx(1.0f));
    CHECK(TargetRingGauge::matchScore(-15.5f, -14.0f, 1.0f) == Catch::Approx(0.5f));
    CHECK(TargetRingGauge::matchScore(-18.0f, -14.0f, 1.0f) == Catch::Approx(0.0f));
    CHECK(TargetRingGauge::matchScore(-14.0f, -14.0f, 0.0f) == Catch::Approx(0.0f));
}
