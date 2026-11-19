// tests/ui/ExportScreenTest.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <atomic>

TEST_CASE("Export cancel flag", "[export]")
{
    std::atomic<bool> cancelRequested{false};

    SECTION("default is not cancelled") { REQUIRE_FALSE(cancelRequested.load()); }
    SECTION("setting flag cancels")
    {
        cancelRequested.store(true);
        REQUIRE(cancelRequested.load());
    }
    SECTION("flag can be reset")
    {
        cancelRequested.store(true);
        cancelRequested.store(false);
        REQUIRE_FALSE(cancelRequested.load());
    }
}

TEST_CASE("Predicted peak after gain", "[export]")
{
    auto predictedPeak = [](float currentPeak, float gainChange) { return currentPeak + gainChange; };
    REQUIRE(predictedPeak(-0.3f, -1.0f) == Catch::Approx(-1.3f));
    REQUIRE(predictedPeak(-0.3f, +1.0f) == Catch::Approx(+0.7f));
}
