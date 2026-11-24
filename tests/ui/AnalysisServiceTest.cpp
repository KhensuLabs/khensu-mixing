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
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>
#include "IBackgroundWork.h"
#include "analysis/AnalysisController.h"
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

    // The engine was asked and did not answer: re-analysed - in place
    // (FN2-2b): the result stays on screen and the gate open meanwhile.
    f.analysis.beginAnalysis();
    auto unanswered = Fixture::finalResult(/*deep*/ false);
    unanswered.inputs_failed = khensu::analysis_inputs::serviceInputs(/*withReference*/ false);
    f.analysis.deliverResultForTesting(unanswered);
    const int run2 = f.analysis.analysisRunToken();
    f.analysis.reanalyzeIfServiceRecovered();
    CHECK(recovered == 1);
    CHECK(f.analysis.analysisRunToken() == run2 + 1);
    CHECK(f.analysis.resultsGate() == Gate::Open);
    CHECK(f.analysis.upgradeInFlight());
    CHECK(f.analysis.hasAnalysis());

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
    // that is worth a re-run too - in place (FN2-2b), the results stay.
    f.analysis.beginAnalysis();
    auto noDrums = Fixture::finalResult(/*deep*/ true);
    noDrums.inputs_failed = in::kDrums;
    f.analysis.deliverResultForTesting(noDrums);
    run = f.analysis.analysisRunToken();
    f.analysis.reanalyzeIfServiceRecovered();
    CHECK(f.analysis.analysisRunToken() == run + 1);
    CHECK(f.analysis.resultsGate() == Gate::Open);
    const auto analyzingSoFar = std::count(f.gates.begin(), f.gates.end(), Gate::Analyzing);

    // The re-run came back unanswered as well. Not one-shot any more (the
    // results are not hidden by a re-run), but bounded: while the engine
    // stays up - every health tick reports Running - nothing repeats it, a
    // busy engine answers no better now...
    f.analysis.deliverResultForTesting(noDrums);
    f.analysis.setEngineStatus(khensu::PythonStatus::Running); // an up-edge: the 2nd re-run
    CHECK(f.analysis.analysisRunToken() == run + 2);
    f.analysis.deliverResultForTesting(noDrums);
    f.analysis.setEngineStatus(khensu::PythonStatus::Running); // still up: nothing
    f.analysis.setEngineStatus(khensu::PythonStatus::Running);
    CHECK(f.analysis.analysisRunToken() == run + 2);
    CHECK_FALSE(f.analysis.analysisInFlight());
    // ... each time it comes back up, one more, up to kMaxRecoveryReruns.
    f.analysis.setEngineStatus(khensu::PythonStatus::Stopped);
    f.analysis.setEngineStatus(khensu::PythonStatus::Running);
    CHECK(f.analysis.analysisRunToken() == run + 3);
    f.analysis.deliverResultForTesting(noDrums);
    f.analysis.setEngineStatus(khensu::PythonStatus::Stopped);
    f.analysis.setEngineStatus(khensu::PythonStatus::Running);
    f.analysis.reanalyzeIfServiceRecovered();
    CHECK(f.analysis.analysisRunToken() == run + 3);
    CHECK(f.analysis.resultsGate() == Gate::Open);
    // None of them hid the results.
    CHECK(std::count(f.gates.begin(), f.gates.end(), Gate::Analyzing) == analyzingSoFar);
    CHECK(AnalysisService::kMaxRecoveryReruns == 3);

    // The user's own re-run earns them again.
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

// ---- FN2-2b: native findings before the engine, then the deep pass in place ----

namespace {

template <typename Pred> bool pumpUntil(Pred pred, int timeoutMs = 15000)
{
    auto* mm = juce::MessageManager::getInstance();
    const auto deadline = juce::Time::currentTimeMillis() + timeoutMs;
    while (!pred()) {
        if (juce::Time::currentTimeMillis() > deadline) return false;
        mm->runDispatchLoopUntil(20);
    }
    return true;
}

/// Two seconds of a stereo sine in a temp folder: enough for the native pass.
struct SineFile {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu-fn2-2b-" + juce::Uuid().toString());
    juce::File file;
    SineFile()
    {
        dir.createDirectory();
        file = dir.getChildFile("track.wav");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        REQUIRE(stream != nullptr);
        const auto options =
            juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(16);
        auto writer = wav.createWriterFor(stream, options);
        REQUIRE(writer != nullptr);
        juce::AudioBuffer<float> buf(2, 88200);
        for (int i = 0; i < buf.getNumSamples(); ++i) {
            const float v = 0.25f * std::sin(juce::MathConstants<float>::twoPi * 440.0f * (float)i / 44100.0f);
            buf.setSample(0, i, v);
            buf.setSample(1, i, v);
        }
        writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
    }
    ~SineFile() { dir.deleteRecursively(); }

    khensu::Session session(const char* id) const
    {
        khensu::Session s;
        s.id = id;
        s.name = "t";
        khensu::AudioFileRef f;
        f.id = "file-1";
        f.path = file.getFullPathName().toStdString();
        f.format = "wav";
        s.files.push_back(f);
        return s;
    }
};

/// An engine that is down until `up`, answers a deep job (after `release`,
/// or gives up when the run is superseded) and answers every scan with an
/// error about the request (an answer, not "unanswered").
struct BootingEngine {
    std::atomic<bool> up{false}, release{true};
    std::atomic<int> deepCalls{0}, deepCancelled{0};

    khensu::AnalysisServiceCalls calls()
    {
        using khensu::Result;
        using Error = khensu::ipc::BridgeError;
        const Error none{Error::Kind::HttpError, "not for this test", 422};
        khensu::AnalysisServiceCalls c;
        c.available = [this] { return up.load(); };
        c.knownNotReady = [this] { return !up.load(); };
        c.deep = [this](const std::string&, const khensu::DeepAnalysisJobHooks& hooks) {
            ++deepCalls;
            khensu::DeepAnalysisJobResult r;
            const auto deadline = juce::Time::currentTimeMillis() + 10000;
            while (!release && juce::Time::currentTimeMillis() < deadline) {
                if (hooks.shouldCancel()) {
                    ++deepCancelled;
                    r.outcome = khensu::DeepAnalysisOutcome::Cancelled;
                    return r;
                }
                juce::Thread::sleep(2);
            }
            r.outcome = khensu::DeepAnalysisOutcome::Completed;
            r.hasResponse = true; // an empty response still marks the deep pass as run
            return r;
        };
        c.sections = [none](const std::string&) { return Result<khensu::SectionAnalysis, Error>::err(none); };
        c.drums = [none](const std::string&) { return Result<AnalysisResult::DrumData, Error>::err(none); };
        c.reference = [none](const std::string&, const std::string&) {
            return Result<khensu::ReferenceDeltas, Error>::err(none);
        };
        c.chords = [none](const std::string&) { return Result<khensu::ChordAnalysis, Error>::err(none); };
        c.trust = [none](const std::string&) { return Result<juce::var, Error>::err(none); };
        return c;
    }
};

} // namespace

TEST_CASE("AnalysisService: native findings publish at once while the engine starts; the deep pass replaces "
          "them in place (FN2-2b)",
          "[split][analysis][fn2-2b]")
{
    namespace in = khensu::analysis_inputs;
    Fixture f(/*pipeline*/ true);
    BootingEngine engine;
    f.analysis.setServiceCallsForTesting(engine.calls());
    f.analysis.setEngineStatus(khensu::PythonStatus::Starting);
    SineFile track;
    f.sessions.adopt(track.session("s-cold"));
    int broadcasts = 0, key = 0;
    f.analysis.addAnalysisListener(&key, [&](const AnalysisResult&) { ++broadcasts; });

    const auto t0 = juce::Time::getMillisecondCounter();
    f.analysis.beginAnalysis();
    REQUIRE(pumpUntil([&] { return f.analysis.resultsGate() == Gate::Open && f.analysis.hasAnalysis(); }));
    const auto elapsed = juce::Time::getMillisecondCounter() - t0;
    INFO("native findings after " << elapsed << " ms");
    CHECK(elapsed < 6000); // not held for the engine's 8 s wait
    // A complete native result: published (not partial), the deep inputs unanswered.
    CHECK(broadcasts == 1);
    CHECK_FALSE(f.analysis.currentAnalysis().is_partial);
    CHECK_FALSE(f.analysis.currentAnalysis().has_python_analysis());
    CHECK((f.analysis.currentAnalysis().inputs_failed & in::kDeep) != 0);
    CHECK(f.analysis.deepPassState() == khensu::DeepPassState::EngineUnavailable);
    CHECK(engine.deepCalls == 0);
    CHECK(f.gates == std::vector<Gate>{Gate::Analyzing, Gate::Open});
    const auto version = f.analysis.analysisVersion();
    const int run = f.analysis.analysisRunToken();

    // The engine comes up: the analysis re-runs in place - the gate stays
    // open, the native result stays readable - until the full one lands.
    engine.release = false;
    engine.up = true;
    f.analysis.setEngineStatus(khensu::PythonStatus::Running);
    CHECK(f.analysis.upgradeInFlight());
    CHECK(f.analysis.analysisRunToken() == run + 1);
    REQUIRE(pumpUntil([&] { return engine.deepCalls.load() == 1; }));
    CHECK(f.analysis.resultsGate() == Gate::Open);
    CHECK_FALSE(f.analysis.resultsPending());
    CHECK(f.analysis.hasAnalysis());
    CHECK((f.analysis.currentAnalysis().inputs_failed & in::kDeep) != 0);
    CHECK(f.analysis.analysisVersion() == version);
    // Running again (every health tick) starts nothing more meanwhile.
    f.analysis.setEngineStatus(khensu::PythonStatus::Running);
    CHECK(f.analysis.analysisRunToken() == run + 1);

    engine.release = true;
    REQUIRE(pumpUntil([&] { return f.analysis.currentAnalysis().has_python_analysis(); }));
    CHECK_FALSE(f.analysis.upgradeInFlight());
    CHECK_FALSE(f.analysis.analysisInFlight());
    CHECK(f.analysis.resultsGate() == Gate::Open);
    CHECK(f.analysis.currentAnalysis().inputs_failed == 0);
    CHECK(f.analysis.analysisVersion() > version);
    CHECK(broadcasts == 2);
    CHECK(f.finals == 2);
    CHECK(f.analysis.deepPassState() == khensu::DeepPassState::Complete);
    // Never hidden again: no second Analyzing on the way.
    CHECK(std::count(f.gates.begin(), f.gates.end(), Gate::Analyzing) == 1);
    // Nothing left unanswered: the next health tick re-runs nothing.
    f.analysis.setEngineStatus(khensu::PythonStatus::Running);
    CHECK(f.analysis.analysisRunToken() == run + 1);
    f.analysis.removeAnalysisListener(&key);
}

TEST_CASE("AnalysisService: with the engine up the results gate still waits for the main pipeline (FN2-2b)",
          "[split][analysis][fn2-2b]")
{
    Fixture f(/*pipeline*/ true);
    BootingEngine engine;
    engine.up = true;
    engine.release = false;
    f.analysis.setServiceCallsForTesting(engine.calls());
    f.analysis.setEngineStatus(khensu::PythonStatus::Running);
    SineFile track;
    f.sessions.adopt(track.session("s-warm"));
    f.analysis.beginAnalysis();
    REQUIRE(pumpUntil([&] { return engine.deepCalls.load() == 1; }));
    // The fast pass is in; the grade is withheld while the deep job runs.
    CHECK(f.analysis.resultsGate() == Gate::Analyzing);
    CHECK_FALSE(f.analysis.hasAnalysis());
    CHECK_FALSE(f.analysis.analysisModel().hasResult());
    engine.release = true;
    REQUIRE(pumpUntil([&] { return f.analysis.resultsGate() == Gate::Open; }));
    REQUIRE(f.analysis.hasAnalysis());
    CHECK(f.analysis.currentAnalysis().has_python_analysis());
    CHECK(f.analysis.currentAnalysis().inputs_failed == 0);
    CHECK(f.gates == std::vector<Gate>{Gate::Analyzing, Gate::Open});
}

TEST_CASE("AnalysisService: a session switch during the in-place deep pass publishes nothing stale (FN2-2b)",
          "[split][analysis][fn2-2b]")
{
    Fixture f(/*pipeline*/ true);
    BootingEngine engine;
    f.analysis.setServiceCallsForTesting(engine.calls());
    f.analysis.setEngineStatus(khensu::PythonStatus::Starting);
    SineFile track;
    f.sessions.adopt(track.session("s-old"));
    f.analysis.beginAnalysis();
    REQUIRE(pumpUntil([&] { return f.analysis.resultsGate() == Gate::Open && f.analysis.hasAnalysis(); }));

    engine.release = false;
    engine.up = true;
    f.analysis.setEngineStatus(khensu::PythonStatus::Running);
    REQUIRE(pumpUntil([&] { return engine.deepCalls.load() == 1; }));
    REQUIRE(f.analysis.upgradeInFlight());

    // The user opens another session while the old one's deep pass runs.
    f.analysis.resetForSession();
    f.openSession("s-new");
    const auto version = f.analysis.analysisVersion();
    const auto gatesBefore = f.gates.size();
    const int finalsBefore = f.finals;
    // The superseded run stops at its next cancellation check.
    REQUIRE(pumpUntil([&] { return engine.deepCancelled.load() == 1; }));
    engine.release = true;
    // Let anything it might still deliver come through the message loop.
    juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
    CHECK_FALSE(f.analysis.hasAnalysis());
    CHECK_FALSE(f.analysis.analysisModel().hasResult());
    CHECK(f.analysis.analysisVersion() == version);
    CHECK(f.finals == finalsBefore);
    CHECK(f.gates.size() == gatesBefore);
    CHECK_FALSE(f.analysis.upgradeInFlight());
    CHECK_FALSE(f.analysis.analysisInFlight());
    // And the recovery allowance belongs to the new session's runs.
    f.analysis.setEngineStatus(khensu::PythonStatus::Running);
    CHECK_FALSE(f.analysis.analysisInFlight());
}
