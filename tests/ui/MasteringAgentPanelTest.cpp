// tests/ui/MasteringAgentPanelTest.cpp
// E2E-ish lifecycle coverage for MasteringAgentPanel, now constructible
// against FakeAppController instead of a live AppController - see
// docs/superpowers/plans/2026-07-18-appcontroller-testability-facade.md
// (TODO.md "Desktop end-to-end tests for every recommendation/apply route").
#include <catch2/catch_test_macros.hpp>
#include "ui/Copilot/MasteringAgentPanel.h"
#include "support/FakeApplyChainServer.h"
#include "support/FakeAppController.h"
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/session/Session.h>

using namespace khensu;
using khensu::test::FakeApplyChainServer;
using khensu::test::FakeAppController;

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

Session makeActiveSession()
{
    Session s;
    s.id = "sess1";
    s.name = "Test Session";
    AudioFileRef f;
    f.path = "Z:/does/not/exist.wav";
    s.files.push_back(f);
    return s;
}

AnalysisResult makeAnalysisAllowingAction()
{
    AnalysisResult a;
    a.genre_relative_action_allowed = true;
    return a;
}

} // namespace

TEST_CASE("MasteringAgentPanel: run() with no active session sets a status message and never hits the network",
          "[masteringagentpanel]")
{
    FakeAppController fake;
    MasteringAgentPanel panel(fake);

    panel.run();

    CHECK(panel.statusText() == "No active session audio.");
    CHECK(panel.currentJobId().isEmpty());
}

TEST_CASE("MasteringAgentPanel: run() with a session but no analysis abstains without hitting the network",
          "[masteringagentpanel]")
{
    FakeAppController fake;
    fake.session = makeActiveSession();
    MasteringAgentPanel panel(fake);

    panel.run();

    CHECK(panel.statusText() == "Analyze the session before starting a genre-relative mastering pass.");
}

TEST_CASE("MasteringAgentPanel: run() abstains when genre-relative action isn't allowed",
          "[masteringagentpanel]")
{
    FakeAppController fake;
    fake.session = makeActiveSession();
    AnalysisResult a;
    a.genre_relative_action_allowed = false;
    a.genre_abstained_reason = "confirm genre targets or provide a reference track";
    fake.analysisResult = a;
    MasteringAgentPanel panel(fake);

    panel.run();

    CHECK(panel.statusText() == "Mastering abstained: confirm genre targets or provide a reference track");
}

TEST_CASE("MasteringAgentPanel: a completed+converged job with a passing guardrail loads an A/B candidate",
          "[masteringagentpanel]")
{
    // Port picked below Windows' ephemeral/excluded TCP range - see
    // RenderJobManagerTest.cpp's identical note.
    FakeApplyChainServer server(19716);
    server.setResponse("POST /copilot/mastering_agent/start", {200, R"({"job_id":"job-master"})"});
    server.setResponse("GET /copilot/mastering_agent/status/job-master",
                       {200, R"({"status":"completed","result":{
                           "converged": true, "requires_user_choice": false,
                           "final_output_path": "Z:/mastered.wav", "decision_hash": "abc",
                           "guardrail": {"ok": true}, "iterations": []
                       }})"});

    FakeAppController fake(server.baseUrl());
    fake.session = makeActiveSession();
    fake.analysisResult = makeAnalysisAllowingAction();
    MasteringAgentPanel panel(fake, server.baseUrl().toStdString());

    panel.run();

    // currentJobId() starts empty (before /start resolves) and stays empty if
    // /start errors, so waiting for it to become empty again is ambiguous
    // with "never started" - wait for the job to actually start first.
    REQUIRE(pumpUntil([&] { return !panel.currentJobId().isEmpty() || panel.statusText().startsWith("Error"); },
                     10000));
    REQUIRE(pumpUntil([&] { return panel.currentJobId().isEmpty(); }, 15000));
    CHECK(panel.statusText().startsWith("Converged in 0 iteration(s)."));
    CHECK(panel.isRunButtonEnabled());

    // The guardrail-passed candidate is loaded into the A/B transport, not
    // silently promoted - matches the module doc's "never silently promoted"
    // comment.
    CHECK(fake.lastAbProcessedPath == "Z:/mastered.wav");

    panel.acceptCandidate();
    CHECK(fake.lastRegisteredRenderPath == "Z:/mastered.wav");
    CHECK(fake.registerAppliedRenderCallCount == 1);
    CHECK(fake.enqueueProvenanceEventCallCount == 1);
    CHECK(fake.lastEventType == "mastering_candidate_accepted");
    CHECK(fake.lastEventSessionId == "sess1");
    // An object (EventRequest.payload is a dict - a bare string was a 422 the
    // outbox dropped) carrying the agent run's decision_hash for provenance.
    REQUIRE(fake.lastEventPayload.getDynamicObject() != nullptr);
    CHECK(fake.lastEventPayload["output_path"].toString() == "Z:/mastered.wav");
    CHECK(fake.lastEventPayload["decision_hash"].toString() == "abc");
}

TEST_CASE("MasteringAgentPanel: a 409 abstention from /start surfaces as an error status, not a crash",
          "[masteringagentpanel]")
{
    FakeApplyChainServer server(19717);
    server.setResponse("POST /copilot/mastering_agent/start", {409, R"({"detail":"abstained"})"});

    FakeAppController fake(server.baseUrl());
    fake.session = makeActiveSession();
    fake.analysisResult = makeAnalysisAllowingAction();
    MasteringAgentPanel panel(fake, server.baseUrl().toStdString());

    panel.run();

    REQUIRE(pumpUntil([&] { return panel.isRunButtonEnabled(); }, 10000));
    // The 409 detail is kept and the abstention classified (AUDIT-068).
    CHECK(panel.statusText() == "Mastering abstained: abstained");
    CHECK(panel.currentJobId().isEmpty());
}

TEST_CASE("MasteringAgentPanel starts from the session's genre (WF-3)", "[masteringagentpanel][wf3]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeAppController fake;
    REQUIRE(fake.genreDb.loadFromString(R"({"genres": [
        {"id": "pop", "name": "Pop", "lufs_target": -9.0, "true_peak_max": -1.0},
        {"id": "techno", "name": "Techno", "lufs_target": -7.0, "true_peak_max": -0.5}]})")
                .is_ok());
    fake.session = makeActiveSession();
    fake.session->target.primary_genre = "techno";
    MasteringAgentPanel panel(fake);
    CHECK(panel.selectedGenreIdForTesting() == "pop"); // the list's first, before it is shown
    panel.syncToSessionGenre();
    CHECK(panel.selectedGenreIdForTesting() == "techno");
}
