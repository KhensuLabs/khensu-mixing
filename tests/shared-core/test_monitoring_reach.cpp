// NEW-10: a finding below what the user's monitoring reproduces is marked.
#include <catch2/catch_test_macros.hpp>
#include <khensu/recommendations/MonitoringReach.h>

using namespace khensu;

namespace {
Recommendation inBand(float lo, float hi)
{
    Recommendation r;
    r.freq_range = FrequencyRange{lo, hi};
    return r;
}
} // namespace

TEST_CASE("Monitoring reach marks findings the monitoring cannot reproduce", "[monitoring][new10]")
{
    const auto sub = inBand(20.0f, 60.0f);      // sub-bass
    const auto lowMid = inBand(60.0f, 250.0f);  // low end
    const auto presence = inBand(2000.0f, 5000.0f);

    CHECK(monitoring::reach(sub, "nearfield").outOfReach);
    CHECK_FALSE(monitoring::reach(sub, "full-range").outOfReach);
    CHECK_FALSE(monitoring::reach(sub, "headphones").outOfReach);
    CHECK(monitoring::reach(lowMid, "laptop").outOfReach);
    CHECK_FALSE(monitoring::reach(lowMid, "nearfield").outOfReach);
    CHECK_FALSE(monitoring::reach(presence, "phone").outOfReach);

    // Nothing is marked when the monitoring is not stated or unknown, or the finding is broadband.
    CHECK_FALSE(monitoring::reach(sub, "").outOfReach);
    CHECK_FALSE(monitoring::reach(sub, "boombox").outOfReach);
    CHECK_FALSE(monitoring::reach(Recommendation{}, "phone").outOfReach);

    const auto r = monitoring::reach(lowMid, "laptop");
    CHECK(r.limitHz == 200.0f);
    CHECK(std::string(r.monitoring->label) == "Laptop speakers");
}

TEST_CASE("Monitoring profiles have unique ids, unstated first", "[monitoring][new10]")
{
    CHECK(std::string(monitoring::kProfiles[0].id).empty());
    for (size_t i = 0; i < std::size(monitoring::kProfiles); ++i)
        for (size_t j = i + 1; j < std::size(monitoring::kProfiles); ++j)
            CHECK(std::string(monitoring::kProfiles[i].id) != monitoring::kProfiles[j].id);
}
