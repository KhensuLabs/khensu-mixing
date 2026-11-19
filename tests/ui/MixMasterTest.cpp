// tests/ui/MixMasterTest.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

TEST_CASE("Safe gain recommendation", "[mixmaster]")
{
    // Given current peak and a target LUFS gain change,
    // predict whether clipping will occur
    auto willClip = [](float currentPeakDbtp, float gainChangeDb) -> bool {
        return (currentPeakDbtp + gainChangeDb) > -0.1f;
    };
    SECTION("safe gain boost")
    {
        REQUIRE_FALSE(willClip(-3.0f, 1.0f)); // -2.0 dBTP - safe
    }
    SECTION("unsafe gain boost causes clip")
    {
        REQUIRE(willClip(-0.5f, 1.0f)); // +0.5 dBTP - clips
    }
    SECTION("gain cut is always safe")
    {
        REQUIRE_FALSE(willClip(-0.3f, -2.0f)); // -2.3 dBTP - safe
    }
}

TEST_CASE("Streaming platform adjustment", "[mixmaster]")
{
    auto adjustment = [](float measuredLufs, float targetLufs) -> float {
        return targetLufs - measuredLufs; // negative = turned down
    };
    SECTION("Spotify: -14 LUFS target, mix at -13 LUFS -> turned down 1 dB")
    {
        REQUIRE(adjustment(-13.0f, -14.0f) == Catch::Approx(-1.0f));
    }
    SECTION("Apple Music: -16 LUFS target, mix at -13 LUFS -> turned down 3 dB")
    {
        REQUIRE(adjustment(-13.0f, -16.0f) == Catch::Approx(-3.0f));
    }
}
