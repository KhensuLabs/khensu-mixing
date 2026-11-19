#include <catch2/catch_test_macros.hpp>
#include "support/FakeApplyChainServer.h"
#include <khensu/ipc/HttpServiceBridge.h>

using khensu::ipc::HttpServiceBridge;
using khensu::ipc::ILocalServiceBridge;

TEST_CASE("HttpServiceBridge builds dynamic analysis, separation, and intake job routes", "[ipc][integration]")
{
    khensu::test::FakeApplyChainServer server(19724);
    server.setResponse("POST /analyze/start", {200, R"({"job_id":"analysis-1"})"});
    server.setResponse("GET /analyze/status/analysis-1", {200, R"({"status":"completed","stage":"done","stage_index":1,"total_stages":1,"result":{}})"});
    server.setResponse("POST /analyze/analysis-1/cancel", {200, R"({"ok":true})"});
    server.setResponse("POST /separate/start", {200, R"({"job_id":"separate-1"})"});
    server.setResponse("GET /separate/status/separate-1", {200, R"({"status":"cancelled","progress":0.5})"});
    server.setResponse("POST /separate/separate-1/cancel", {200, R"({"ok":true})"});
    server.setResponse("POST /analyze/full_intake/start", {200, R"({"job_id":"intake-1"})"});
    server.setResponse("GET /analyze/full_intake/status/intake-1", {200, R"({"status":"running"})"});
    server.setResponse("POST /analyze/full_intake/cancel/intake-1", {200, R"({"ok":true})"});
    server.setResponse("POST /analyze/full_intake/retry/musical_context", {200, R"({"ok":true})"});

    HttpServiceBridge bridge(server.baseUrl().toStdString(), 1000, 0);
    const auto analysis = bridge.startAnalysisJob("C:/track.wav", ILocalServiceBridge::AnalysisMode::Advanced);
    REQUIRE(analysis.is_ok());
    CHECK(analysis.value() == "analysis-1");
    CHECK(bridge.getAnalysisProgress(analysis.value()).is_ok());
    bridge.cancelAnalysisJob(analysis.value());

    const auto separation = bridge.startSeparationJob("C:/track.wav", "C:/stems", "hpss");
    REQUIRE(separation.is_ok());
    CHECK(bridge.getSeparationProgress(separation.value()).is_ok());
    bridge.cancelSeparationJob(separation.value());

    const auto intake = bridge.startFullIntake("C:/track.wav");
    REQUIRE(intake["job_id"].toString() == "intake-1");
    CHECK(bridge.fullIntakeStatus("intake-1")["status"].toString() == "running");
    CHECK((bool)bridge.cancelFullIntake("intake-1")["ok"]);
    CHECK((bool)bridge.retryFullIntakeStage("C:/track.wav", "musical_context")["ok"]);

    CHECK(server.requestCount("POST /analyze/analysis-1/cancel") == 1);
    CHECK(server.requestCount("POST /separate/separate-1/cancel") == 1);
    CHECK(server.requestCount("POST /analyze/full_intake/cancel/intake-1") == 1);
    CHECK(server.requestCount("POST /analyze/full_intake/retry/musical_context") == 1);
}

TEST_CASE("HttpServiceBridge sends a fix check as an advanced job with purpose fix_check (FEAT-1)",
          "[ipc][integration][feat1]")
{
    khensu::test::FakeApplyChainServer server(19725);
    server.setResponse("POST /analyze/start", {200, R"({"job_id":"analysis-1"})"});
    HttpServiceBridge bridge(server.baseUrl().toStdString(), 1000, 0);

    REQUIRE(bridge.startAnalysisJob("C:/fixed.wav", ILocalServiceBridge::AnalysisMode::FixCheck).is_ok());
    const auto fixBody = juce::JSON::parse(server.lastBody("POST /analyze/start"));
    CHECK(fixBody["mode"].toString() == "advanced");
    CHECK(fixBody["purpose"].toString() == "fix_check");

    REQUIRE(bridge.startAnalysisJob("C:/track.wav", ILocalServiceBridge::AnalysisMode::Advanced).is_ok());
    const auto fullBody = juce::JSON::parse(server.lastBody("POST /analyze/start"));
    CHECK(fullBody["mode"].toString() == "advanced");
    CHECK_FALSE(fullBody.hasProperty("purpose"));
}
