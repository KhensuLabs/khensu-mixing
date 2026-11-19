// tests/ui/AbPreferenceNudgeTest.cpp
// Unit tests for nudgeLufsOffsetTowardAbPreference() - the pure decision math
// behind AppController::learnFromAbPreference(), extracted so it's testable
// without a live AppController (feedbackMgr/sessionState_/database). See
// TODO.md "desktop matched-level A/B regression tests covering accept/reject".
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "AbPreferenceMath.h"

using namespace khensu;
using Catch::Matchers::WithinAbs;

TEST_CASE("nudgeLufsOffsetTowardAbPreference: picked side louder than target nudges up by one step",
          "[appcontroller]")
{
    // currentOffset=0, target=-14, picked=-10 (louder than target) -> implied
    // offset (+4) is above current (0), so the offset steps up by stepDb.
    const auto result = nudgeLufsOffsetTowardAbPreference(0.0f, -10.0f, -14.0f, 0.3f);
    CHECK_THAT(result, WithinAbs(0.3f, 1e-4f));
}

TEST_CASE("nudgeLufsOffsetTowardAbPreference: picked side quieter than target nudges down by one step",
          "[appcontroller]")
{
    // implied offset (-4) is below current (0) -> steps down.
    const auto result = nudgeLufsOffsetTowardAbPreference(0.0f, -18.0f, -14.0f, 0.3f);
    CHECK_THAT(result, WithinAbs(-0.3f, 1e-4f));
}

TEST_CASE("nudgeLufsOffsetTowardAbPreference: already at the implied offset does not move",
          "[appcontroller]")
{
    // currentOffset already equals impliedOffset exactly -> no nudge either way.
    const auto result = nudgeLufsOffsetTowardAbPreference(2.0f, -12.0f, -14.0f, 0.3f);
    CHECK_THAT(result, WithinAbs(2.0f, 1e-4f));
}

TEST_CASE("nudgeLufsOffsetTowardAbPreference: clamps at +4 dB regardless of how far the implied offset sits",
          "[appcontroller]")
{
    // Starting already at +3.9, one more up-step would land at 4.2 - must clamp to 4.0.
    const auto result = nudgeLufsOffsetTowardAbPreference(3.9f, 10.0f, -14.0f, 0.3f);
    CHECK_THAT(result, WithinAbs(4.0f, 1e-4f));
}

TEST_CASE("nudgeLufsOffsetTowardAbPreference: clamps at -4 dB regardless of how far the implied offset sits",
          "[appcontroller]")
{
    const auto result = nudgeLufsOffsetTowardAbPreference(-3.9f, -30.0f, -14.0f, 0.3f);
    CHECK_THAT(result, WithinAbs(-4.0f, 1e-4f));
}

TEST_CASE("nudgeLufsOffsetTowardAbPreference: repeated picks in the same direction walk the offset toward it",
          "[appcontroller]")
{
    // Simulates several A/B picks in a row all favoring a louder render -
    // each call's output feeds the next call's currentOffsetDb, same as
    // learnFromAbPreference() persisting via UserPreferenceModel between calls.
    float offset = 0.0f;
    for (int i = 0; i < 5; ++i)
        offset = nudgeLufsOffsetTowardAbPreference(offset, -10.0f, -14.0f, 0.3f);
    CHECK_THAT(offset, WithinAbs(1.5f, 1e-4f));
}
