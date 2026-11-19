// tests/ui/SectionIntensityPanelTest.cpp
// E2E-ish lifecycle coverage for SectionIntensityPanel, now constructible
// against FakeAppController instead of a live AppController - see
// docs/superpowers/plans/2026-07-18-appcontroller-testability-facade.md
// (TODO.md "Desktop end-to-end tests for every recommendation/apply route").
#include <catch2/catch_test_macros.hpp>
#include "ui/Copilot/SectionIntensityPanel.h"
#include "support/FakeApplyChainServer.h"
#include "support/FakeAppController.h"
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/analysis/SectionAnalysis.h>
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

/// An analysis result carrying one detected section and allowing
/// genre-relative action - the minimum apply() needs to actually submit.
AnalysisResult makeAnalysisWithOneSection()
{
    AnalysisResult a;
    a.genre_relative_action_allowed = true;
    SectionAnalysis sa;
    SectionData sec;
    sec.label = "section_1";
    sec.start_sec = 0.0f;
    sec.end_sec = 30.0f;
    sa.sections.push_back(sec);
    sa.section_count = 1;
    a.section_analysis = sa;
    return a;
}

} // namespace

TEST_CASE("SectionIntensityPanel: refreshSections() with no section analysis disables apply",
          "[sectionintensitypanel]")
{
    FakeAppController fake;
    fake.session = makeActiveSession();
    SectionIntensityPanel panel(fake);

    panel.refreshSections();

    CHECK(panel.statusText() == "No section analysis available for the current track.");
    CHECK_FALSE(panel.isApplyButtonEnabled());
}

TEST_CASE("SectionIntensityPanel: empty section analysis explains why apply is disabled",
          "[sectionintensitypanel]")
{
    FakeAppController fake;
    fake.session = makeActiveSession();
    AnalysisResult analysis;
    analysis.section_analysis = SectionAnalysis{};
    fake.analysisResult = analysis;
    SectionIntensityPanel panel(fake);

    panel.refreshSections();

    CHECK_FALSE(panel.isApplyButtonEnabled());
    CHECK(panel.statusText() == "No sections detected; section-aware processing is unavailable.");
}

TEST_CASE("SectionIntensityPanel: refreshSections() with sections enables apply and reports the count",
          "[sectionintensitypanel]")
{
    FakeAppController fake;
    fake.session = makeActiveSession();
    fake.analysisResult = makeAnalysisWithOneSection();
    SectionIntensityPanel panel(fake);

    panel.refreshSections();

    CHECK(panel.isApplyButtonEnabled());
    CHECK(panel.statusText().startsWith("1 sections detected."));
}

TEST_CASE("SectionIntensityPanel: apply() abstains when genre-relative action isn't allowed",
          "[sectionintensitypanel]")
{
    FakeAppController fake;
    fake.session = makeActiveSession();
    auto analysis = makeAnalysisWithOneSection();
    analysis.genre_relative_action_allowed = false;
    analysis.genre_abstained_reason = "confirm genre targets";
    fake.analysisResult = analysis;
    SectionIntensityPanel panel(fake);
    panel.refreshSections();

    panel.apply();

    CHECK(panel.statusText() == "Section processing abstained: confirm genre targets");
    CHECK(panel.currentJobId().isEmpty());
}

TEST_CASE("SectionIntensityPanel: a completed job registers the render on the controller",
          "[sectionintensitypanel]")
{
    FakeApplyChainServer server(19718);
    server.setResponse("POST /copilot/apply_section_chain/start", {200, R"({"job_id":"job-sect"})"});
    server.setResponse("GET /copilot/apply_section_chain/status/job-sect",
                       {200, R"({"status":"completed","result":{
                           "output_path":"Z:/sectioned.wav", "guardrail":{"ok":true}, "sections":[]
                       }})"});

    FakeAppController fake(server.baseUrl());
    fake.session = makeActiveSession();
    fake.analysisResult = makeAnalysisWithOneSection();
    SectionIntensityPanel panel(fake, server.baseUrl().toStdString());
    panel.refreshSections();

    panel.apply();

    // See MasteringAgentPanelTest.cpp's identical comment: wait for the job
    // to actually start before waiting for it to clear again.
    REQUIRE(pumpUntil([&] { return !panel.currentJobId().isEmpty() || panel.statusText().startsWith("Error"); },
                     10000));
    REQUIRE(pumpUntil([&] { return panel.currentJobId().isEmpty(); }, 15000));
    CHECK(panel.statusText().startsWith("Done: 0 sections rendered."));
    CHECK(panel.isApplyButtonEnabled());
    CHECK(fake.lastRegisteredRenderPath == "Z:/sectioned.wav");
    CHECK(fake.registerAppliedRenderCallCount == 1);
}

TEST_CASE("SectionIntensityPanel: a 409 block from /start surfaces as an error status, not a crash",
          "[sectionintensitypanel]")
{
    FakeApplyChainServer server(19719);
    server.setResponse("POST /copilot/apply_section_chain/start", {409, R"({"detail":"blocked"})"});

    FakeAppController fake(server.baseUrl());
    fake.session = makeActiveSession();
    fake.analysisResult = makeAnalysisWithOneSection();
    SectionIntensityPanel panel(fake, server.baseUrl().toStdString());
    panel.refreshSections();

    panel.apply();

    REQUIRE(pumpUntil([&] { return panel.isApplyButtonEnabled(); }, 10000));
    // A non-abstention 409 reads as a conflict, with the service's detail (AUDIT-068).
    CHECK(panel.statusText().startsWith("Error: The request conflicts with the current state: blocked"));
    CHECK(panel.currentJobId().isEmpty());
}
