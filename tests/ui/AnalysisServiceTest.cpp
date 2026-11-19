// tests/ui/AnalysisServiceTest.cpp
// Pins AnalysisService (ENG-1 stage C5, extracted from AppController): the
// results gate (Analyzing -> Open on the final result; the follow-up scans
// counted but not waited for, PERF-1; stale batch tokens ignored, cancel), the run token, partial results never
// published, the failure path, the service-recovered decision, the pipeline
// listeners and the reference track - without an AppController or a live
// service. The fixture switches the pipeline off (skipPipelineForTesting);
// results are fed through the documented deliver*ForTesting seam, the same
// handler the AnalysisController calls. Background work is counted, not run.
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "IBackgroundWork.h"
#include "session/AnalysisService.h"
#include "session/SessionService.h"
#include <khensu/analysis/AnalysisInputs.h>
#include <khensu/targets/GenreConfirmation.h>

using khensu::AnalysisResult;
using khensu::AnalysisService;
using khensu::SessionService;
using Gate = khensu::AnalysisService::ResultsGate;

namespace {
struct CountingWork final : khensu::IBackgroundWork {
    int jobs = 0;
    void runInBackground(std::function<void()>) override { ++jobs; }
    void runLongInBackground(std::function<void()>) override { ++jobs; }
    uint64_t sessionEpoch() const noexcept override { return 0; }
    bool isShuttingDown() const noexcept override { return false; }
};

struct Fixture {
    CountingWork work;
    SessionService sessions;
    AnalysisService analysis{work, sessions};

    std::vector<Gate> gates;
    juce::StringArray statuses;
    std::vector<std::pair<int, int>> followUps;
    juce::String failure;
    int finals = 0, cancels = 0, referenceChanges = 0;

    explicit Fixture(bool pipeline = false)
    {
        if (!pipeline) analysis.skipPipelineForTesting();
        analysis.attachFeedbackManager(nullptr);
        analysis.onGateChanged = [this](Gate g) { gates.push_back(g); };
        analysis.onStatus = [this](const juce::String& text, bool, bool) { statuses.add(text); };
        analysis.onFollowUpsChanged = [this](int pending, int total) { followUps.emplace_back(pending, total); };
        analysis.onFailed = [this](const juce::String& message) { failure = message; };
        analysis.onFinalResult = [this] { ++finals; };
        analysis.onCancelled = [this] { ++cancels; };
        analysis.onReferenceChanged = [this] { ++referenceChanges; };
    }

    void openSession(const char* id = "session-1")
    {
        khensu::Session session;
        session.id = id;
        session.name = "t";
        sessions.adopt(std::move(session)); // no files
    }

    static AnalysisResult finalResult(bool deep = true)
    {
        AnalysisResult r;
        r.loudness.lufs_integrated = -11.0f;
        r.python_analysis_ran = deep;
        return r;
    }
};
} // namespace

TEST_CASE("AnalysisService withholds results until the final result, not its follow-up scans (PERF-1)",
          "[split][analysis]")
{
    Fixture f;
    f.openSession();
    const int run0 = f.analysis.analysisRunToken();
    f.analysis.beginAnalysis();
    CHECK(f.analysis.analysisRunToken() == run0 + 1);
    CHECK(f.analysis.resultsGate() == Gate::Analyzing);
    CHECK(f.analysis.resultsPending());
    CHECK(f.analysis.analysisInFlight());
    CHECK(f.statuses.contains("Analyzing..."));

    // The Dashboard registers its scans inside the final broadcast, while
    // the gate is still closed (so it knows to count them).
    int batch = 0, broadcasts = 0;
    int key = 0;
    f.analysis.addAnalysisListener(&key, [&](const AnalysisResult&) {
        ++broadcasts;
        CHECK(f.analysis.resultsPending());
        batch = f.analysis.startFollowUpScanBatch();
        f.analysis.addFollowUpScan(batch);
        f.analysis.addFollowUpScan(batch);
    });

    f.analysis.deliverResultForTesting(Fixture::finalResult());
    CHECK(broadcasts == 1);
    CHECK(f.finals == 1);
    CHECK(f.analysis.hasAnalysis());
    CHECK(f.analysis.currentAnalysis().loudness.lufs_integrated == -11.0f);
    CHECK_FALSE(f.analysis.analysisInFlight());
    // Open at once: the scans can't change a grade, so they don't hold it.
    CHECK(f.analysis.resultsGate() == Gate::Open);
    CHECK_FALSE(f.analysis.resultsPending());
    CHECK(f.gates == std::vector<Gate>{Gate::Analyzing, Gate::Open});
    CHECK(f.analysis.followUpPending() == 2);
    REQUIRE(f.followUps.size() == 2);
    CHECK(f.followUps.back() == std::make_pair(2, 2));

    // A scan from an older batch neither counts nor finishes anything.
    f.analysis.followUpScanFinished(batch - 1);
    f.analysis.addFollowUpScan(batch - 1);
    CHECK(f.analysis.followUpPending() == 2);

    f.analysis.followUpScanFinished(batch);
    CHECK(f.followUps.back() == std::make_pair(1, 2));
    f.analysis.followUpScanFinished(batch);
    CHECK(f.followUps.back() == std::make_pair(0, 2));
    CHECK(f.analysis.followUpPending() == 0);

    // A late extra report neither goes negative nor touches the gate.
    f.analysis.followUpScanFinished(batch);
    CHECK(f.analysis.followUpPending() == 0);
    CHECK(f.followUps.size() == 4);
    CHECK(f.gates.size() == 2);
    f.analysis.removeAnalysisListener(&key);
}

TEST_CASE("AnalysisService: a session with no files is not left \"Analyzing\" (FIX-6)", "[split][analysis][fix6]")
{
    Fixture f{/*pipeline*/ true};
    f.openSession(); // no files: the pipeline has nothing to run and would never report
    f.analysis.beginAnalysis();
    CHECK_FALSE(f.analysis.analysisInFlight());
    CHECK(f.failure.isNotEmpty());
    CHECK(f.analysis.resultsGate() == Gate::Failed);
    CHECK(f.finals == 0);
}

TEST_CASE("AnalysisService opens at once when no follow-up scan starts", "[split][analysis]")
{
    Fixture f;
    f.openSession();
    f.analysis.beginAnalysis();
    f.analysis.deliverResultForTesting(Fixture::finalResult());
    CHECK(f.gates == std::vector<Gate>{Gate::Analyzing, Gate::Open});
    CHECK(f.work.jobs == 0); // no store attached: nothing to save
}

TEST_CASE("AnalysisService never publishes a partial result", "[split][analysis]")
{
    Fixture f;
    f.openSession();
    f.analysis.beginAnalysis();
    int broadcasts = 0;
    int key = 0;
    f.analysis.addAnalysisListener(&key, [&](const AnalysisResult&) { ++broadcasts; });
    int provisionals = 0;
    float provisionalLufs = 0.0f;
    f.analysis.onProvisionalResult = [&](const AnalysisResult& fast) {
        ++provisionals;
        provisionalLufs = fast.loudness.lufs_integrated;
    };
    auto partial = Fixture::finalResult(false);
    partial.is_partial = true;
    f.analysis.deliverResultForTesting(partial);
    CHECK(broadcasts == 0);
    // ...but its plain measurements go to the owner, marked provisional (PERF-1).
    CHECK(provisionals == 1);
    CHECK(provisionalLufs == -11.0f);
    CHECK_FALSE(f.analysis.hasAnalysis());
    CHECK(f.analysis.resultsGate() == Gate::Analyzing);
    CHECK(f.analysis.analysisInFlight());
    CHECK(f.statuses.contains("Deep analysis..."));
    f.analysis.removeAnalysisListener(&key);
}

TEST_CASE("AnalysisService reports a failed run without publishing", "[split][analysis]")
{
    Fixture f;
    f.openSession();
    f.analysis.beginAnalysis();
    AnalysisResult failed;
    failed.analysis_failed = true;
    failed.error_message = "decode failed";
    f.analysis.deliverResultForTesting(failed);
    CHECK(f.analysis.resultsGate() == Gate::Failed);
    CHECK(f.failure == "decode failed");
    CHECK_FALSE(f.analysis.hasAnalysis());
    CHECK_FALSE(f.analysis.analysisInFlight());
    CHECK(f.finals == 0);
}

TEST_CASE("AnalysisService cancel keeps a finished result but stops a running analysis", "[split][analysis]")
{
    SECTION("while follow-up scans run: nothing to cancel, the result stays")
    {
        Fixture f;
        f.openSession();
        f.analysis.beginAnalysis();
        int batch = 0;
        int key = 0;
        f.analysis.addAnalysisListener(&key, [&](const AnalysisResult&) {
            batch = f.analysis.startFollowUpScanBatch();
            f.analysis.addFollowUpScan(batch);
        });
        f.analysis.deliverResultForTesting(Fixture::finalResult());
        REQUIRE(f.analysis.resultsGate() == Gate::Open);
        f.analysis.cancelAnalysis();
        CHECK(f.analysis.resultsGate() == Gate::Open);
        CHECK(f.analysis.hasAnalysis());
        CHECK(f.cancels == 0);
        f.analysis.followUpScanFinished(batch);
        CHECK(f.gates.back() == Gate::Open);
        f.analysis.removeAnalysisListener(&key);
    }
    SECTION("while analysing: cancelled, nothing published")
    {
        Fixture f;
        f.openSession();
        f.analysis.beginAnalysis();
        f.analysis.cancelAnalysis();
        CHECK(f.analysis.resultsGate() == Gate::Cancelled);
        CHECK(f.cancels == 1);
        CHECK(f.statuses.contains("Cancelled"));
        CHECK_FALSE(f.analysis.analysisInFlight());
        CHECK_FALSE(f.analysis.hasAnalysis());
        // A second cancel (or one once open) is a no-op.
        f.analysis.cancelAnalysis();
        CHECK(f.cancels == 1);
    }
}

TEST_CASE("AnalysisService session reset forgets the analysis and opens the gate silently", "[split][analysis]")
{
    Fixture f;
    f.openSession();
    f.analysis.setReferenceTrack("C:/refs/ref.wav");
    CHECK(f.analysis.getReferenceTrack() == "C:/refs/ref.wav");
    CHECK(f.referenceChanges == 1);

    f.analysis.beginAnalysis();
    f.analysis.deliverResultForTesting(Fixture::finalResult());
    REQUIRE(f.analysis.hasAnalysis());
    const auto gatesBefore = f.gates.size();
    const int run = f.analysis.analysisRunToken();
    f.analysis.beginAnalysis(); // a re-run in flight when the session changes
    f.analysis.resetForSession();
    CHECK(f.analysis.analysisRunToken() == run + 2);
    CHECK(f.analysis.resultsGate() == Gate::Open);
    CHECK(f.gates.size() == gatesBefore + 1); // only the re-run's Analyzing; the reset fires nothing
    CHECK_FALSE(f.analysis.hasAnalysis());
    CHECK_FALSE(f.analysis.analysisModel().hasResult());
    CHECK_FALSE(f.analysis.analysisInFlight());
    CHECK(f.analysis.getReferenceTrack().isEmpty());
    CHECK(f.referenceChanges == 1); // cleared without an autosave (AUDIT-039)
}

TEST_CASE("AnalysisService re-runs only an analysis missing its deep half when the service recovers",
          "[split][analysis]")
{
    Fixture f;
    int recovered = 0;
    int key = 0;
    f.analysis.addServiceRecoveredListener(&key, [&] { ++recovered; });

    // No session: nothing happens.
    f.analysis.reanalyzeIfServiceRecovered();
    CHECK(recovered == 0);
    CHECK(f.gates.empty());

    f.openSession();
    // In flight: left alone.
    f.analysis.beginAnalysis();
    const int run = f.analysis.analysisRunToken();
    f.analysis.reanalyzeIfServiceRecovered();
    CHECK(f.analysis.analysisRunToken() == run);

    // Deep half present: the listeners re-run their scans, no re-analysis.
    f.analysis.deliverResultForTesting(Fixture::finalResult(/*deep*/ true));
    f.analysis.reanalyzeIfServiceRecovered();
    CHECK(recovered == 1);
    CHECK(f.analysis.analysisRunToken() == run);

    // The engine was asked and did not answer: re-analysed.
    f.analysis.beginAnalysis();
    auto unanswered = Fixture::finalResult(/*deep*/ false);
    unanswered.inputs_failed = khensu::analysis_inputs::serviceInputs(/*withReference*/ false);
    f.analysis.deliverResultForTesting(unanswered);
    const int run2 = f.analysis.analysisRunToken();
    f.analysis.reanalyzeIfServiceRecovered();
    CHECK(recovered == 1);
    CHECK(f.analysis.analysisRunToken() == run2 + 1);
    CHECK(f.analysis.resultsGate() == Gate::Analyzing);

    f.analysis.removeServiceRecoveredListener(&key);
}

TEST_CASE("AnalysisService leaves alone on recovery what the engine was never asked for (FIX-2)",
          "[split][analysis][fix2]")
{
    namespace in = khensu::analysis_inputs;
    Fixture f;
    f.openSession();
    int recovered = 0;
    int key = 0;
    f.analysis.addServiceRecoveredListener(&key, [&] { ++recovered; });

    // Deep analysis was off for this session: the result has no deep half,
    // and nothing failed. Results stay on screen; the scans may re-run.
    f.analysis.beginAnalysis();
    f.analysis.deliverResultForTesting(Fixture::finalResult(/*deep*/ false));
    int run = f.analysis.analysisRunToken();
    f.analysis.reanalyzeIfServiceRecovered();
    CHECK(f.analysis.analysisRunToken() == run);
    CHECK(f.analysis.resultsGate() == Gate::Open);
    CHECK(recovered == 1);

    // Cancelled by the user: stays cancelled.
    f.analysis.beginAnalysis();
    f.analysis.cancelAnalysis();
    run = f.analysis.analysisRunToken();
    f.analysis.reanalyzeIfServiceRecovered();
    CHECK(f.analysis.analysisRunToken() == run);
    CHECK(f.analysis.resultsGate() == Gate::Cancelled);

    // Failed on the file itself: the failure is not raised a second time.
    f.analysis.beginAnalysis();
    AnalysisResult failed;
    failed.analysis_failed = true;
    failed.error_message = "missing";
    f.analysis.deliverResultForTesting(failed);
    f.failure.clear();
    run = f.analysis.analysisRunToken();
    f.analysis.reanalyzeIfServiceRecovered();
    CHECK(f.analysis.analysisRunToken() == run);
    CHECK(f.analysis.resultsGate() == Gate::Failed);
    CHECK(f.failure.isEmpty());

    // One part of the pipeline went unanswered (the deep half is there):
    // that is worth a re-run too.
    f.analysis.beginAnalysis();
    auto noDrums = Fixture::finalResult(/*deep*/ true);
    noDrums.inputs_failed = in::kDrums;
    f.analysis.deliverResultForTesting(noDrums);
    run = f.analysis.analysisRunToken();
    f.analysis.reanalyzeIfServiceRecovered();
    CHECK(f.analysis.analysisRunToken() == run + 1);
    CHECK(f.analysis.resultsGate() == Gate::Analyzing);

    // The re-run came back unanswered as well (a busy engine misses its
    // health probes and "recovers" again): no second automatic run, or the
    // results would be hidden over and over.
    f.analysis.deliverResultForTesting(noDrums);
    run = f.analysis.analysisRunToken();
    f.analysis.reanalyzeIfServiceRecovered();
    CHECK(f.analysis.analysisRunToken() == run);
    CHECK(f.analysis.resultsGate() == Gate::Open);

    // The user's own re-run earns one more.
    f.analysis.reanalyzeActiveSession();
    f.analysis.deliverResultForTesting(noDrums);
    run = f.analysis.analysisRunToken();
    f.analysis.reanalyzeIfServiceRecovered();
    CHECK(f.analysis.analysisRunToken() == run + 1);

    f.analysis.removeServiceRecoveredListener(&key);
}

TEST_CASE("AnalysisService fans pipeline stages out after the owner's stage hook", "[split][analysis]")
{
    Fixture f;
    std::vector<std::string> order;
    f.analysis.onStage = [&](const std::string& stage, int, int) { order.push_back("hook:" + stage); };
    int a = 0, b = 0;
    f.analysis.addPipelineProgressListener(&a, [&](const std::string& stage, int idx, int total) {
        order.push_back("a:" + stage + ":" + std::to_string(idx) + "/" + std::to_string(total));
        f.analysis.removePipelineProgressListener(&a); // deregistering mid-fan-out is safe
    });
    f.analysis.addPipelineProgressListener(&b,
                                           [&](const std::string& stage, int, int) { order.push_back("b:" + stage); });

    f.analysis.deliverStageForTesting("fast", 1, 6);
    REQUIRE(order.size() == 3);
    CHECK(order[0] == "hook:fast");

    // The stems stage reaches the listeners without the analysis hook.
    order.clear();
    f.analysis.notifyPipelineProgress("stems:running:0.5", 6, 6);
    CHECK(order == std::vector<std::string>{"b:stems:running:0.5"});
    f.analysis.removePipelineProgressListener(&b);
}

TEST_CASE("AnalysisService drops a result for a session that is no longer open (DSK-02)", "[split][analysis]")
{
    Fixture f;
    f.openSession("session-2");
    f.analysis.beginAnalysis();
    int broadcasts = 0;
    int key = 0;
    f.analysis.addAnalysisListener(&key, [&](const AnalysisResult&) { ++broadcasts; });

    // A run started for the previous session delivers after the switch: its
    // final, its failure and its partial all go nowhere.
    f.analysis.deliverResultForTesting(Fixture::finalResult(), "session-1");
    AnalysisResult failed;
    failed.analysis_failed = true;
    failed.error_message = "old run failed";
    f.analysis.deliverResultForTesting(failed, "session-1");
    CHECK(broadcasts == 0);
    CHECK(f.finals == 0);
    CHECK(f.failure.isEmpty());
    CHECK_FALSE(f.analysis.hasAnalysis());
    CHECK(f.analysis.analysisInFlight());
    CHECK(f.analysis.resultsGate() == Gate::Analyzing);
    CHECK(f.gates == std::vector<Gate>{Gate::Analyzing});

    // The active session's own result still opens the gate.
    f.analysis.deliverResultForTesting(Fixture::finalResult(), "session-2");
    CHECK(broadcasts == 1);
    CHECK(f.analysis.resultsGate() == Gate::Open);
    f.analysis.removeAnalysisListener(&key);
}

TEST_CASE("AnalysisService: a follow-up scan that never reports holds nothing (PERF-1)", "[split][analysis]")
{
    Fixture f;
    f.openSession();
    int key = 0;
    int batch = 0;
    f.analysis.addAnalysisListener(&key, [&](const AnalysisResult&) {
        batch = f.analysis.startFollowUpScanBatch();
        f.analysis.addFollowUpScan(batch); // e.g. its panel was destroyed mid-request
    });
    f.analysis.beginAnalysis();
    f.analysis.deliverResultForTesting(Fixture::finalResult());
    CHECK(f.analysis.resultsGate() == Gate::Open);
    CHECK(f.analysis.followUpPending() == 1);

    // The next run forgets the stuck scan's count.
    f.analysis.beginAnalysis();
    CHECK(f.analysis.followUpPending() == 0);
    f.analysis.followUpScanFinished(batch); // stale batch: ignored
    CHECK(f.analysis.followUpPending() == 0);
    f.analysis.removeAnalysisListener(&key);
}

TEST_CASE("AnalysisService allows genre-relative actions once the genre is confirmed (P0-1)", "[split][analysis][genre]")
{
    Fixture f;
    f.openSession();
    // No analysis yet: never allowed, and the reason says why.
    CHECK_FALSE(f.analysis.genreRelativeActionAllowed());
    CHECK(f.analysis.genreRelativeAbstainedReason() == "Analyze the session first.");

    f.analysis.beginAnalysis();
    auto deep = Fixture::finalResult();
    deep.genre_relative_action_allowed = false; // what the cached deep response always says
    f.analysis.deliverResultForTesting(deep);
    REQUIRE(f.analysis.hasAnalysis());

    // A detected candidate alone is not a confirmation.
    CHECK_FALSE(f.analysis.genreRelativeActionAllowed());
    CHECK(f.analysis.genreRelativeAbstainedReason().contains("Confirm the genre"));

    // A genre the user stated at intake allows the Mastering Agent & co.
    f.sessions.state().session->target.genre_confirmed_source = khensu::genre_confirmation::kUserStated;
    CHECK(f.analysis.genreRelativeActionAllowed());
    CHECK(f.analysis.genreRelativeAbstainedReason().isEmpty());

    // Targets turned off at intake override the confirmation.
    f.sessions.state().session->target.use_genre_targets = false;
    CHECK_FALSE(f.analysis.genreRelativeActionAllowed());
    CHECK(f.analysis.genreRelativeAbstainedReason().contains("turned off"));

    // Unconfirmed, but a loaded reference track allows it.
    f.sessions.state().session->target.use_genre_targets = true;
    f.sessions.state().session->target.genre_confirmed_source.clear();
    CHECK_FALSE(f.analysis.genreRelativeActionAllowed());
    f.analysis.setReferenceTrack("C:/refs/reference.wav");
    CHECK(f.analysis.genreRelativeActionAllowed());
}
