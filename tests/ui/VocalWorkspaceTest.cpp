// tests/ui/VocalWorkspaceTest.cpp
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Stem isolation state transitions", "[vocal]")
{
    enum class StemState { None, Running, Ready };

    auto next = [](StemState s, bool success) -> StemState {
        if (s == StemState::None) return StemState::Running;
        if (s == StemState::Running) return success ? StemState::Ready : StemState::None;
        return s;
    };

    SECTION("None -> Running on start") { REQUIRE(next(StemState::None, true) == StemState::Running); }
    SECTION("Running -> Ready on success") { REQUIRE(next(StemState::Running, true) == StemState::Ready); }
    SECTION("Running -> None on failure") { REQUIRE(next(StemState::Running, false) == StemState::None); }
}
