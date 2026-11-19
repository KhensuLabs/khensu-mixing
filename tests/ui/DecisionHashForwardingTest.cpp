// tests/ui/DecisionHashForwardingTest.cpp
// The decision_hash a Copilot reply carries (llm_config.decision_hash) must
// reach every render request built from that reply, so the service's
// provenance event can trace the render back to the model decision - and a
// render no model proposed must not send the key at all. Drives the real
// RenderJobManager / HttpServiceBridge against FakeApplyChainServer and
// inspects the wire bodies.
#include <catch2/catch_test_macros.hpp>
#include "RenderJobManager.h"
#include "support/FakeApplyChainServer.h"
#include <khensu/ipc/HttpServiceBridge.h>

using namespace khensu;
using khensu::test::FakeApplyChainServer;

namespace {

template <typename Pred> bool pumpUntil(Pred pred, int timeoutMs = 10000)
{
    auto* mm = juce::MessageManager::getInstance();
    const auto deadline = juce::Time::currentTimeMillis() + timeoutMs;
    while (!pred()) {
        if (juce::Time::currentTimeMillis() > deadline) return false;
        mm->runDispatchLoopUntil(20);
    }
    return true;
}

juce::var parsedBody(const FakeApplyChainServer& server, const juce::String& key)
{
    return juce::JSON::parse(server.lastBody(key));
}

std::vector<ipc::HttpServiceBridge::EffectSpec> oneStageChain()
{
    ipc::HttpServiceBridge::EffectSpec eq;
    eq.effect = "PeakFilter";
    eq.parameters["cutoff_frequency_hz"] = 3000.0f;
    eq.parameters["gain_db"] = -2.0f;
    return {eq};
}

} // namespace

TEST_CASE("Decision hash: a chat-proposed chain render carries decision_hash in apply_chain/start", "[decisionhash]")
{
    FakeApplyChainServer server(19871);
    server.setResponse("POST /copilot/apply_chain/start", {200, R"({"job_id":"job-dh"})"});
    server.setResponse("GET /copilot/apply_chain/status/job-dh",
                       {200, R"({"status":"completed","result":{"output_path":"Z:/out.wav"}})"});

    RenderJobManager mgr(server.baseUrl().toStdString());
    bool called = false;
    // Same call shape as ChatConsolePanel::performApplyChain().
    mgr.startChainRender(
        "Copilot chain -> out.wav", "Z:/in.wav", "Z:/out.wav", oneStageChain(), "session1", "copilot_chat",
        [&](bool, const juce::String&, const juce::var&) { called = true; },
        /*genreRelative=*/true, /*genreActionAllowed=*/true, /*abstainedReason=*/{},
        /*targetStem=*/"mix", /*decisionHash=*/"0123456789abcdef");

    REQUIRE(pumpUntil([&] { return called; }));
    const auto body = parsedBody(server, "POST /copilot/apply_chain/start");
    CHECK(body["decision_hash"].toString() == "0123456789abcdef");
    CHECK(body["session_id"].toString() == "session1");
    CHECK(mgr.lastChainDecisionHash() == "0123456789abcdef");
}

TEST_CASE("Decision hash: a render no model proposed omits decision_hash and clears the last-chain hash",
          "[decisionhash]")
{
    FakeApplyChainServer server(19872);
    server.setResponse("POST /copilot/apply_chain/start", {200, R"({"job_id":"job-plain"})"});
    server.setResponse("GET /copilot/apply_chain/status/job-plain",
                       {200, R"({"status":"completed","result":{"output_path":"Z:/out.wav"}})"});

    RenderJobManager mgr(server.baseUrl().toStdString());
    int done = 0;
    mgr.startChainRender(
        "Copilot chain", "Z:/in.wav", "Z:/out.wav", oneStageChain(), "session1", "copilot_chat",
        [&](bool, const juce::String&, const juce::var&) { ++done; }, true, true, {}, "mix", "fedcba9876543210");
    REQUIRE(pumpUntil([&] { return done == 1; }));

    // Then e.g. an EQ-curve apply (AppController::applyEqBands): no hash.
    mgr.startChainRender("EQ curve", "Z:/in.wav", "Z:/out2.wav", oneStageChain(), "session1", "eq_curve_apply",
                         [&](bool, const juce::String&, const juce::var&) { ++done; });
    REQUIRE(pumpUntil([&] { return done == 2; }));

    // Keep the parsed var alive: a pointer into a temporary dangles (the Debug
    // heap caught it as a SIGSEGV; Release happened to read freed memory).
    const auto body = parsedBody(server, "POST /copilot/apply_chain/start");
    const auto* obj = body.getDynamicObject();
    REQUIRE(obj != nullptr);
    CHECK_FALSE(obj->hasProperty("decision_hash"));
    // A later batch re-use of lastChain() must not inherit the chat hash.
    CHECK(mgr.lastChainDecisionHash().isEmpty());
}

TEST_CASE("Decision hash: batch and section-chain starts forward it only when set", "[decisionhash]")
{
    FakeApplyChainServer server(19873);
    server.setResponse("POST /copilot/apply_chain/batch/start", {200, R"({"job_id":"job-batch"})"});
    server.setResponse("POST /copilot/apply_section_chain/start", {200, R"({"job_id":"job-section"})"});

    ipc::HttpServiceBridge bridge(server.baseUrl().toStdString());
    ipc::HttpServiceBridge::SectionChainSpec section;
    section.label = "chorus";
    section.startSec = 0.0;
    section.endSec = 10.0;
    section.effects = oneStageChain();

    REQUIRE(
        bridge.startApplyChainBatchJob("Z:/folder", {}, oneStageChain(), "_processed", {}, "00ff00ff00ff00ff").is_ok());
    CHECK(parsedBody(server, "POST /copilot/apply_chain/batch/start")["decision_hash"].toString() ==
          "00ff00ff00ff00ff");
    REQUIRE(bridge.startApplyChainBatchJob("Z:/folder", {}, oneStageChain()).is_ok());
    CHECK_FALSE(
        parsedBody(server, "POST /copilot/apply_chain/batch/start").getDynamicObject()->hasProperty("decision_hash"));

    REQUIRE(bridge
                .startSectionChainJob("Z:/in.wav", "Z:/out.wav", {section}, 30.0, "s1", "section_aware", true, true, {},
                                      {}, "aabbccddeeff0011")
                .is_ok());
    CHECK(parsedBody(server, "POST /copilot/apply_section_chain/start")["decision_hash"].toString() ==
          "aabbccddeeff0011");
    REQUIRE(bridge.startSectionChainJob("Z:/in.wav", "Z:/out.wav", {section}).is_ok());
    CHECK_FALSE(
        parsedBody(server, "POST /copilot/apply_section_chain/start").getDynamicObject()->hasProperty("decision_hash"));
    // (The synchronous applyChain() wrapper was removed as dead code - audit
    // 9.2; renders go through startApplyChainJob, covered above.)
}

TEST_CASE("Decision hash: a guardrail-rejected chat render keeps it for the expert override", "[decisionhash]")
{
    FakeApplyChainServer server(19874);
    server.setResponse("POST /copilot/apply_chain/start", {200, R"({"job_id":"job-rej"})"});
    server.setResponse("GET /copilot/apply_chain/status/job-rej",
                       {200, R"({"status":"failed","error":"rejected","result":{"status":"rejected",)"
                             R"("guardrail_status":"rejected","quarantine_path":"Z:/.quarantine_out.wav",)"
                             R"("output_path":"Z:/out.wav","source_path":"Z:/in.wav","measurements":{"ok":false}}})"});
    server.setResponse("POST /copilot/override_guardrail", {200, R"({"status":"success"})"});

    RenderJobManager mgr(server.baseUrl().toStdString());
    std::optional<GuardrailRejection> rejection;
    mgr.onGuardrailRejected = [&](const GuardrailRejection& r) { rejection = r; };
    bool called = false;
    mgr.startChainRender(
        "Copilot chain", "Z:/in.wav", "Z:/out.wav", oneStageChain(), "session1", "copilot_chat",
        [&](bool, const juce::String&, const juce::var&) { called = true; }, true, true, {}, "mix", "0a0b0c0d0e0f1011");
    REQUIRE(pumpUntil([&] { return called; }));
    REQUIRE(rejection.has_value());
    CHECK(rejection->decisionHash == "0a0b0c0d0e0f1011");

    // AppController::requestGuardrailOverride's call shape.
    ipc::HttpServiceBridge bridge(server.baseUrl().toStdString());
    REQUIRE(bridge
                .overrideGuardrail(rejection->sourcePath, rejection->quarantinePath, rejection->outputPath,
                                   "intentional clipping", rejection->sessionId, rejection->sourceRecommendationId,
                                   rejection->decisionHash)
                .is_ok());
    CHECK(parsedBody(server, "POST /copilot/override_guardrail")["decision_hash"].toString() == "0a0b0c0d0e0f1011");
}
