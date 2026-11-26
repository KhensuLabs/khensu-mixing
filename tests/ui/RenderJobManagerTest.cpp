// tests/ui/RenderJobManagerTest.cpp
// Lifecycle regression tests for RenderJobManager - the localId-keyed
// cancellation and service-loss bounding fixed in dc4565f
// (frontend-review-2026-07-17.md P1s). No live Python service is started for
// these tests: RenderJobManager always constructs a real HttpServiceBridge
// pointed at its hardcoded default URL (127.0.0.1:47239), so a "start" call
// either fails fast (nothing listening - the expected case in CI and on a
// dev machine with the app closed) or, if the desktop app happens to be
// running locally, hits the real service with a bogus file path and still
// resolves to an error. Either way `ok=false` is what these tests assert.
#include <catch2/catch_test_macros.hpp>
#include "RenderJobManager.h"
#include "support/FakeApplyChainServer.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <memory>

using namespace khensu;
using khensu::test::FakeApplyChainServer;

namespace {

/// Pumps the JUCE message loop - processing MessageManager::callAsync
/// callbacks RenderJobManager's background threads post back - until `pred`
/// is true or `timeoutMs` elapses. Returns whether `pred` became true.
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

} // namespace

TEST_CASE("RenderJobManager: start failure surfaces ok=false and clears the job", "[renderjobmanager]")
{
    RenderJobManager mgr;
    bool called = false;
    bool ok = true;

    mgr.startChainRender("test render", "Z:/does/not/exist.wav", "Z:/does/not/exist_out.wav", {}, "session1", "rec1",
                         [&](bool okArg, const juce::String&, const juce::var&) {
                             called = true;
                             ok = okArg;
                         });

    REQUIRE(pumpUntil([&] { return called; }));
    CHECK_FALSE(ok);
    CHECK(mgr.activeJobs().empty());
}

TEST_CASE("RenderJobManager: activeJobs() exposes a stable localId immediately, before jobId exists",
          "[renderjobmanager]")
{
    RenderJobManager mgr;
    mgr.startChainRender("test render", "Z:/does/not/exist.wav", "Z:/does/not/exist_out.wav", {}, "session1", "rec1",
                         [](bool, const juce::String&, const juce::var&) {});

    auto jobs = mgr.activeJobs();
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].localId != 0);
    // jobId is only assigned once the async /start call returns - it's
    // reasonable for it to still be empty here since the background thread
    // hasn't had a chance to run yet, but it must never be relied on for
    // cancellation (that was the P1 bug: Cancel was a dead control until
    // jobId existed).
    pumpUntil([&] { return mgr.activeJobs().empty(); }, 10000);
}

TEST_CASE("RenderJobManager: cancelling by localId while /start is in flight fires once and removes the job",
          "[renderjobmanager]")
{
    RenderJobManager mgr;
    int callCount = 0;
    bool ok = true;

    mgr.startChainRender("test render", "Z:/does/not/exist.wav", "Z:/does/not/exist_out.wav", {}, "session1", "rec1",
                         [&](bool okArg, const juce::String&, const juce::var&) {
                             ++callCount;
                             ok = okArg;
                         });

    auto jobs = mgr.activeJobs();
    REQUIRE(jobs.size() == 1);
    const auto localId = jobs[0].localId;

    // Cancel immediately - keyed by localId, which is valid from the moment
    // the job is submitted (unlike the old jobId-keyed cancelJob, which
    // no-op'd here because jobId is still empty at this point).
    mgr.cancelJob(localId);

    // The cancel-while-starting path fires the callback synchronously and
    // removes the job from the visible list right away.
    CHECK(callCount == 1);
    CHECK_FALSE(ok);
    CHECK(mgr.activeJobs().empty());

    // Let the background /start call finish (it fails fast with no live
    // service, or resolves against a real one) and confirm the late
    // response is suppressed rather than firing the callback a second time.
    pumpUntil([] { return false; }, 1500);
    CHECK(callCount == 1);
}

TEST_CASE("RenderJobManager: destroying the manager while a start call is in flight does not crash",
          "[renderjobmanager]")
{
    auto mgr = std::make_unique<RenderJobManager>();
    mgr->startChainRender("test render", "Z:/does/not/exist.wav", "Z:/does/not/exist_out.wav", {}, "session1", "rec1",
                          [](bool, const juce::String&, const juce::var&) {});

    // Destroy immediately - the detached background thread's
    // MessageManager::callAsync completion lambda captures a
    // juce::WeakReference and must see self == nullptr safely instead of
    // dereferencing a dangling RenderJobManager (the same class of bug
    // fixed for DeliveryQaPanel in dc4565f, exercised here on the one
    // render-lifecycle class that doesn't need a live AppController to test).
    mgr.reset();

    auto* mm = juce::MessageManager::getInstance();
    mm->runDispatchLoopUntil(1500);
    SUCCEED("manager destroyed mid-flight without crashing");
}

TEST_CASE("RenderJobManager: cancelling after jobId is assigned posts a real /cancel request",
          "[renderjobmanager]")
{
    // Unlike the in-flight-/start cancel test above (which never reaches the
    // network), this exercises the *other* cancelJob() branch: jobId already
    // set, so cancel must actually round-trip to the service.
    // Port picked below Windows' ephemeral/excluded TCP range (see `netsh
    // int ipv4 show excludedportrange protocol=tcp`) - ports up around
    // 49000-65000 routinely land inside a reserved block and bind() fails
    // with WSAEACCES.
    FakeApplyChainServer server(19712);
    server.setResponse("POST /copilot/apply_chain/start", {200, R"({"job_id":"job-abc"})"});

    RenderJobManager mgr(server.baseUrl().toStdString());
    int callCount = 0;
    bool ok = true;
    mgr.startChainRender("test render", "Z:/does/not/exist.wav", "Z:/does/not/exist_out.wav", {}, "session1", "rec1",
                         [&](bool okArg, const juce::String&, const juce::var&) {
                             ++callCount;
                             ok = okArg;
                         });

    // Wait for /start to resolve and jobId to land on the job.
    REQUIRE(pumpUntil([&] {
        auto jobs = mgr.activeJobs();
        return !jobs.empty() && jobs[0].jobId.isNotEmpty();
    }));
    const auto localId = mgr.activeJobs()[0].localId;

    mgr.cancelJob(localId);

    // cancelJob's jobId-known branch fires the callback synchronously.
    CHECK(callCount == 1);
    CHECK_FALSE(ok);
    CHECK(mgr.activeJobs().empty());

    // The fire-and-forget POST /copilot/apply_chain/job-abc/cancel happens on
    // a detached background thread - give it a moment to actually land.
    REQUIRE(pumpUntil([&] { return server.requestCount("POST /copilot/apply_chain/job-abc/cancel") > 0; }, 5000));
}

TEST_CASE("RenderJobManager: a completed job's fullResult and output path reach the callback",
          "[renderjobmanager]")
{
    // The happy-path round trip the earlier tests in this file don't cover:
    // /start -> a "completed" /status response -> ok=true with the render's
    // output path and full result payload (what finishJob() uses to run the
    // post-render guardrail check).
    FakeApplyChainServer server(19714);
    server.setResponse("POST /copilot/apply_chain/start", {200, R"({"job_id":"job-done"})"});
    server.setResponse("GET /copilot/apply_chain/status/job-done",
                       {200, R"({"status":"completed","result":{"output_path":"Z:/out.wav","applied_effects":["eq"]}})"});

    RenderJobManager mgr(server.baseUrl().toStdString());
    bool called = false;
    bool ok = false;
    juce::String outputPath;
    mgr.startChainRender("test render", "Z:/does/not/exist.wav", "Z:/does/not/exist_out.wav", {}, "session1", "rec1",
                         [&](bool okArg, const juce::String& out, const juce::var&) {
                             called = true;
                             ok = okArg;
                             outputPath = out;
                         });

    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(ok);
    CHECK(outputPath == "Z:/out.wav");
    CHECK(mgr.activeJobs().empty());
}

TEST_CASE("RenderJobManager: target_stem is carried through to the apply_chain request body",
          "[renderjobmanager][ai02]")
{
    // Audit AI-02 end-to-end (desktop -> bridge -> HTTP): a non-mix target
    // must actually appear in the request the service receives, not be
    // dropped so the backend silently defaults to "mix". Drives the real
    // startChainRender -> startApplyChainJob -> HttpServiceBridge path against
    // a fake server and inspects the wire body.
    FakeApplyChainServer server(19717);
    server.setResponse("POST /copilot/apply_chain/start", {200, R"({"job_id":"job-stem"})"});
    server.setResponse("GET /copilot/apply_chain/status/job-stem",
                       {200, R"({"status":"completed","result":{"output_path":"Z:/v.wav"}})"});

    RenderJobManager mgr(server.baseUrl().toStdString());
    bool called = false;
    mgr.startChainRender("vocals render", "Z:/vocals.wav", "Z:/vocals_out.wav", {}, "session1", "rec1",
                         [&](bool, const juce::String&, const juce::var&) { called = true; },
                         /*genreRelative=*/false, /*genreActionAllowed=*/false, /*abstainedReason=*/{},
                         /*targetStem=*/"vocals");

    REQUIRE(pumpUntil([&] { return called; }));
    const juce::String body = server.lastBody("POST /copilot/apply_chain/start");
    CHECK(body.contains("\"target_stem\""));
    CHECK(body.contains("vocals"));
}

TEST_CASE("RenderJobManager: a keyed render sends key_stem beside stem_paths", "[renderjobmanager][enh6]")
{
    // ENH-6: the service-path render of a keyed DynamicEQ (masking fix) must
    // name its key stem, and the stem must be among the stem_paths it sends,
    // or the service keys the stage on the track itself.
    FakeApplyChainServer server(19733);
    server.setResponse("POST /copilot/apply_chain/start", {200, R"({"job_id":"job-key"})"});
    server.setResponse("GET /copilot/apply_chain/status/job-key",
                       {200, R"({"status":"completed","result":{"output_path":"Z:/k.wav"}})"});

    RenderJobManager mgr(server.baseUrl().toStdString());
    mgr.setStemProvenanceProvider([] {
        return std::map<juce::String, juce::String>{{"vocals", "Z:/stems/vocals.wav"}};
    });
    bool called = false;
    mgr.startChainRender("masking fix", "Z:/mix.wav", "Z:/mix_out.wav", {}, "session1", "rec1",
                         [&](bool, const juce::String&, const juce::var&) { called = true; },
                         false, false, {}, {}, {}, /*keyStem=*/"vocals");

    REQUIRE(pumpUntil([&] { return called; }));
    const auto body = juce::JSON::parse(server.lastBody("POST /copilot/apply_chain/start"));
    CHECK(body["key_stem"].toString() == "vocals");
    CHECK(body["stem_paths"]["vocals"].toString() == "Z:/stems/vocals.wav");

    // Without a key stem the key is left out entirely.
    called = false;
    mgr.startChainRender("plain", "Z:/mix.wav", "Z:/mix_out2.wav", {}, "session1", "rec1",
                         [&](bool, const juce::String&, const juce::var&) { called = true; });
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK_FALSE(server.lastBody("POST /copilot/apply_chain/start").contains("\"key_stem\""));
}

TEST_CASE("RenderJobManager: an unspecified target_stem is omitted (backend defaults to mix)",
          "[renderjobmanager][ai02]")
{
    // The 6 existing startChainRender call sites that don't target a stem must
    // stay unaffected - an empty target_stem is omitted from the body so the
    // server applies its own "mix" default rather than receiving an empty
    // string (which validate_stem_attribution would reject).
    FakeApplyChainServer server(19718);
    server.setResponse("POST /copilot/apply_chain/start", {200, R"({"job_id":"job-mix"})"});
    server.setResponse("GET /copilot/apply_chain/status/job-mix",
                       {200, R"({"status":"completed","result":{"output_path":"Z:/m.wav"}})"});

    RenderJobManager mgr(server.baseUrl().toStdString());
    bool called = false;
    mgr.startChainRender("mix render", "Z:/mix.wav", "Z:/mix_out.wav", {}, "session1", "rec1",
                         [&](bool, const juce::String&, const juce::var&) { called = true; });

    REQUIRE(pumpUntil([&] { return called; }));
    const juce::String body = server.lastBody("POST /copilot/apply_chain/start");
    CHECK_FALSE(body.contains("\"target_stem\""));
}

TEST_CASE("RenderJobManager: a failed job's guardrail-rejection result reaches the callback and onGuardrailRejected",
          "[renderjobmanager][guardrail]")
{
    // Audit AI-01: the poll loop used to discard a failed job's `result`
    // entirely (passing {} to finishJob), throwing away the quarantine_path/
    // guardrail_status/measurements the server now attaches when a render was
    // quarantined rather than promoted. This is the regression test for that
    // fix, plus the new onGuardrailRejected callback it enables.
    FakeApplyChainServer server(19716);
    server.setResponse("POST /copilot/apply_chain/start", {200, R"({"job_id":"job-rejected"})"});
    server.setResponse("GET /copilot/apply_chain/status/job-rejected",
                       {200, R"({"status":"failed","error":"Post-render guardrail rejected the output",)"
                             R"("result":{"status":"rejected","guardrail_status":"rejected",)"
                             R"("quarantine_path":"Z:/.quarantine_out.wav","output_path":"Z:/out.wav",)"
                             R"("source_path":"Z:/in.wav","measurements":{"ok":false}}})"});

    RenderJobManager mgr(server.baseUrl().toStdString());

    bool rejectionFired = false;
    GuardrailRejection rejection;
    mgr.onGuardrailRejected = [&](const GuardrailRejection& r) {
        rejectionFired = true;
        rejection = r;
    };

    bool called = false;
    bool ok = true;
    juce::var fullResult;
    mgr.startChainRender("test render", "Z:/in.wav", "Z:/out.wav", {}, "session1", "rec1",
                         [&](bool okArg, const juce::String&, const juce::var& fr) {
                             called = true;
                             ok = okArg;
                             fullResult = fr;
                         });

    REQUIRE(pumpUntil([&] { return called; }));
    CHECK_FALSE(ok);
    CHECK(fullResult["quarantine_path"].toString() == "Z:/.quarantine_out.wav"); // no longer discarded as {}

    REQUIRE(rejectionFired);
    CHECK(rejection.guardrailStatus == "rejected");
    CHECK(rejection.sourcePath == "Z:/in.wav");
    CHECK(rejection.quarantinePath == "Z:/.quarantine_out.wav");
    CHECK(rejection.outputPath == "Z:/out.wav");
}

TEST_CASE("RenderJobManager: a 409 abstention from /start surfaces as a prefixed ok=false error",
          "[renderjobmanager]")
{
    // Mirrors the server-side genre-action-block/attribution-block 409s that
    // /copilot/apply_chain/start, /apply_chain/batch/start,
    // /apply_section_chain/start, and /mastering_agent/start can all return
    // (khensu_services/copilot/router.py's _genre_action_block /
    // validate_stem_attribution / validate_render_request) - only the
    // apply_chain route is reachable through RenderJobManager without a live
    // AppController (see TODO.md), so this is the one desktop-side test of
    // that translation. registerStart() detects "abstained" in the HTTP
    // error detail and prefixes the message accordingly.
    FakeApplyChainServer server(19715);
    server.setResponse("POST /copilot/apply_chain/start",
                       {409, R"({"detail":"Mastering agent abstained: confirm genre targets or provide a reference track."})"});

    RenderJobManager mgr(server.baseUrl().toStdString());
    bool called = false;
    bool ok = true;
    juce::String errorMsg;
    mgr.startChainRender("test render", "Z:/does/not/exist.wav", "Z:/does/not/exist_out.wav", {}, "session1", "rec1",
                         [&](bool okArg, const juce::String& err, const juce::var&) {
                             called = true;
                             ok = okArg;
                             errorMsg = err;
                         });

    REQUIRE(pumpUntil([&] { return called; }));
    CHECK_FALSE(ok);
    CHECK(errorMsg.startsWith("Render abstained: "));
    CHECK(errorMsg.contains("confirm genre targets"));
    CHECK(mgr.activeJobs().empty());
}

TEST_CASE("RenderJobManager: repeated poll failures surface as a service-loss error instead of polling forever",
          "[renderjobmanager]")
{
    FakeApplyChainServer server(19713);
    server.setResponse("POST /copilot/apply_chain/start", {200, R"({"job_id":"job-xyz"})"});
    // Every status poll 500s - the polling-bound path (kMaxConsecutivePollErrors
    // / kMaxPollErrorElapsedMs in RenderJobManager.cpp), not the /start failure
    // path the first test in this file covers.
    server.setResponse("GET /copilot/apply_chain/status/job-xyz", {500, R"({"detail":"boom"})"});

    RenderJobManager mgr(server.baseUrl().toStdString());
    bool called = false;
    bool ok = true;
    juce::String errorMsg;
    mgr.startChainRender("test render", "Z:/does/not/exist.wav", "Z:/does/not/exist_out.wav", {}, "session1", "rec1",
                         [&](bool okArg, const juce::String& err, const juce::var&) {
                             called = true;
                             ok = okArg;
                             errorMsg = err;
                         });

    // The poll timer fires every 750ms and the error budget is 8 consecutive
    // failures / 20s elapsed - comfortably within a 30s test timeout.
    REQUIRE(pumpUntil([&] { return called; }, 30000));
    CHECK_FALSE(ok);
    // The message carries the last poll's error (2026-11-18: it is what told a
    // closed identity gate from a timeout or an HTTP error in the soak test).
    CHECK(errorMsg.startsWith("Analysis service isn't reachable - render status unknown ("));
    CHECK(errorMsg.contains("apply_chain/status"));
    CHECK(mgr.activeJobs().empty());
    CHECK(server.requestCount("GET /copilot/apply_chain/status/job-xyz") > 1);
}

namespace {

/// Builds a fullResult juce::var matching the shape RenderJobManager::finishJob()
/// receives from /copilot/apply_chain's response, with a "guardrail" object.
juce::var makeGuardrailResult(bool guardOk, const juce::StringArray& blocks, const juce::StringArray& warnings,
                              juce::var artifactDelta = {})
{
    auto* guardObj = new juce::DynamicObject();
    guardObj->setProperty("ok", guardOk);
    juce::Array<juce::var> blocksArr;
    for (const auto& b : blocks)
        blocksArr.add(b);
    guardObj->setProperty("blocks", blocksArr);
    juce::Array<juce::var> warnArr;
    for (const auto& w : warnings)
        warnArr.add(w);
    guardObj->setProperty("warnings", warnArr);
    if (!artifactDelta.isVoid()) guardObj->setProperty("artifact_score_delta", artifactDelta);

    auto* rootObj = new juce::DynamicObject();
    rootObj->setProperty("guardrail", juce::var(guardObj));
    return juce::var(rootObj);
}

} // namespace

// Post-render safety guardrail (safety audit P0 / TODO.md "end-to-end UI
// regression coverage for the accept/reject path"): evaluateGuardrailAlert()
// is the pure decision logic behind that path - whether a completed render
// surfaces a hard block (regression the user must review before treating the
// render as an improvement) or a soft warning, and what it says. Unlike the
// job-lifecycle tests above, this needs no live service at all: it's a pure
// function of the JSON response body, split out of finishJob() specifically
// so it's testable without a network round-trip.

TEST_CASE("evaluateGuardrailAlert: no guardrail field means no alert", "[guardrail]")
{
    auto* rootObj = new juce::DynamicObject();
    rootObj->setProperty("applied_effects", juce::Array<juce::var>());
    const auto alert = evaluateGuardrailAlert(juce::var(rootObj));
    CHECK_FALSE(alert.has_value());
}

TEST_CASE("evaluateGuardrailAlert: guardOk with no warnings means no alert", "[guardrail]")
{
    const auto result = makeGuardrailResult(true, {}, {});
    const auto alert = evaluateGuardrailAlert(result);
    CHECK_FALSE(alert.has_value());
}

TEST_CASE("evaluateGuardrailAlert: a regression (guardOk=false with blocks) is a hard block", "[guardrail]")
{
    const auto result = makeGuardrailResult(false, {"True peak exceeded -1 dBTP", "Mono-fold detected"}, {});
    const auto alert = evaluateGuardrailAlert(result);
    REQUIRE(alert.has_value());
    CHECK(alert->isBlock);
    CHECK(alert->details == "True peak exceeded -1 dBTP\nMono-fold detected");
}

TEST_CASE("evaluateGuardrailAlert: warnings with guardOk=true are a soft warning, not a block", "[guardrail]")
{
    const auto result = makeGuardrailResult(true, {}, {"Dynamics squashed vs. source"});
    const auto alert = evaluateGuardrailAlert(result);
    REQUIRE(alert.has_value());
    CHECK_FALSE(alert->isBlock);
    CHECK(alert->details == "Dynamics squashed vs. source");
}

TEST_CASE("evaluateGuardrailAlert: an artifact-score delta alone is not a caution", "[guardrail]")
{
    // 2026-11-18: every render with a delta - 0.0, or -0.12 (an improvement) -
    // raised a red "Render caution" banner. The service words a warning when
    // the delta crosses its threshold; that one still surfaces (above).
    for (const double delta : {-0.12, 0.0, 0.04}) {
        const auto result = makeGuardrailResult(true, {}, {}, juce::var(delta));
        CHECK_FALSE(evaluateGuardrailAlert(result).has_value());
    }
}

TEST_CASE("evaluateGuardrailAlert: blocks take priority over warnings when both are present", "[guardrail]")
{
    const auto result = makeGuardrailResult(false, {"Clipping introduced"}, {"Dynamics squashed vs. source"});
    const auto alert = evaluateGuardrailAlert(result);
    REQUIRE(alert.has_value());
    CHECK(alert->isBlock);
    CHECK(alert->details == "Clipping introduced");
}

TEST_CASE("evaluateGuardrailAlert: guardOk=false with an empty blocks array falls through to warnings", "[guardrail]")
{
    // Mirrors the original inline logic: `!guardOk && !issues.isEmpty()` must
    // both hold for a hard block - a malformed guardOk=false with no actual
    // block reasons must not suppress a real warning underneath it.
    const auto result = makeGuardrailResult(false, {}, {"Dynamics squashed vs. source"});
    const auto alert = evaluateGuardrailAlert(result);
    REQUIRE(alert.has_value());
    CHECK_FALSE(alert->isBlock);
    CHECK(alert->details == "Dynamics squashed vs. source");
}

// describeGuardrailRejection() (audit AI-01): pure decision logic
// distinguishing a guardrail-caused job failure (quarantined, override
// available) from a generic one (pre-render block, unrelated exception),
// given the failed job's result JSON.

TEST_CASE("describeGuardrailRejection: no quarantine_path means no rejection detail", "[guardrail]")
{
    auto* rootObj = new juce::DynamicObject();
    rootObj->setProperty("status", "blocked");
    CHECK_FALSE(describeGuardrailRejection(juce::var(rootObj)).has_value());
}

TEST_CASE("describeGuardrailRejection: an empty fullResult means no rejection detail", "[guardrail]")
{
    CHECK_FALSE(describeGuardrailRejection(juce::var()).has_value());
}

TEST_CASE("describeGuardrailRejection: a quarantined render surfaces all fields", "[guardrail]")
{
    auto* measurements = new juce::DynamicObject();
    measurements->setProperty("ok", false);

    auto* rootObj = new juce::DynamicObject();
    rootObj->setProperty("status", "rejected");
    rootObj->setProperty("guardrail_status", "rejected");
    rootObj->setProperty("source_path", "Z:/in.wav");
    rootObj->setProperty("quarantine_path", "Z:/.quarantine_out.wav");
    rootObj->setProperty("output_path", "Z:/out.wav");
    rootObj->setProperty("measurements", juce::var(measurements));

    const auto rejection = describeGuardrailRejection(juce::var(rootObj));
    REQUIRE(rejection.has_value());
    CHECK(rejection->guardrailStatus == "rejected");
    CHECK(rejection->sourcePath == "Z:/in.wav");
    CHECK(rejection->quarantinePath == "Z:/.quarantine_out.wav");
    CHECK(rejection->outputPath == "Z:/out.wav");
    CHECK_FALSE(rejection->measurements.isVoid());
}

TEST_CASE("describeGuardrailRejection: a missing-guardrail failure has no measurements but is still surfaced",
          "[guardrail]")
{
    auto* rootObj = new juce::DynamicObject();
    rootObj->setProperty("guardrail_status", "missing");
    rootObj->setProperty("quarantine_path", "Z:/.quarantine_out.wav");

    const auto rejection = describeGuardrailRejection(juce::var(rootObj));
    REQUIRE(rejection.has_value());
    CHECK(rejection->guardrailStatus == "missing");
    CHECK(rejection->measurements.isVoid());
}

// GuardrailRejection::renderState() (audit AI-01 first-class terminal state).

TEST_CASE("GuardrailRejection::renderState prefers the server-supplied render_state", "[guardrail][ai01]")
{
    auto* rootObj = new juce::DynamicObject();
    rootObj->setProperty("guardrail_status", "error");
    rootObj->setProperty("render_state", "unvalidated"); // server's normalized field
    rootObj->setProperty("quarantine_path", "Z:/.quarantine_out.wav");

    const auto rejection = describeGuardrailRejection(juce::var(rootObj));
    REQUIRE(rejection.has_value());
    CHECK(rejection->renderState() == "unvalidated");
}

TEST_CASE("GuardrailRejection::renderState derives from guardrail_status when the server omits render_state",
          "[guardrail][ai01]")
{
    auto* rejectedObj = new juce::DynamicObject();
    rejectedObj->setProperty("guardrail_status", "rejected");
    rejectedObj->setProperty("quarantine_path", "Z:/.quarantine_out.wav");
    CHECK(describeGuardrailRejection(juce::var(rejectedObj))->renderState() == "rejected");

    auto* errorObj = new juce::DynamicObject();
    errorObj->setProperty("guardrail_status", "error");
    errorObj->setProperty("quarantine_path", "Z:/.quarantine_out.wav");
    CHECK(describeGuardrailRejection(juce::var(errorObj))->renderState() == "unvalidated");
}
