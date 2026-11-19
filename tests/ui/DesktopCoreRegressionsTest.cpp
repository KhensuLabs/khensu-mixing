// tests/ui/DesktopCoreRegressionsTest.cpp
// Regression tests for the 2026-11-06 audit, desktop core slice (§10, DSK-*):
// the session-reset fan-out, auto-scan re-run queueing, the intake queue
// snapshot, the stems HUD stage, the A/B preview matching and preference
// learning, ABX statistics, the snapshot combo, settings read-modify-write,
// the derived-data cache and the HPSS stem provenance.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "ui/shared/SessionScoped.h"
#include "ui/Dashboard/AutoScanCompletion.h"
#include "ui/SessionIntake/IntakeQueueState.h"
#include "ui/shared/IntakeProgressHud.h"
#include "ui/shared/AbxStatistics.h"
#include "ui/shared/KhensuComparisonStrip.h"
#include "ui/shared/LiveLufsMeterComponent.h"
#include "session/PlaybackSourceController.h"
#include "session/AnalysisResultModel.h"
#include "session/StemSeparationInfo.h"
#include "settings/OnlineSettings.h"
#include "AbPreferenceMath.h"
#include <limits>
#include <thread>
#include <vector>

using namespace khensu;
using Catch::Approx;

// ---- S4 session-reset fan-out ----------------------------------------------

namespace {
struct Counter final : SessionScoped {
    int resets = 0;
    void resetForSession() override { ++resets; }
};
struct FakeRegistrar {
    SessionResetRegistry registry;
    void addSessionScoped(SessionScoped* s) { registry.add(s); }
    void removeSessionScoped(SessionScoped* s) { registry.remove(s); }
};
} // namespace

TEST_CASE("SessionResetRegistry: every registered object resets once per session change", "[dsk][session]")
{
    FakeRegistrar reg;
    Counter a, b;
    {
        ScopedSessionRegistrationT<FakeRegistrar> ra(reg, a);
        ScopedSessionRegistrationT<FakeRegistrar> rb(reg, b);
        reg.registry.add(&a); // duplicate registration is ignored
        CHECK(reg.registry.size() == 2);
        reg.registry.resetAll();
        CHECK(a.resets == 1);
        CHECK(b.resets == 1);
    }
    CHECK(reg.registry.size() == 0); // RAII deregistration
    reg.registry.resetAll();
    CHECK(a.resets == 1);
}

TEST_CASE("SessionResetRegistry: an object removed during the fan-out is skipped", "[dsk][session]")
{
    SessionResetRegistry registry;
    Counter victim;
    struct Remover final : SessionScoped {
        SessionResetRegistry* r = nullptr;
        SessionScoped* target = nullptr;
        void resetForSession() override { r->remove(target); }
    } remover;
    remover.r = &registry;
    remover.target = &victim;
    registry.add(&remover);
    registry.add(&victim);
    registry.resetAll();
    CHECK(victim.resets == 0);
}

// ---- DSK-03 auto-scan re-run queueing --------------------------------------

TEST_CASE("AutoScanCompletion::request queues a re-run behind an in-flight scan", "[dsk][resultsgate]")
{
    AutoScanCompletion scan;
    std::vector<std::function<void()>> deferred;
    scan.setDeferrer([&](std::function<void()> f) { deferred.push_back(std::move(f)); });

    bool running = true; // e.g. the previous session's scan is still in flight
    int starts = 0, done = 0;
    auto start = [&] {
        ++starts;
        running = true;
    };
    auto isRunning = [&] { return running; };

    CHECK(scan.request([&] { ++done; }, start, isRunning)); // counted by the gate
    CHECK(starts == 0);
    CHECK(scan.hasQueuedRerun());

    running = false; // the old scan's result arrives (and is dropped by its epoch check)
    scan.finish();
    REQUIRE(deferred.size() == 1);
    deferred.front()();
    CHECK(starts == 1); // the queued re-run started for the new session
    CHECK(done == 0);
    running = false;
    scan.finish(); // ...and its completion is what the gate waits for
    CHECK(done == 1);
}

TEST_CASE("AutoScanCompletion::reset drops a queued re-run (session change)", "[dsk][resultsgate]")
{
    AutoScanCompletion scan;
    std::vector<std::function<void()>> deferred;
    scan.setDeferrer([&](std::function<void()> f) { deferred.push_back(std::move(f)); });
    bool running = true;
    int starts = 0;
    scan.request([] {}, [&] { ++starts; }, [&] { return running; });
    scan.reset();
    running = false;
    scan.finish();
    CHECK(deferred.empty());
    CHECK(starts == 0);
}

TEST_CASE("AutoScanCompletion::request starts at once when idle", "[dsk][resultsgate]")
{
    AutoScanCompletion scan;
    bool running = false;
    int done = 0;
    CHECK(scan.request([&] { ++done; }, [&] { running = true; }, [&] { return running; }));
    running = false;
    scan.finish();
    CHECK(done == 1);
}

// ---- DSK-01 intake queue snapshot ------------------------------------------

namespace {
IntakeQueueState::Entry entry(const char* p)
{
    IntakeQueueState::Entry e;
    e.path = p;
    return e;
}
} // namespace

TEST_CASE("IntakeQueueState: the worker follows reorders and cancels made meanwhile", "[dsk][intake]")
{
    IntakeQueueState q;
    const int gen = q.reset({entry("a"), entry("b"), entry("c")});
    auto first = q.takeNext(gen);
    REQUIRE(first.has_value());
    CHECK(first->path == "a");
    q.reorder({"a", "c", "b"}); // the user dragged c above b while a was analysed
    q.cancel("b");
    auto second = q.takeNext(gen);
    REQUIRE(second.has_value());
    CHECK(second->path == "c");
    CHECK_FALSE(q.takeNext(gen).has_value());
}

TEST_CASE("IntakeQueueState: a new drop stops the previous worker and cancels its files", "[dsk][intake]")
{
    IntakeQueueState q;
    const int oldGen = q.reset({entry("a"), entry("b")});
    auto running = q.takeNext(oldGen);
    REQUIRE(running.has_value());
    const int newGen = q.reset({entry("x")});
    CHECK(running->cancelled->load()); // its in-flight job is cancelled
    CHECK_FALSE(q.isCurrent(oldGen));
    CHECK_FALSE(q.takeNext(oldGen).has_value());
    CHECK(q.takeNext(newGen)->path == "x");
}

TEST_CASE("IntakeQueueState: concurrent message-thread edits and worker reads are safe", "[dsk][intake]")
{
    IntakeQueueState q;
    std::vector<IntakeQueueState::Entry> files;
    for (int i = 0; i < 200; ++i)
        files.push_back(entry(juce::String("f" + juce::String(i)).toRawUTF8()));
    const int gen = q.reset(files);
    std::thread worker([&] {
        while (q.takeNext(gen).has_value()) {
        }
    });
    for (int i = 0; i < 200; ++i) {
        q.reorder({"f" + juce::String(199 - i)});
        q.cancel("f" + juce::String(i * 7 % 200));
    }
    worker.join();
    CHECK(q.takeNext(gen) == std::nullopt);
}

// ---- DSK-13 stems stage in the intake HUD ----------------------------------

TEST_CASE("IntakeProgressHud: a running stems stage (6 of 6) is not 'INTAKE DONE'", "[dsk][hud]")
{
    IntakeProgressHud hud;
    hud.updateStage("fast", 1, 6);
    hud.updateStage("chords", 5, 6);
    hud.markAnalysisFinished();
    CHECK(hud.showsDone());
    hud.updateStage("stems:running:0.25", 6, 6); // Demucs still running
    CHECK_FALSE(hud.showsDone());
    CHECK(hud.overallFraction() < 1.0f);
    hud.updateStage("stems:completed:1.0", 6, 6);
    CHECK(hud.showsDone());
    CHECK(IntakeProgressHud::isTerminalStemStage("stems:cancelled:0"));
    CHECK_FALSE(IntakeProgressHud::isTerminalStemStage("stems:running:0.5"));
}

// ---- DSK-14 level-matched previews -----------------------------------------

TEST_CASE("PlaybackSourceController: a device preview is muted until measured, then matched to A", "[dsk][ab]")
{
    PlaybackSourceController p;
    p.userVolume = 0.5f;
    p.previewPath = "phone.wav";
    PlaybackSourceController::GainContext ctx;
    ctx.originalLufs = -14.0f;
    ctx.originalPath = "mix.wav";
    ctx.loadedPath = "phone.wav";
    CHECK(p.gainFor(ctx) == 0.0f); // muted until its loudness is known
    p.previewLufs = -8.0f;         // 6 dB louder than the mix
    CHECK(p.gainFor(ctx) == Approx(0.5f * std::pow(10.0f, -6.0f / 20.0f)).epsilon(1e-4));
    p.previewLufs = -20.0f; // quieter: lifted only within the volume headroom
    CHECK(p.gainFor(ctx) == Approx(0.5f * std::pow(10.0f, 6.0f / 20.0f)).epsilon(1e-4)); // +6 dB lift
    p.previewLufs = -40.0f;                // far quieter: the lift stops at unity
    CHECK(p.gainFor(ctx) == Approx(1.0f));
    p.resetForSession();
    CHECK(p.previewPath.isEmpty());
}

// ---- DSK-07 preference learning --------------------------------------------

TEST_CASE("A/B preference: a matched pick never teaches loudness; a blind reference teaches nothing", "[dsk][ab]")
{
    const auto own = decideAbPreferenceLearning(/*hasProcessedB*/ true, /*blind*/ false);
    CHECK_FALSE(own.learnLufs);
    CHECK(own.recordProcessingPreference);
    const auto blind = decideAbPreferenceLearning(true, true);
    CHECK_FALSE(blind.learnLufs);
    CHECK_FALSE(blind.recordProcessingPreference);
    float pref = 0.0f;
    for (int i = 0; i < 20; ++i)
        pref = nudgeProcessingPreference(pref, true);
    CHECK(pref > 0.9f);
    CHECK(pref <= 1.0f);
    CHECK(nudgeProcessingPreference(0.0f, false) < 0.0f);
}

// ---- §10.3 ABX statistics --------------------------------------------------

TEST_CASE("ABX statistics: binomial p-values and the trials needed", "[dsk][abx]")
{
    CHECK(abx_stats::binomialPValue(0, 0) == 1.0);
    CHECK(abx_stats::binomialPValue(5, 5) == Approx(1.0 / 32.0));
    CHECK(abx_stats::binomialPValue(0, 10) == Approx(1.0));
    CHECK(abx_stats::binomialPValue(9, 10) == Approx(11.0 / 1024.0));
    CHECK(abx_stats::correctNeededForSignificance(4) == -1); // 4/4 = 0.0625
    CHECK(abx_stats::correctNeededForSignificance(10) == 9);
    CHECK(abx_stats::minimumTrialsForSignificance() == 5);
    CHECK(abx_stats::describeScore(3, 3).contains("trials needed"));
    CHECK(abx_stats::describeScore(9, 10).contains("very unlikely"));
}

// ---- DSK-12 snapshot combo -------------------------------------------------

TEST_CASE("KhensuComparisonStrip: the snapshot combo is rebuilt only on change and follows B", "[dsk][ab]")
{
    KhensuComparisonStrip strip;
    const std::vector<juce::String> history{"a_render.wav", "b_render.wav"};
    for (int i = 0; i < 25; ++i) // one second of the transport's 40 ms timer
        strip.setSnapshots(history, "a_render.wav");
    CHECK(strip.snapshotRebuildCount() == 1);
    CHECK(strip.selectedSnapshotId() == 2); // "a_render.wav", not snapped back to "Current"
    strip.setSnapshots(history, {});
    CHECK(strip.selectedSnapshotId() == 1);
    strip.setSnapshots({"a_render.wav", "b_render.wav", "c_render.wav"}, "c_render.wav");
    CHECK(strip.snapshotRebuildCount() == 2);
    CHECK(strip.selectedSnapshotId() == 4);
}

// ---- DSK-15 live meter -----------------------------------------------------

TEST_CASE("LiveLufsMeterComponent: silent readings are gated out of the distribution", "[dsk][meter]")
{
    CHECK(LiveLufsMeterComponent::isGatedReading(-96.0f));
    CHECK(LiveLufsMeterComponent::isGatedReading(std::numeric_limits<float>::quiet_NaN()));
    CHECK_FALSE(LiveLufsMeterComponent::isGatedReading(-14.0f));
}

// ---- DSK-11 settings read-modify-write -------------------------------------

TEST_CASE("OnlineSettings::update keeps fields another writer changed meanwhile", "[dsk][settings]")
{
    const auto dir = juce::File::createTempFile("khensu_settings_rmw");
    dir.createDirectory();
    {
        OnlineSettings initial;
        initial.save(dir);
    }
    const OnlineSettings screenCopy = OnlineSettings::load(dir); // loaded at construction

    // Meanwhile the scheduled backup records its timestamp.
    OnlineSettings::update(dir, [](OnlineSettings& s) { s.lastScheduledBackupIso = "2026-11-06T10:00:00Z"; });

    // The Settings screen saves only what it owns.
    bool ok = false;
    const auto saved = OnlineSettings::update(
        dir, [&](OnlineSettings& s) { s.updateCheck = !screenCopy.updateCheck; }, &ok);
    CHECK(ok);
    const auto reread = OnlineSettings::load(dir);
    CHECK(reread.lastScheduledBackupIso == "2026-11-06T10:00:00Z");
    CHECK(reread.updateCheck == !screenCopy.updateCheck);
    CHECK(saved.lastScheduledBackupIso == reread.lastScheduledBackupIso);
    dir.deleteRecursively();
}

// ---- §10.3 derived data once per result ------------------------------------

TEST_CASE("AnalysisResultModel::derived computes once per result and key", "[dsk][results]")
{
    AnalysisResultModel model;
    AnalysisResult r;
    r.loudness.lufs_integrated = -12.0f;
    model.setResult(r);
    int computes = 0;
    auto compute = [&](const AnalysisResult& res) {
        ++computes;
        return res.loudness.lufs_integrated;
    };
    CHECK(model.derived<float>("k", compute) == Approx(-12.0f));
    CHECK(model.derived<float>("k", compute) == Approx(-12.0f)); // second consumer: cache hit
    CHECK(computes == 1);
    model.derived<float>("other-weights", compute);
    CHECK(computes == 2);
    model.setResult(r); // a new result invalidates
    model.derived<float>("other-weights", compute);
    CHECK(computes == 3);
    model.clear();
    CHECK_FALSE(model.hasResult());
}

// ---- AID-07 stem provenance ------------------------------------------------

TEST_CASE("StemSeparationInfo: HPSS stems are approximate and carry the real reason", "[dsk][stems]")
{
    const juce::String hpssJson("{\"method\": \"hpss\", \"reason\": \"model download blocked (offline)\", \"model\": \"\"}");
    const auto hpss = StemSeparationInfo::fromJson(hpssJson);
    CHECK(hpss.known());
    CHECK(hpss.isApproximate());
    CHECK(hpss.describe().contains("model download blocked"));
    const juce::String demucsJson("{\"method\": \"demucs\", \"model\": \"htdemucs\"}");
    const auto demucs = StemSeparationInfo::fromJson(demucsJson);
    CHECK_FALSE(demucs.isApproximate());
    CHECK(demucs.describe().contains("htdemucs"));
    CHECK_FALSE(StemSeparationInfo::fromJson("not json").known());

    const auto dir = juce::File::createTempFile("khensu_stems_info");
    dir.createDirectory();
    CHECK_FALSE(StemSeparationInfo::readFrom(dir).known());
    dir.getChildFile(StemSeparationInfo::kFileName).replaceWithText("{\"method\":\"hpss\",\"reason\":\"r\"}");
    CHECK(StemSeparationInfo::readFrom(dir).isApproximate());
    dir.deleteRecursively();
}
