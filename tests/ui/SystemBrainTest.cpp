// tests/ui/SystemBrainTest.cpp
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

TEST_CASE("Feature list search filter", "[systembrain]")
{
    std::vector<std::string> features = {"Integrated LUFS", "Loudness Range", "True Peak", "Spectral Centroid",
                                         "Phase Correlation"};

    auto filter = [&](const std::string& query) -> std::vector<std::string> {
        std::vector<std::string> out;
        for (const auto& f : features)
            if (query.empty() || f.find(query) != std::string::npos) out.push_back(f);
        return out;
    };

    SECTION("empty query returns all") { REQUIRE(filter("").size() == 5); }
    SECTION("exact match returns 1") { REQUIRE(filter("Integrated LUFS").size() == 1); }
    SECTION("partial match works") { REQUIRE(filter("Phase").size() == 1); }
    SECTION("no match returns empty") { REQUIRE(filter("ZZZZ").size() == 0); }
}
