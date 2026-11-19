// tests/ui/SectionContrastTest.cpp
// Loudness-arc contrast with the section labels the service actually emits
// (A/B/C clusters), not only hand-named verse/chorus parts.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "ui/Dashboard/SectionContrast.h"

using namespace khensu::section_contrast;
using Catch::Approx;

TEST_CASE("Named verse/chorus sections use the role labels", "[ui][loudness_arc]")
{
    const auto r = compute(
        {{"Intro", -22.0f}, {"Verse 1", -18.0f}, {"Chorus 1", -14.0f}, {"Verse 2", -18.0f}, {"Chorus 2", -14.0f}});
    CHECK(r.basis == Basis::RoleLabels);
    CHECK(r.liftDb == Approx(4.0f));
}

TEST_CASE("Repeated A/B clusters infer chorus and verse roles", "[ui][loudness_arc]")
{
    // A B A B C B: B repeats most and is loudest -> chorus role; A is the
    // repeated quieter part -> verse role. C (a one-off bridge) is ignored.
    const auto r = compute({{"A", -18.0f}, {"B", -13.0f}, {"A", -17.0f}, {"B", -14.0f}, {"C", -16.0f}, {"B", -13.5f}});
    CHECK(r.basis == Basis::RepeatStructure);
    CHECK(r.liftLabel == "B");
    CHECK(r.baseLabel == "A");
    CHECK(r.liftDb == Approx(-13.5f - -17.5f));
}

TEST_CASE("Repeat inference ignores occurrence suffixes and picks the most-repeated base", "[ui][loudness_arc]")
{
    // Intro (A) once, verse-like B three times, loud C twice, quiet D twice.
    const auto r = compute({{"A", -24.0f},
                            {"B1", -19.0f},
                            {"C1", -12.0f},
                            {"B2", -19.0f},
                            {"D1", -21.0f},
                            {"C2", -12.0f},
                            {"D2", -21.0f},
                            {"B3", -19.0f}});
    CHECK(r.basis == Basis::RepeatStructure);
    CHECK(r.liftLabel == "C");
    CHECK(r.baseLabel == "B"); // 3 repeats beats D's 2
    CHECK(r.liftDb == Approx(7.0f));
}

TEST_CASE("Without repeats the contrast falls back to peak vs median", "[ui][loudness_arc]")
{
    const auto r = compute({{"A", -20.0f}, {"B", -16.0f}, {"C", -18.0f}, {"D", -14.0f}});
    CHECK(r.basis == Basis::PeakVsMedian);
    CHECK(r.liftDb == Approx(-14.0f - -16.0f));
}

TEST_CASE("Too few sections yield no contrast", "[ui][loudness_arc]")
{
    CHECK(compute({{"A", -18.0f}, {"B", -14.0f}}).basis == Basis::None);
}
