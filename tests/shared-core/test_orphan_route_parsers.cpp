// tests/shared-core/test_orphan_route_parsers.cpp
// Audit 2026-11-06 phase 2: the typed parsers behind the new bridge wrappers
// for the formerly orphan routes (§9.2) tolerate partial / null / older
// responses, and seekTransport after shutdownBackgroundWorkers no longer
// resurrects a static-destruction join (CORE-14).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <khensu/ipc/HttpServiceBridge.h>
#include <juce_core/juce_core.h>

using Catch::Approx;
using khensu::ipc::HttpServiceBridge;

TEST_CASE("Orphan-route parsers tolerate empty and partial bodies", "[ipc][orphan-routes]")
{
    const juce::var empty(new juce::DynamicObject());

    const auto g = HttpServiceBridge::parseGrooveTiming(empty);
    CHECK(g.swingRatio == Approx(1.0)); // absent swing = straight, not 0
    CHECK(g.events.empty());

    const auto h = HttpServiceBridge::parseHarmonyTimeline(empty);
    CHECK_FALSE(h.initialKey.has_value());
    CHECK(h.progression.empty());

    const auto r = HttpServiceBridge::parseReverbTail(empty);
    CHECK_FALSE(r.rt60Sec.has_value());

    CHECK(HttpServiceBridge::parseStemPhase(empty).pairs.empty());
    CHECK(HttpServiceBridge::parseCodecPlatforms(empty).empty());
    CHECK(HttpServiceBridge::parseFingerprintCompare(empty).verdict.isEmpty());

    const auto v = HttpServiceBridge::parseVocalHarmony(empty);
    CHECK(v.eventCount == 0);
    CHECK_FALSE(v.lowQualityExtraction);
}

TEST_CASE("Harmony timeline parser drops null keys and no-chord rows", "[ipc][orphan-routes]")
{
    const auto body = juce::JSON::parse(
        R"({"initial_key":{"time_sec":0.0,"key":null,"mode":null},)"
        R"("key_changes":[{"time_sec":5.0,"key":"D","mode":"major"},{"time_sec":9.0,"key":null}],)"
        R"("progression":[{"time_sec":0.0,"chord":"N","confidence":0.0},{"time_sec":1.5,"chord":"Bb","confidence":0.42}]})");
    const auto h = HttpServiceBridge::parseHarmonyTimeline(body);
    CHECK_FALSE(h.initialKey.has_value());
    REQUIRE(h.keyChanges.size() == 1);
    CHECK(h.keyChanges[0].key == "D");
    REQUIRE(h.progression.size() == 1);
    CHECK(h.progression[0].chord == "Bb");
    CHECK(h.progression[0].timeSec == Approx(1.5));
}

TEST_CASE("Reverb tail parser keeps a measured RT60", "[ipc][orphan-routes]")
{
    const auto r = HttpServiceBridge::parseReverbTail(juce::JSON::parse(
        R"({"rt60_sec":1.25,"fit_r2":0.93,"tail_energy_ratio":0.2,"source_reverb_tail":"moderate","decay_segments":4})"));
    REQUIRE(r.rt60Sec.has_value());
    CHECK(*r.rt60Sec == Approx(1.25));
    CHECK(r.decaySegments == 4);
    CHECK(r.tailLabel == "moderate");
}

TEST_CASE("seekTransport after shutdownBackgroundWorkers is safe and re-shuts cleanly (CORE-14)",
          "[ipc][orphan-routes][shutdown]")
{
    HttpServiceBridge::shutdownBackgroundWorkers();
    {
        // Nothing listens here: the send fails fast and is only logged.
        HttpServiceBridge bridge("http://127.0.0.1:19968", 300, 0);
        bridge.seekTransport(1.5f); // lazily starts a fresh worker in the leaked slot
        bridge.seekTransport(2.5f); // latest wins
    }
    // A second explicit shutdown joins the recreated worker; no join is left
    // for static destruction either way.
    HttpServiceBridge::shutdownBackgroundWorkers();
    HttpServiceBridge::shutdownBackgroundWorkers(); // idempotent
    SUCCEED();
}
