// tests/ui/MasterAssistantTest.cpp
// FEAT-2: the Master Assistant's macros over the mastering agent's chain.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "ui/ExportDelivery/MasterMacros.h"

using namespace khensu;
using Catch::Approx;
using master_macros::Chain;
using master_macros::Effect;

namespace {
const Effect* find(const Chain& c, const char* effect)
{
    for (const auto& e : c)
        if (e.effect == effect) return &e;
    return nullptr;
}
Chain agentChain()
{
    return {Effect{"LowShelfFilter", {{"cutoff_frequency_hz", 120.0f}, {"gain_db", 2.0f}}},
            Effect{"PeakFilter", {{"cutoff_frequency_hz", 3000.0f}, {"gain_db", -3.0f}, {"q", 1.0f}}},
            Effect{"Compressor", {{"threshold_db", -18.0f}, {"ratio", 3.0f}}},
            Effect{"Limiter", {{"threshold_db", -1.0f}, {"release_ms", 100.0f}}}};
}
} // namespace

TEST_CASE("neutral macros keep the agent's chain (FEAT-2)", "[master][macros]")
{
    const auto out = master_macros::apply(agentChain(), {});
    REQUIRE(out.size() == 4);
    CHECK(find(out, "PeakFilter")->parameters.at("gain_db") == Approx(-3.0f));
    CHECK(find(out, "Compressor")->parameters.at("ratio") == Approx(3.0f));
    CHECK(find(out, "Limiter")->parameters.at("threshold_db") == Approx(-1.0f));
    CHECK(find(out, "Gain") == nullptr);
    CHECK(find(out, "StereoWidener") == nullptr);
}

TEST_CASE("each macro moves its own stages (FEAT-2)", "[master][macros]")
{
    master_macros::Macros m;
    m.tone = 0.5f;
    m.punch = 1.5f;
    m.loudnessDb = 2.0f;
    m.widthPct = 20.0f;
    m.ceilingDbtp = -0.5f;
    const auto out = master_macros::apply(agentChain(), m);

    CHECK(find(out, "LowShelfFilter")->parameters.at("gain_db") == Approx(1.0f));
    CHECK(find(out, "PeakFilter")->parameters.at("gain_db") == Approx(-1.5f));
    CHECK(find(out, "PeakFilter")->parameters.at("q") == Approx(1.0f));     // only the gain
    CHECK(find(out, "Compressor")->parameters.at("ratio") == Approx(4.0f)); // 1 + (3-1) * 1.5
    CHECK(find(out, "StereoWidener")->parameters.at("width") == Approx(1.2f));
    CHECK(find(out, "Gain")->parameters.at("gain_db") == Approx(2.0f));
    CHECK(find(out, "Limiter")->parameters.at("threshold_db") == Approx(-0.5f));
    // Gain and width go in before the limiter, which stays last.
    REQUIRE(!out.empty());
    CHECK(out.back().effect == "Limiter");
    CHECK(out[out.size() - 2].effect == "Gain");
}

TEST_CASE("a chain without a limiter gets one at the ceiling (FEAT-2)", "[master][macros]")
{
    master_macros::Macros m;
    m.loudnessDb = -1.0f;
    const auto out = master_macros::apply({Effect{"Gain", {{"gain_db", 3.0f}}}}, m);
    REQUIRE(out.size() == 2);
    CHECK(out[0].parameters.at("gain_db") == Approx(2.0f)); // the existing gain into the limiter
    CHECK(out[1].effect == "Limiter");
    CHECK(out[1].parameters.at("threshold_db") == Approx(-1.0f));
}
