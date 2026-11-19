// tests/ui/HeatSpectrumTest.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../../desktop-app/Source/ui/shared/HeatSpectrumComponent.h"

using namespace khensu;

TEST_CASE("HeatSpectrumComponent colour gradient mapping", "[spectrum]")
{
    SECTION("top ranked bar is hot red")
    {
        auto c = HeatSpectrumComponent::colourForRank(1.0f);
        REQUIRE(c == juce::Colour(0xffff3030));
    }
    SECTION("bottom ranked bar is cool blue")
    {
        auto c = HeatSpectrumComponent::colourForRank(0.0f);
        REQUIRE(c == juce::Colour(0xff2255aa));
    }
    SECTION("mid ranked bar is amber")
    {
        // rank 0.6 is in amber zone (0.5-0.75)
        auto c = HeatSpectrumComponent::colourForRank(0.6f);
        // amber: 0xfff5c842 - just check it's not red or blue
        REQUIRE(c.getRed() > 200);
        REQUIRE(c.getBlue() < 100);
        REQUIRE(c.getGreen() > 150);
    }
}

TEST_CASE("HeatSpectrumComponent band ranking", "[spectrum]")
{
    std::vector<float> bands = {-30.0f, -10.0f, -20.0f, -5.0f, -25.0f};

    SECTION("highest energy band gets rank 1.0")
    {
        auto ranks = HeatSpectrumComponent::computeRanks(bands);
        // -5.0 is the highest → rank 1.0
        REQUIRE(ranks[3] == Catch::Approx(1.0f));
    }
    SECTION("lowest energy band gets rank 0.0")
    {
        auto ranks = HeatSpectrumComponent::computeRanks(bands);
        // -30.0 is the lowest → rank 0.0
        REQUIRE(ranks[0] == Catch::Approx(0.0f));
    }
}
