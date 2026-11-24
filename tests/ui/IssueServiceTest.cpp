// tests/ui/IssueServiceTest.cpp
// CON-1: the one issue queue. IssueService's states (open / done / dismissed,
// stored in the feedback store), the order (open by severity, settled last),
// the fix check's outcome per issue (FEAT-1), Fix and Fix top issue through
// the render service, and the queue panel's pure labels.
#include <catch2/catch_test_macros.hpp>
#include "analysis/FixVerification.h"
#include "session/IssueService.h"
#include "session/RenderHistory.h"
#include "session/SessionService.h"
#include "support/FakeAppController.h"
#include "support/NullServices.h"
#include "ui/shared/IssueQueuePanel.h"
#include <khensu/feedback/FeedbackManager.h>
#include <SQLiteCpp/SQLiteCpp.h>

using khensu::AnalysisResult;
using khensu::FeedbackManager;
using khensu::IIssues;
using khensu::IssueService;
using khensu::Recommendation;
namespace fv = khensu::fix_verification;

namespace {
Recommendation finding(const std::string& code, float severity, bool renderable = true)
{
    Recommendation r;
    r.id = code + "_f1";
    r.what = code;
    r.severity = severity;
    if (renderable) r.suggested_parameters["highpass_hz"] = 30.0f; // a renderable chain
    return r;
}

struct Hub final : khensu::test::NullAnalysisHub {
    AnalysisResult result;
    bool has = false;
    FeedbackManager* fm = nullptr;
    std::optional<fv::Result> check;
    std::map<void*, std::function<void(const AnalysisResult&)>> analysisListeners;
    std::map<void*, std::function<void()>> verificationListeners;
    int verifyCalls = 0;
    juce::String lastVerified;

    bool hasAnalysis() const noexcept override { return has; }
    const AnalysisResult& currentAnalysis() const noexcept override { return result; }
    FeedbackManager* getFeedbackManager() const noexcept override { return fm; }
    const fv::Result* currentVerification() const noexcept override { return check ? &*check : nullptr; }
    void addAnalysisListener(void* key, std::function<void(const AnalysisResult&)> cb) override
    {
        analysisListeners[key] = std::move(cb);
    }
    void removeAnalysisListener(void* key) override { analysisListeners.erase(key); }
    void addVerificationListener(void* key, std::function<void()> cb) override
    {
        verificationListeners[key] = std::move(cb);
    }
    void removeVerificationListener(void* key) override { verificationListeners.erase(key); }
    void verifyRender(const juce::String& path) override
    {
        ++verifyCalls;
        lastVerified = path;
    }

    void publish(AnalysisResult r)
    {
        result = std::move(r);
        has = true;
        for (auto& [k, cb] : analysisListeners)
            cb(result);
    }
};

struct Renders final : khensu::test::NullRenders {
    std::vector<std::string> fixed;
    juce::String current;
    bool fromOriginal = false; ///< "Start from original": the next fix processes the original,
                               ///< not the current version (renderInputPath, WF-1)
    bool hold = false;       ///< keep a fix "rendering" until finish()
    khensu::RenderHistory history;
    std::vector<std::pair<juce::String, Done>> rendering;
    std::map<void*, std::function<void()>> treeListeners;

    static juce::String pathOf(const std::string& recId) { return "D:/song_fix_" + juce::String(recId) + ".wav"; }
    void changed()
    {
        const auto listeners = treeListeners;
        for (auto& [k, cb] : listeners)
            cb();
    }
    /// Completes the oldest held render, the way RenderService does: the
    /// render joins the tree under its input, then the caller hears.
    void finish(bool ok = true)
    {
        REQUIRE_FALSE(rendering.empty());
        auto [path, onDone] = rendering.front();
        rendering.erase(rendering.begin());
        if (ok) {
            history.registerRenderFrom(path, fromOriginal ? juce::String() : current);
            current = path;
            changed();
        }
        if (onDone) onDone(ok, ok ? path : juce::String("Render cancelled."));
    }
    void applyRecommendationChain(const Recommendation& rec, Done onDone) override
    {
        fixed.push_back(rec.id);
        rendering.emplace_back(pathOf(rec.id), std::move(onDone));
        if (!hold) finish();
    }
    const std::vector<khensu::RenderHistory::TreeNode>& renderTree() const noexcept override { return history.tree(); }
    int currentRenderTreeNode() const noexcept override { return history.currentNode(); }
    const std::vector<juce::String>& getAppliedRenderHistory() const noexcept override { return history.applied(); }
    juce::String currentRenderPath() const override { return current; }
    void addRenderTreeListener(void* key, std::function<void()> cb) override { treeListeners[key] = std::move(cb); }
    void removeRenderTreeListener(void* key) override { treeListeners.erase(key); }
};

struct Fixture {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu-issues-" + juce::Uuid().toString());
    std::unique_ptr<FeedbackManager> fm;
    Hub hub;
    Renders renders;
    khensu::SessionService sessions;
    khensu::test::NullServiceLifecycle lifecycle;
    std::unique_ptr<IssueService> issues;
    int changes = 0;

    Fixture()
    {
        dir.createDirectory();
        fm = std::make_unique<FeedbackManager>(dir.getChildFile("khensu.db").getFullPathName().toStdString());
        hub.fm = fm.get();
        issues = std::make_unique<IssueService>(hub, renders, sessions, lifecycle);
        issues->attach();
        issues->addIssuesListener(this, [this] { ++changes; });
        AnalysisResult r;
        r.file_id = "f1";
        auto strength = finding("phase_clean", 0.0f, false);
        strength.type = khensu::RecommendationType::Strength;
        r.recommendations = {finding("too_bright", 50.0f), finding("true_peak_over", 85.0f),
                             finding("narrow_image", 20.0f, /*renderable*/ false), strength};
        hub.publish(r);
    }
    ~Fixture()
    {
        issues->removeIssuesListener(this);
        issues.reset();
        fm.reset();
        dir.deleteRecursively();
    }
};
} // namespace

TEST_CASE("IssueService lists open findings by severity, strengths excluded (CON-1)", "[con1]")
{
    Fixture f;
    const auto list = f.issues->list();
    REQUIRE(list.size() == 3);
    CHECK(list[0].code == "true_peak_over");
    CHECK(list[1].code == "too_bright");
    CHECK(list[2].code == "narrow_image");
    CHECK(list[0].renderable);
    CHECK_FALSE(list[2].renderable);
    CHECK(f.issues->openCount() == 3);
    CHECK(f.changes >= 1); // the publish
}

TEST_CASE("IssueService stores done, dismissed and reopened in the feedback store", "[con1]")
{
    Fixture f;
    f.issues->markDone("too_bright_f1");
    f.issues->dismiss("narrow_image_f1", "Intentional");
    CHECK(f.issues->issue("too_bright_f1")->state == IIssues::State::Done);
    CHECK(f.issues->issue("narrow_image_f1")->state == IIssues::State::Dismissed);
    CHECK(f.issues->openCount() == 1);
    // Settled issues go below the open one.
    CHECK(f.issues->list().front().code == "true_peak_over");

    // Survives a reload (a new analysis of the same file).
    f.hub.publish(f.hub.result);
    CHECK(f.issues->issue("narrow_image_f1")->state == IIssues::State::Dismissed);
    f.issues->waitForPendingWrites(); // FIX-6: writes land on the writer thread
    const auto rows = f.fm->getFeedbackBatch({"narrow_image_f1"});
    CHECK(rows.at("narrow_image_f1").note == "dismissed: Intentional");

    f.issues->reopen("narrow_image_f1");
    CHECK(f.issues->issue("narrow_image_f1")->state == IIssues::State::Open);
    CHECK(IssueService::stateOf({false, "reopened"}) == IIssues::State::Open);
    CHECK(IssueService::stateOf({true, ""}) == IIssues::State::Done);
    CHECK(IssueService::dismissNote({}) == "dismissed");
}

TEST_CASE("IssueService: Fix renders through the render service and marks the issue done", "[con1]")
{
    Fixture f;
    bool ok = false;
    f.issues->fix("too_bright_f1", [&](bool success, juce::String) { ok = success; });
    CHECK(ok);
    REQUIRE(f.renders.fixed == std::vector<std::string>{"too_bright_f1"});
    CHECK(f.issues->issue("too_bright_f1")->state == IIssues::State::Done);

    // Fix top issue: the most severe open, renderable one.
    CHECK(f.issues->fixTopIssue());
    CHECK(f.renders.fixed.back() == "true_peak_over_f1");
    // Only the advice-only finding is left open: nothing to render.
    CHECK_FALSE(f.issues->fixTopIssue());

    bool failed = true;
    f.issues->fix("gone_f1", [&](bool success, juce::String) { failed = !success; });
    CHECK(failed);

    f.issues->verify();
    CHECK(f.hub.verifyCalls == 1);
}

TEST_CASE("IssueService: a fix the current version is not built on is no longer Done (FIX-3)", "[con1][fix3]")
{
    Fixture f;
    f.issues->fix("too_bright_f1");
    REQUIRE(f.issues->issue("too_bright_f1")->state == IIssues::State::Done);

    // The next fix stacks on the first: both are in the audio.
    f.issues->fix("true_peak_over_f1");
    CHECK(f.issues->issue("too_bright_f1")->state == IIssues::State::Done);
    CHECK(f.issues->issue("true_peak_over_f1")->state == IIssues::State::Done);

    // "Start from original", then fix the peak again: the brightness fix is
    // not in the current version, so its issue is open again - and the
    // feedback store says so (taste learning reads it).
    f.renders.fromOriginal = true;
    f.issues->reopen("true_peak_over_f1");
    f.issues->fix("true_peak_over_f1");
    CHECK(f.issues->issue("too_bright_f1")->state == IIssues::State::Open);
    CHECK(f.issues->issue("true_peak_over_f1")->state == IIssues::State::Done);
    f.issues->waitForPendingWrites(); // FIX-6: writes land on the writer thread
    CHECK_FALSE(f.fm->getFeedbackBatch({"too_bright_f1"}).at("too_bright_f1").acted_on);

    // Stepping back to the stacked version brings it back.
    f.renders.history.undo();
    f.renders.current = f.renders.history.currentPath();
    f.renders.changed();
    CHECK(f.issues->issue("too_bright_f1")->state == IIssues::State::Done);
}

TEST_CASE("IssueService renders one fix at a time (FIX-4)", "[con1][fix4]")
{
    Fixture f;
    f.renders.hold = true;
    CHECK_FALSE(f.issues->fixInProgress());

    int firstDone = 0;
    f.issues->fix("true_peak_over_f1", [&](bool ok, juce::String) { firstDone += ok ? 1 : 0; });
    CHECK(f.issues->fixInProgress());

    // A second press - the same fix, another row, or F - renders nothing.
    bool refused = false;
    juce::String why;
    f.issues->fix("true_peak_over_f1", [&](bool ok, juce::String m) { refused = !ok; why = m; });
    CHECK(refused);
    CHECK(why.isNotEmpty());
    refused = false;
    f.issues->fix("too_bright_f1", [&](bool ok, juce::String) { refused = !ok; });
    CHECK(refused);
    refused = false;
    CHECK(f.issues->fixTopIssue([&](bool ok, juce::String) { refused = !ok; })); // handled: it said why
    CHECK(refused);
    CHECK(f.renders.fixed == std::vector<std::string>{"true_peak_over_f1"});
    CHECK(f.issues->issue("too_bright_f1")->state == IIssues::State::Open);

    f.renders.finish();
    CHECK(firstDone == 1);
    CHECK_FALSE(f.issues->fixInProgress());
    CHECK(f.issues->issue("true_peak_over_f1")->state == IIssues::State::Done);

    // A failed render frees the queue too, and records nothing.
    f.issues->fix("too_bright_f1");
    CHECK(f.issues->fixInProgress());
    f.renders.finish(false);
    CHECK_FALSE(f.issues->fixInProgress());
    CHECK(f.issues->issue("too_bright_f1")->state == IIssues::State::Open);
    f.renders.hold = false;
    f.issues->fix("too_bright_f1");
    CHECK(f.issues->issue("too_bright_f1")->state == IIssues::State::Done);
}

TEST_CASE("IssueService forgets the session it leaves (REF-2)", "[con1][ref2]")
{
    Fixture f;
    f.renders.hold = true;
    f.issues->fix("too_bright_f1");
    REQUIRE(f.issues->fixInProgress());
    const int before = f.changes;

    f.issues->resetForSession();
    CHECK_FALSE(f.issues->fixInProgress());
    CHECK(f.changes > before);

    // The old session's render finishing late must not mark anything done.
    f.renders.finish();
    f.issues->waitForPendingWrites(); // FIX-6: writes land on the writer thread
    const auto rows = f.fm->getFeedbackBatch({"too_bright_f1"});
    const bool recordedDone = rows.count("too_bright_f1") != 0 && rows.at("too_bright_f1").acted_on;
    CHECK_FALSE(recordedDone);
    // ...nor does a later change to the render tree touch its issue.
    f.renders.history.clear();
    f.renders.changed();
    CHECK(f.issues->issue("too_bright_f1")->state == IIssues::State::Open);
}

TEST_CASE("IssueService shows what the fix check found per issue (FEAT-1)", "[con1][feat1]")
{
    Fixture f;
    AnalysisResult after;
    after.file_id = "f1";
    after.recommendations = {finding("too_bright", 20.0f), finding("narrow_image", 20.0f, false),
                             finding("muddy", 40.0f)};
    f.hub.check = fv::compare(f.hub.result, after, "D:/song_fix.wav");
    for (auto& [k, cb] : f.hub.verificationListeners)
        cb();

    CHECK(f.issues->issue("true_peak_over_f1")->check == IIssues::Check::Resolved);
    CHECK(f.issues->issue("too_bright_f1")->check == IIssues::Check::Improved);
    CHECK(f.issues->issue("narrow_image_f1")->check == IIssues::Check::Unchanged);
    // Resolved counts as settled; the check's new finding joins the queue.
    CHECK(f.issues->openCount() == 3);
    const auto list = f.issues->list();
    REQUIRE(list.size() == 4);
    bool sawNew = false;
    for (const auto& i : list)
        if (i.code == "muddy") {
            sawNew = true;
            CHECK(i.check == IIssues::Check::New);
            CHECK(i.recId.empty());
        }
    CHECK(sawNew);
    CHECK(list.back().code == "true_peak_over"); // resolved: listed last
}

TEST_CASE("IssueService keeps an issue open when the check could not measure it (FIX-1)", "[con1][feat1][fix1]")
{
    namespace in = khensu::analysis_inputs;
    Fixture f;
    // The original had its deep pass; one of its findings needs it.
    AnalysisResult original = f.hub.result;
    original.python_analysis_ran = true;
    auto hook = finding("hook_density_low", 70.0f, false);
    hook.requires_inputs = in::kDeep;
    original.recommendations.push_back(hook);
    f.hub.publish(original);
    REQUIRE(f.issues->openCount() == 4);

    // The render was analysed while the engine was busy: fast pass only, and
    // the loudness fix worked.
    AnalysisResult after;
    after.file_id = "f1";
    after.recommendations = {finding("too_bright", 50.0f), finding("narrow_image", 20.0f, false)};
    f.hub.check = fv::compare(f.hub.result, after, "D:/song_fix.wav");
    for (auto& [k, cb] : f.hub.verificationListeners)
        cb();

    CHECK(f.issues->issue("true_peak_over_f1")->check == IIssues::Check::Resolved);
    const auto hookIssue = f.issues->issue("hook_density_low_f1");
    REQUIRE(hookIssue.has_value());
    CHECK(hookIssue->check == IIssues::Check::NotChecked);
    CHECK_FALSE(hookIssue->settled());
    CHECK(f.issues->openCount() == 3); // the hook finding still counts as open
    CHECK(khensu::IssueQueuePanel::stateLabel(*hookIssue).contains("check: Could not check"));

    // A finding only the checked version raised, on an input the original
    // lacked, joins the queue as open - but not as "new".
    AnalysisResult fastOriginal = f.hub.result;
    fastOriginal.python_analysis_ran = false;
    fastOriginal.recommendations.pop_back();
    f.hub.publish(fastOriginal);
    AnalysisResult deepAfter = after;
    deepAfter.python_analysis_ran = true;
    deepAfter.recommendations.push_back(hook);
    f.hub.check = fv::compare(f.hub.result, deepAfter, "D:/song_fix.wav");
    for (auto& [k, cb] : f.hub.verificationListeners)
        cb();
    bool saw = false;
    for (const auto& i : f.issues->list())
        if (i.code == "hook_density_low") {
            saw = true;
            CHECK(i.check == IIssues::Check::NotChecked);
            CHECK(i.recId.empty());
            CHECK_FALSE(i.settled());
        }
    CHECK(saw);
}

TEST_CASE("Issue queue panel labels", "[con1]")
{
    IIssues::Issue open;
    open.recId = "a_f1";
    open.renderable = true;
    CHECK(khensu::IssueQueuePanel::stateLabel(open) == "Open");
    open.check = IIssues::Check::Worse;
    CHECK(khensu::IssueQueuePanel::stateLabel(open) == "Open - check: Worse");
    IIssues::Issue advice;
    advice.recId = "b_f1";
    CHECK(khensu::IssueQueuePanel::stateLabel(advice).contains("no one-click fix"));
    IIssues::Issue done;
    done.recId = "c_f1";
    done.state = IIssues::State::Done;
    done.check = IIssues::Check::Resolved;
    CHECK(khensu::IssueQueuePanel::stateLabel(done) == "Done - check: Resolved");

    CHECK(khensu::IssueQueuePanel::headerSummary({}) == "No issues found");
    // A finding without "what" still gets a headline.
    IIssues::Issue bare;
    bare.code = "low_end_thin";
    CHECK(khensu::IssueQueuePanel::titleFor(bare) == "Low end thin");
    bare.rec.engineer_note = "The low end is thin.\nMore detail.";
    CHECK(khensu::IssueQueuePanel::titleFor(bare) == "The low end is thin.");
    CHECK(khensu::IssueQueuePanel::headerSummary({open, advice, done}) == "2 open issues, 1 done");
}

namespace {
struct CapturingLogger final : juce::Logger {
    juce::StringArray lines;
    juce::CriticalSection lock;
    void logMessage(const juce::String& message) override
    {
        const juce::ScopedLock sl(lock);
        lines.add(message);
    }
};
} // namespace

TEST_CASE("IssueService logs a feedback-store write that failed on the writer thread (FIX-6)", "[con1]")
{
    Fixture f;
    {
        // Break the store underneath: the next upsert fails inside SQLite.
        SQLite::Database db(f.dir.getChildFile("khensu.db").getFullPathName().toStdString(), SQLite::OPEN_READWRITE);
        db.exec("DROP TABLE recommendation_feedback");
    }
    CapturingLogger logger;
    juce::Logger::setCurrentLogger(&logger);
    f.issues->markDone("too_bright_f1");
    f.issues->waitForPendingWrites();
    juce::Logger::setCurrentLogger(nullptr);

    CHECK(logger.lines.joinIntoString(" | ").contains("could not store too_bright_f1"));
    // The queue still shows what the user chose.
    CHECK(f.issues->issue("too_bright_f1")->state == khensu::IIssues::State::Done);
}

namespace {
// Two issues the panel shows: sub-bass (20-60 Hz) and a broadband one.
struct TwoIssues final : khensu::test::FakeAppController {
    std::vector<IIssues::Issue> list() const override
    {
        IIssues::Issue sub;
        sub.recId = "r-sub";
        sub.code = "sub_bass_excess";
        sub.rec.id = "r-sub";
        sub.rec.what = "Sub-bass builds up";
        sub.rec.freq_range = khensu::FrequencyRange{20.0f, 60.0f};
        IIssues::Issue loud;
        loud.recId = "r-loud";
        loud.code = "loudness_low";
        loud.rec.id = "r-loud";
        loud.rec.what = "Quieter than the genre";
        return {sub, loud};
    }
};
} // namespace

TEST_CASE("Issue queue marks findings the stated monitoring cannot reproduce (NEW-10)", "[con1][new10]")
{
    juce::ScopedJuceInitialiser_GUI init;
    TwoIssues controller;
    khensu::IssueQueuePanel panel(controller);
    panel.setSize(900, 600);

    std::string monitoring = "nearfield";
    panel.setMonitoringSource([&monitoring] { return monitoring; });
    panel.refresh();
    auto notes = panel.rowNotesForTesting();
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].contains("Small studio monitors (5 inch)"));
    CHECK(notes[0].contains("50 Hz"));
    CHECK(notes[1].isEmpty()); // broadband: any monitoring can judge it

    monitoring = "headphones"; // reproduces 20-60 Hz
    panel.refresh();
    notes = panel.rowNotesForTesting();
    CHECK(notes[0].isEmpty());

    monitoring = ""; // not stated: never marked
    panel.refresh();
    CHECK(panel.rowNotesForTesting()[0].isEmpty());
}

#include "ui/shared/IssuePlaylist.h"

TEST_CASE("The issue playlist steps through timed findings in time order (FEAT-8)", "[con1][feat8]")
{
    using namespace khensu::issue_playlist;
    auto timed = [](const char* id, float start, float end, bool settled = false) {
        IIssues::Issue i;
        i.recId = id;
        i.rec.id = id;
        i.rec.time_range = khensu::TimeRange{start, end};
        if (settled) i.state = IIssues::State::Done;
        return i;
    };
    IIssues::Issue broadband;
    broadband.recId = "whole-song";
    const std::vector<IIssues::Issue> issues{timed("late", 90.0f, 95.0f), broadband, timed("early", 10.0f, 12.0f),
                                             timed("fixed", 30.0f, 31.0f, true)};
    const auto order = timedOrder(issues);
    REQUIRE(order.size() == 2); // the broadband and the settled one are not in it
    CHECK(issues[order[0]].recId == "early");
    CHECK(issues[order[1]].recId == "late");

    CHECK(step(-1, 2, true) == 0);
    CHECK(step(0, 2, true) == 1);
    CHECK(step(1, 2, true) == 0);  // wraps
    CHECK(step(-1, 2, false) == 1); // back from nothing: the last
    CHECK(step(0, 0, true) == -1);

    const auto [start, end] = region(khensu::TimeRange{10.0f, 10.5f});
    CHECK(start == 9.5);
    CHECK(end == 11.5); // at least two seconds
    CHECK(region(khensu::TimeRange{0.2f, 0.4f}).first == 0.0);
}

TEST_CASE("IssueService fixes at a chosen amount and reports it on the issue (OVH-6, UI-1)", "[con1][ovh6]")
{
    Fixture f;
    CHECK(f.issues->issue("too_bright_f1")->fixedAmount == 0.0f);
    bool ok = false;
    f.issues->fixScaled("too_bright_f1", 0.5f, [&](bool success, juce::String) { ok = success; });
    CHECK(ok);
    CHECK(f.issues->issue("too_bright_f1")->fixedAmount == 0.5f);
    CHECK(f.issues->issue("too_bright_f1")->state == IIssues::State::Done);

    // Nothing to render at 0 %: refused, nothing rendered.
    const auto before = f.renders.fixed.size();
    bool refused = false;
    f.issues->fixScaled("true_peak_over_f1", 0.0f, [&](bool success, juce::String) { refused = !success; });
    CHECK(refused);
    CHECK(f.renders.fixed.size() == before);
    CHECK_FALSE(f.issues->fixInProgress());
}

TEST_CASE("Issue rows carry one primary action (UI-2)", "[con1][ui2]")
{
    using khensu::IssueQueuePanel;
    IIssues::Issue open;
    open.recId = "r1";
    open.renderable = true;
    CHECK(IssueQueuePanel::primaryLabel(open, 1.0f, false) == "Fix");
    CHECK(IssueQueuePanel::primaryLabel(open, 0.6f, false) == "Fix 60%");
    CHECK(IssueQueuePanel::primaryLabel(open, 0.6f, true) == "Keep");
    IIssues::Issue advice = open;
    advice.renderable = false;
    CHECK(IssueQueuePanel::primaryLabel(advice, 1.0f, false).isEmpty());
    IIssues::Issue done = open;
    done.state = IIssues::State::Done;
    CHECK(IssueQueuePanel::primaryLabel(done, 1.0f, false) == "Reopen");
    CHECK(IssueQueuePanel::severityWord(80.0f) == "Severe");
    CHECK(IssueQueuePanel::severityWord(40.0f) == "Moderate");
    CHECK(IssueQueuePanel::severityWord(5.0f) == "Minor");
}

TEST_CASE("Selecting on the hero selects the row and back (UI-1)", "[con1][ui1]")
{
    juce::ScopedJuceInitialiser_GUI init;
    TwoIssues controller;
    khensu::IssueQueuePanel panel(controller);
    panel.setSize(1100, 800);
    panel.setMonitoringSource([] { return std::string(); });
    panel.refresh();
    // The top open issue is selected by default, and the hero follows.
    CHECK(panel.selectedKey() == "r-sub");
    CHECK(panel.heroForTesting().selected() == "r-sub");
    panel.select("r-loud", true);
    CHECK(panel.heroForTesting().selected() == "r-loud");
    // The hero's click handler selects the row.
    REQUIRE(panel.heroForTesting().onFindingClicked);
    panel.heroForTesting().onFindingClicked("r-sub");
    CHECK(panel.selectedKey() == "r-sub");
    // Selection survives a rebuild.
    panel.refresh();
    CHECK(panel.selectedKey() == "r-sub");
    CHECK(panel.amountFor("r-sub") == 1.0f);
}
