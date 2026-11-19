// tests/ui/ServiceContractRegressionTest.cpp
// Regressions from the 2026-10-16 C++ <-> Python JSON contract check: each
// case serves the payload shape the Python service actually emits and
// asserts the bridge reads it. The static half of the check (every key /
// status literal the bridge uses exists service-side) lives in
// local-services/tests/test_cpp_json_contract.py.
#include <catch2/catch_test_macros.hpp>
#include "support/FakeApplyChainServer.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../../desktop-app/Source/ui/shared/IsoTime.h"

using khensu::ipc::HttpServiceBridge;

TEST_CASE("Mixer-chain job poll reads the JobStore 'completed' status", "[ipc][contract]")
{
    // common/jobs.py settles every job as "completed" / "failed"; the poll
    // used to wait for "done", so a finished generation never surfaced.
    khensu::test::FakeApplyChainServer server(19851);
    server.setResponse("GET /copilot/generate_mixer_chain/status/mc-1",
                       {200, R"({"status":"completed","error":null,"result":{"description":"d",)"
                             R"("plugins":[{"name":"Fruity Limiter","category":"Dynamics",)"
                             R"("settings_description":"s","ecosystem":"Stock FL Studio"}],)"
                             R"("decision_hash":"abc","consensus":null}})"});
    server.setResponse("GET /copilot/generate_mixer_chain/status/mc-2",
                       {200, R"({"status":"failed","error":"boom","result":null})"});

    HttpServiceBridge bridge(server.baseUrl().toStdString(), 1000, 0);
    const auto done = bridge.getMixerChainJobStatus("mc-1");
    REQUIRE(done.is_ok());
    CHECK(done.value().status == "completed");
    REQUIRE(done.value().result.has_value());
    REQUIRE(done.value().result->plugins.size() == 1);
    CHECK(done.value().result->plugins[0].name == "Fruity Limiter");
    CHECK(done.value().result->decision_hash == "abc");

    const auto failed = bridge.getMixerChainJobStatus("mc-2");
    REQUIRE(failed.is_ok());
    CHECK(failed.value().status == "failed");
    CHECK(failed.value().error == "boom");
    CHECK_FALSE(failed.value().result.has_value());
}

TEST_CASE("Full-intake stage retry sends the same intake settings as the start", "[ipc][contract]")
{
    // The retry rebuilds the bundle under a cache key derived from these
    // fields; sending only file_path re-ran the stage with default genre
    // settings instead of the user's.
    khensu::test::FakeApplyChainServer server(19852);
    server.setResponse("POST /analyze/full_intake/retry/musical_context",
                       {200, R"({"stage":"musical_context","stage_status":{"ok":true},"result":{},"bundle":{}})"});
    HttpServiceBridge bridge(server.baseUrl().toStdString(), 1000, 0);
    const auto resp =
        bridge.retryFullIntakeStage("C:/track.wav", "musical_context", "trap", "C:/ref.wav", "stated", false, false);
    // The route returns the one stage's status directly, not a map by stage.
    CHECK((bool)resp["stage_status"]["ok"]);

    const auto sent = juce::JSON::parse(server.lastBody("POST /analyze/full_intake/retry/musical_context"));
    CHECK(sent["stated_genre_id"].toString() == "trap");
    CHECK(sent["reference_path"].toString() == "C:/ref.wav");
    CHECK(sent["selection_mode"].toString() == "stated");
    CHECK_FALSE((bool)sent["use_genre_targets"]);
}

TEST_CASE("Deep humanize forwards a caller-chosen output path", "[ipc][contract]")
{
    // Without output_path the service writes a fixed deep_humanized.wav that
    // every run overwrites (AUDIT-018 hazard).
    khensu::test::FakeApplyChainServer server(19853);
    server.setResponse("POST /humanize/deep", {200, R"({"output_path":"C:/stems/v_deep_humanized_1.wav"})"});
    HttpServiceBridge bridge(server.baseUrl().toStdString(), 1000, 0);
    const auto r = bridge.deepHumanize({{"vocals", "C:/stems/v.wav"}}, {{"vocals", 0.5f}}, 0.5f, 0.3f, 4.0f, 0.8f,
                                       -72.0f, {}, {}, {}, {}, "C:/stems/v_deep_humanized_1.wav");
    REQUIRE(r.is_ok());
    const auto sent = juce::JSON::parse(server.lastBody("POST /humanize/deep"));
    CHECK(sent["output_path"].toString() == "C:/stems/v_deep_humanized_1.wav");

    // Omitted path: the key is not sent at all, so the service default applies.
    bridge.deepHumanize({{"vocals", "C:/stems/v.wav"}}, {{"vocals", 0.5f}}, 0.5f);
    CHECK_FALSE(
        juce::JSON::parse(server.lastBody("POST /humanize/deep")).getDynamicObject()->hasProperty("output_path"));
}

TEST_CASE("Service event timestamps are shown in local time", "[contract]")
{
    // events/db.py writes datetime.now(timezone.utc).isoformat().
    const auto utc = juce::Time(2026, 9, 16, 12, 34, 56, 0, false); // 2026-10-16 12:34:56 UTC
    CHECK(khensu::localHourMinute("2026-10-16T12:34:56.123456+00:00") == utc.formatted("%H:%M"));
    CHECK(khensu::localHourMinute("2026-10-16T12:34:56+00:00") == utc.formatted("%H:%M"));
    CHECK(khensu::localHourMinute("not a time") == "not a time");
}

TEST_CASE("Playhead poll carries the service's connected flag", "[ipc][contract]")
{
    // router_transport.py returns placeholder meters (correlation 0, width 1)
    // when no plugin has posted recently; the desktop must be able to tell.
    khensu::test::FakeApplyChainServer server(19854);
    server.setResponse("GET /transport/playhead",
                       {200, R"({"age_ms":null,"connected":false,"position_sec":0.0,"is_playing":false,)"
                             R"("left_peak_db":-96.0,"right_peak_db":-96.0,"momentary_lufs":-96.0,)"
                             R"("short_term_lufs":-96.0,"correlation":0.0,"width":1.0})"});
    HttpServiceBridge bridge(server.baseUrl().toStdString(), 1000, 0);
    auto idle = bridge.getPlayhead();
    REQUIRE(idle.is_ok());
    CHECK_FALSE(idle.value().connected);

    server.setResponse("GET /transport/playhead",
                       {200, R"({"age_ms":40,"connected":true,"position_sec":1.5,"is_playing":true,)"
                             R"("left_peak_db":-6.0,"right_peak_db":-6.5,"momentary_lufs":-12.0,)"
                             R"("short_term_lufs":-13.0,"correlation":0.8,"width":0.4})"});
    auto live = bridge.getPlayhead();
    REQUIRE(live.is_ok());
    CHECK(live.value().connected);
    CHECK(live.value().correlation > 0.79f);
}
