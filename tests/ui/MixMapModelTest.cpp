// tests/ui/MixMapModelTest.cpp
// Unified mix-map model: node placement/energy, time-localized masking
// edges, clash bands, and severity buckets.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/MixMapModel.h"

using Catch::Approx;
using khensu::MixMapModel;

namespace {
MixMapModel makeModel()
{
    MixMapModel m;
    MixMapModel::Pair vb{"vocals", "bass", 0.1, {0.0, 0.1, 0.0, 0.0}, {}};
    MixMapModel::Pair vo{"vocals", "other", 0.7, {0.0, 0.0, 0.9, 0.2}, {{10.0, 20.0, 0.95}, {20.0, 30.0, 0.3}}};
    m.setMasking({vb, vo}, {50.0, 200.0, 800.0, 3200.0});
    m.setEnergy({{2.0, {{"vocals", 0.5}, {"bass", 0.3}, {"other", 0.2}}},
                 {0.0, {{"vocals", 0.1}, {"bass", 0.7}, {"other", 0.2}}}});
    return m;
}
} // namespace

TEST_CASE("Mix map orders stems low to high and averages energy", "[mixmap]")
{
    const auto m = makeModel();
    const auto stems = m.stems();
    REQUIRE(stems.size() == 3);
    CHECK(stems[0] == "bass");
    CHECK(stems[2] == "vocals");
    CHECK(m.durationSec() == Approx(2.0)); // frames were sorted by time

    const auto avg = m.nodes();
    REQUIRE(avg.size() == 3);
    CHECK(avg[0].energy == Approx(0.5)); // bass mean of 0.7 and 0.3
    const auto at0 = m.nodes(0.0);
    CHECK(at0[0].energy == Approx(0.7));
    const auto late = m.nodes(99.0); // past the end -> last frame
    CHECK(late[2].energy == Approx(0.5));

    // Vocals collide around 800-3200 Hz, so they sit above their canonical
    // home's lower neighbours; bass has almost no masking -> near its home.
    CHECK(avg[0].centreHz < avg[1].centreHz);
    CHECK(avg[1].centreHz < avg[2].centreHz);
}

TEST_CASE("Mix map edges follow windowed collisions and report the clash band", "[mixmap]")
{
    const auto m = makeModel();
    auto overall = m.edges();
    REQUIRE(overall.size() == 2);
    CHECK(overall[0].b == "other"); // sorted worst first
    CHECK(overall[0].score == Approx(0.7));
    CHECK(overall[0].clashHz == Approx(std::sqrt(800.0 * 3200.0)));

    CHECK(m.edges(15.0)[0].score == Approx(0.95));
    CHECK(m.edges(25.0)[0].score == Approx(0.3));
    CHECK(m.edges(40.0)[0].score == Approx(0.7)); // no window: whole-song score
    CHECK(m.edges(-1.0, 0.5).size() == 1);        // min-score filter

    CHECK(MixMapModel::severity(0.65) == 3);
    CHECK(MixMapModel::severity(0.45) == 2);
    CHECK(MixMapModel::severity(0.25) == 1);
    CHECK(MixMapModel::severity(0.05) == 0);
    CHECK(MixMapModel::canonicalHz("Bass") < MixMapModel::canonicalHz("vocals"));
    CHECK(MixMapModel().empty());
}
