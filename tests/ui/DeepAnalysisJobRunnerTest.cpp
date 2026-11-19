// tests/ui/DeepAnalysisJobRunnerTest.cpp
// Audit CQ-04: deterministic coverage for the deep-analysis job-poll loop's
// timeout, error-budget, and cancel-POST paths - now shared by both the
// single-file and batch (>1 file) analysis paths, and driven here against a
// fake ILocalServiceBridge with an injected clock and no-op sleep (the
// "injectable bridge seam" the finding asked for).
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include "analysis/DeepAnalysisJobRunner.h"
#include "support/StubLocalServiceBridge.h"

using namespace khensu;
using namespace khensu::ipc;

namespace {

/// Scriptable fake over the do-nothing stub: each getAnalysisProgress() call
/// returns the next queued status (err results are queued as !is_ok).
/// Records how many times cancelAnalysisJob() was POSTed.
class FakeBridge : public khensu::test::StubLocalServiceBridge {
  public:
    // --- knobs the tests set ---
    bool startOk = true;
    BridgeError startError{BridgeError::Kind::HttpError, "start failed"};
    std::vector<Result<AnalysisJobStatus, BridgeError>> polls;
    bool analyzeOk = true;

    // --- observed ---
    int cancelCount = 0;
    int analyzeCount = 0;
    std::string startedFile;
    std::optional<AnalysisMode> startedMode;

    Result<std::string, BridgeError> startAnalysisJob(const std::string& file_path, AnalysisMode mode) override
    {
        startedFile = file_path;
        startedMode = mode;
        if (!startOk) return Result<std::string, BridgeError>::err(startError);
        return Result<std::string, BridgeError>::ok("job-1");
    }

    Result<AnalysisJobStatus, BridgeError> getAnalysisProgress(const std::string&) override
    {
        if (pollIdx_ < polls.size()) return polls[pollIdx_++];
        // Ran out of scripted polls: keep returning "running" so a deadline
        // test can drive termination purely via the injected clock.
        AnalysisJobStatus running;
        running.status = "running";
        return Result<AnalysisJobStatus, BridgeError>::ok(running);
    }

    void cancelAnalysisJob(const std::string&) override { ++cancelCount; }

    Result<DeepAnalysisResponse, BridgeError> analyze(const std::string&, AnalysisMode) override
    {
        ++analyzeCount;
        if (!analyzeOk)
            return Result<DeepAnalysisResponse, BridgeError>::err({BridgeError::Kind::HttpError, "analyze failed"});
        return Result<DeepAnalysisResponse, BridgeError>::ok(DeepAnalysisResponse{});
    }

  private:
    size_t pollIdx_ = 0;
};

Result<AnalysisJobStatus, BridgeError> statusOk(const std::string& s, bool hasResult = false)
{
    AnalysisJobStatus st;
    st.status = s;
    st.has_result = hasResult;
    return Result<AnalysisJobStatus, BridgeError>::ok(st);
}

Result<AnalysisJobStatus, BridgeError> pollErr()
{
    return Result<AnalysisJobStatus, BridgeError>::err({BridgeError::Kind::ServiceUnavailable, "poll failed"});
}

DeepAnalysisJobHooks silentHooks()
{
    DeepAnalysisJobHooks h;
    h.sleep = [](int) {}; // never actually wait
    h.now = [] { return 0u; };
    return h;
}

} // namespace

TEST_CASE("runDeepAnalysisJob: a completed job without a parsed result re-fetches once", "[deepjob][cq04]")
{
    FakeBridge bridge;
    bridge.polls.push_back(statusOk("running"));
    bridge.polls.push_back(statusOk("completed", /*hasResult=*/true));

    auto r = runDeepAnalysisJob(bridge, "in.wav", silentHooks());

    CHECK(r.outcome == DeepAnalysisOutcome::Completed);
    CHECK(r.hasResponse);
    CHECK(bridge.analyzeCount == 1); // re-fetched exactly once
    CHECK(bridge.cancelCount == 0);  // a clean completion never cancels
}

TEST_CASE("runDeepAnalysisJob: a failed start returns JobStartFailed and never polls", "[deepjob][cq04]")
{
    FakeBridge bridge;
    bridge.startOk = false;

    auto r = runDeepAnalysisJob(bridge, "in.wav", silentHooks());

    CHECK(r.outcome == DeepAnalysisOutcome::JobStartFailed);
    CHECK_FALSE(r.hasResponse);
    CHECK(bridge.analyzeCount == 0);
}

TEST_CASE("runDeepAnalysisJob: exhausting the consecutive-error budget POSTs a server-side cancel", "[deepjob][cq04]")
{
    FakeBridge bridge;
    for (int i = 0; i < 5; ++i) bridge.polls.push_back(pollErr());

    DeepAnalysisPollConfig cfg;
    cfg.maxConsecutiveErrors = 3; // give up after 3 in a row
    cfg.deadlineMs = 1'000'000;   // deadline must NOT be what ends this

    auto r = runDeepAnalysisJob(bridge, "in.wav", silentHooks(), cfg);

    CHECK(r.outcome == DeepAnalysisOutcome::TimedOutOrError);
    CHECK_FALSE(r.hasResponse);
    // A local timeout/error is the runner's own decision, so it must cancel
    // the still-running server job rather than abandon it.
    CHECK(bridge.cancelCount == 1);
}

TEST_CASE("runDeepAnalysisJob: a single transient poll error does not trip the budget", "[deepjob][cq04]")
{
    FakeBridge bridge;
    bridge.polls.push_back(statusOk("running"));
    bridge.polls.push_back(pollErr()); // one hiccup
    bridge.polls.push_back(statusOk("completed", /*hasResult=*/true));

    DeepAnalysisPollConfig cfg;
    cfg.maxConsecutiveErrors = 3;

    auto r = runDeepAnalysisJob(bridge, "in.wav", silentHooks(), cfg);

    CHECK(r.outcome == DeepAnalysisOutcome::Completed);
    CHECK(bridge.cancelCount == 0);
}

TEST_CASE("runDeepAnalysisJob: the wall-clock deadline ends the loop and cancels", "[deepjob][cq04]")
{
    FakeBridge bridge; // no scripted polls -> always "running"

    // Injected clock jumps past the deadline on the second read.
    unsigned int fakeNow = 0;
    DeepAnalysisJobHooks hooks = silentHooks();
    hooks.now = [&fakeNow] {
        const unsigned int t = fakeNow;
        fakeNow += 100'000; // +100s per read
        return t;
    };

    DeepAnalysisPollConfig cfg;
    cfg.deadlineMs = 120'000;

    auto r = runDeepAnalysisJob(bridge, "in.wav", hooks, cfg);

    CHECK(r.outcome == DeepAnalysisOutcome::TimedOutOrError);
    CHECK(bridge.cancelCount == 1);
}

TEST_CASE("runDeepAnalysisJob: an external cancel abandons the job WITHOUT self-POSTing a cancel", "[deepjob][cq04]")
{
    FakeBridge bridge;
    for (int i = 0; i < 5; ++i) bridge.polls.push_back(statusOk("running"));

    DeepAnalysisJobHooks hooks = silentHooks();
    hooks.shouldCancel = [] { return true; }; // session switched / newer request

    auto r = runDeepAnalysisJob(bridge, "in.wav", hooks);

    CHECK(r.outcome == DeepAnalysisOutcome::Cancelled);
    CHECK_FALSE(r.hasResponse);
    // The external canceller (AnalysisController::cancel()) owns POSTing the
    // cancel via the recorded job id; the runner must not double-cancel.
    CHECK(bridge.cancelCount == 0);
}

TEST_CASE("runDeepAnalysisJob: a server-side 'failed' status is reported as FailedServerSide", "[deepjob][cq04]")
{
    FakeBridge bridge;
    bridge.polls.push_back(statusOk("running"));
    bridge.polls.push_back(statusOk("failed"));

    auto r = runDeepAnalysisJob(bridge, "in.wav", silentHooks());

    CHECK(r.outcome == DeepAnalysisOutcome::FailedServerSide);
    CHECK_FALSE(r.hasResponse);
    CHECK(bridge.analyzeCount == 0); // nothing to re-fetch
}

TEST_CASE("runDeepAnalysisJob: onJobId reports the started job id", "[deepjob][cq04]")
{
    FakeBridge bridge;
    bridge.polls.push_back(statusOk("completed", /*hasResult=*/true));

    std::string reportedId;
    DeepAnalysisJobHooks hooks = silentHooks();
    hooks.onJobId = [&reportedId](const std::string& id) { reportedId = id; };

    runDeepAnalysisJob(bridge, "in.wav", hooks);

    CHECK(reportedId == "job-1");
}

TEST_CASE("runDeepAnalysisJob says when the engine did not answer, as opposed to refusing the request (FIX-2)",
          "[deepjob][fix2]")
{
    DeepAnalysisPollConfig cfg;
    cfg.maxConsecutiveErrors = 3;
    cfg.deadlineMs = 1'000'000;

    SECTION("the start was refused: an answer about the request")
    {
        FakeBridge bridge;
        bridge.startOk = false;
        bridge.startError = {BridgeError::Kind::HttpError, "undecodable", 422};
        CHECK_FALSE(runDeepAnalysisJob(bridge, "in.wav", silentHooks(), cfg).engineDidNotAnswer);
    }
    SECTION("the engine was down at the start")
    {
        FakeBridge bridge;
        bridge.startOk = false;
        bridge.startError = {BridgeError::Kind::ServiceUnavailable, "down"};
        CHECK(runDeepAnalysisJob(bridge, "in.wav", silentHooks(), cfg).engineDidNotAnswer);
    }
    SECTION("the engine was shedding load at the start")
    {
        FakeBridge bridge;
        bridge.startOk = false;
        bridge.startError = {BridgeError::Kind::HttpError, "busy", 429};
        CHECK(runDeepAnalysisJob(bridge, "in.wav", silentHooks(), cfg).engineDidNotAnswer);
    }
    SECTION("the polls kept failing")
    {
        FakeBridge bridge;
        for (int i = 0; i < 5; ++i) bridge.polls.push_back(pollErr());
        const auto r = runDeepAnalysisJob(bridge, "in.wav", silentHooks(), cfg);
        CHECK(r.outcome == DeepAnalysisOutcome::TimedOutOrError);
        CHECK(r.engineDidNotAnswer);
    }
    SECTION("the deadline passed")
    {
        FakeBridge bridge;
        unsigned int fakeNow = 0;
        DeepAnalysisJobHooks hooks = silentHooks();
        hooks.now = [&fakeNow] {
            const unsigned int t = fakeNow;
            fakeNow += 100'000;
            return t;
        };
        DeepAnalysisPollConfig deadline;
        deadline.deadlineMs = 120'000;
        CHECK(runDeepAnalysisJob(bridge, "in.wav", hooks, deadline).engineDidNotAnswer);
    }
    SECTION("the engine shut down under the job")
    {
        FakeBridge bridge;
        bridge.polls.push_back(statusOk("interrupted"));
        const auto r = runDeepAnalysisJob(bridge, "in.wav", silentHooks(), cfg);
        CHECK(r.outcome == DeepAnalysisOutcome::FailedServerSide);
        CHECK(r.engineDidNotAnswer);
    }
    SECTION("the job failed: an answer about the file")
    {
        FakeBridge bridge;
        bridge.polls.push_back(statusOk("failed"));
        CHECK_FALSE(runDeepAnalysisJob(bridge, "in.wav", silentHooks(), cfg).engineDidNotAnswer);
    }
    SECTION("the job completed")
    {
        FakeBridge bridge;
        bridge.polls.push_back(statusOk("completed", /*hasResult=*/true));
        CHECK_FALSE(runDeepAnalysisJob(bridge, "in.wav", silentHooks(), cfg).engineDidNotAnswer);
    }
    SECTION("the caller cancelled")
    {
        FakeBridge bridge;
        DeepAnalysisJobHooks hooks = silentHooks();
        hooks.shouldCancel = [] { return true; };
        const auto r = runDeepAnalysisJob(bridge, "in.wav", hooks, cfg);
        CHECK(r.outcome == DeepAnalysisOutcome::Cancelled);
        CHECK_FALSE(r.engineDidNotAnswer);
    }
}

TEST_CASE("runDeepAnalysisJob carries the service's failure detail and honours a server-side cancel",
          "[deepjob][merge-a8cf]")
{
    SECTION("a failed job keeps its error text")
    {
        FakeBridge bridge;
        AnalysisJobStatus st;
        st.status = "failed";
        st.error = "decoder exploded";
        bridge.polls.push_back(Result<AnalysisJobStatus, BridgeError>::ok(st));
        const auto r = runDeepAnalysisJob(bridge, "in.wav", silentHooks());
        CHECK(r.outcome == DeepAnalysisOutcome::FailedServerSide);
        CHECK(r.errorMessage == "decoder exploded");
    }
    SECTION("a job the service cancelled is Cancelled, not an empty Completed")
    {
        FakeBridge bridge;
        bridge.polls.push_back(statusOk("cancelled"));
        const auto r = runDeepAnalysisJob(bridge, "in.wav", silentHooks());
        CHECK(r.outcome == DeepAnalysisOutcome::Cancelled);
        CHECK_FALSE(r.hasResponse);
    }
}

TEST_CASE("runDeepAnalysisJob starts the job in the hooks' mode (FEAT-1 fix check)", "[deepjob][feat1]")
{
    using Mode = khensu::ipc::ILocalServiceBridge::AnalysisMode;
    FakeBridge bridge;
    bridge.polls.push_back(statusOk("completed", /*hasResult=*/true));
    auto hooks = silentHooks();
    runDeepAnalysisJob(bridge, "in.wav", hooks);
    CHECK(bridge.startedMode == Mode::Advanced);

    FakeBridge fixBridge;
    fixBridge.polls.push_back(statusOk("completed", /*hasResult=*/true));
    hooks.mode = Mode::FixCheck;
    runDeepAnalysisJob(fixBridge, "in.wav", hooks);
    CHECK(fixBridge.startedMode == Mode::FixCheck);
}
