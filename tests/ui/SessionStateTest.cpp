// tests/ui/SessionStateTest.cpp
// Unit coverage for the SessionState bundle extracted from AppController
// (audit M4). Verifies the session pointer + derived per-session fields reset
// together, so no state leaks across sessions.
#include <catch2/catch_test_macros.hpp>
#include "session/SessionState.h"

using khensu::SessionState;

TEST_CASE("SessionState starts empty")
{
    SessionState s;
    REQUIRE_FALSE(s.hasSession());
    REQUIRE(s.session == nullptr);
    REQUIRE_FALSE(s.vocalAnalysis.has_value());
    REQUIRE_FALSE(s.lastSeparationResult.has_value());
    REQUIRE(s.soloedStemName.isEmpty());
    REQUIRE_FALSE(s.separatingStems);
}

TEST_CASE("SessionState tracks an open session and derived state")
{
    SessionState s;
    s.session = std::make_unique<khensu::Session>();
    s.session->id = "abc";
    s.session->name = "My Mix";
    s.vocalAnalysis = khensu::AnalysisResult{};
    s.soloedStemName = "vocals";
    s.separatingStems = true;

    REQUIRE(s.hasSession());
    REQUIRE(s.session->id == "abc");
    REQUIRE(s.vocalAnalysis.has_value());
    REQUIRE(s.soloedStemName == "vocals");
    REQUIRE(s.separatingStems);
}

TEST_CASE("SessionState reset clears everything back to no-session")
{
    SessionState s;
    s.session = std::make_unique<khensu::Session>();
    s.vocalAnalysis = khensu::AnalysisResult{};
    s.soloedStemName = "drums";
    s.separatingStems = true;

    s.reset();

    REQUIRE_FALSE(s.hasSession());
    REQUIRE(s.session == nullptr);
    REQUIRE_FALSE(s.vocalAnalysis.has_value());
    REQUIRE_FALSE(s.lastSeparationResult.has_value());
    REQUIRE(s.soloedStemName.isEmpty());
    REQUIRE_FALSE(s.separatingStems);
}
