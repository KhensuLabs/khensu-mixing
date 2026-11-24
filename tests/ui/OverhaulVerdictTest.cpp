// tests/ui/OverhaulVerdictTest.cpp
// OVH-1 / UI-3 / UI-7 / UI-10 decision logic: one readiness grade table, the
// verdict's counts and "Start here" (the issue Fix top issue fixes), one
// answer to "is the engine there" (the lifecycle's), the Dashboard's
// deep-pass wording by reason, the re-run once the engine is Running,
// skipped stages shown as skipped, the motion tween and the neutral rollup.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <functional>
#include "IBackgroundWork.h"
#include "analysis/ServiceReadyWait.h"
#include "session/AnalysisService.h"
#include "session/SessionService.h"
#include "ui/Dashboard/VerdictModel.h"
#include "ui/shared/IntakeProgressHud.h"
#include "ui/shared/SessionStateStrip.h"
#include "ui/shared/PipelineProgressBar.h"
#include "ui/shared/ServiceStatusText.h"
#include "ui/shared/ValueAnimator.h"
#include <khensu/analysis/AnalysisInputs.h>

using namespace khensu;

namespace {
Recommendation rec(const char* id, float severity, RecommendationType type = RecommendationType::Technical)
{
    Recommendation r;
    r.id = id;
    r.what = std::string("finding ") + id;
    r.severity = severity;
    r.type = type;
    return r;
}

IIssues::Issue issue(const char* id, float severity, bool renderable, IIssues::State state = IIssues::State::Open)
{
    IIssues::Issue i;
    i.recId = id;
    i.rec = rec(id, severity);
    i.renderable = renderable;
    i.state = state;
    return i;
}

struct CountingWork final : IBackgroundWork {
    void runInBackground(std::function<void()>) override {}
    void runLongInBackground(std::function<void()>) override {}
    uint64_t sessionEpoch() const noexcept override { return 0; }
    bool isShuttingDown() const noexcept override { return false; }
};
} // namespace

TEST_CASE("Readiness has one grade table for the headline and every category (OVH-1)", "[ovh1][verdict]")
{
    CHECK(verdict::gradeOf(100.0f) == verdict::Grade::Good);
    CHECK(verdict::gradeOf(80.0f) == verdict::Grade::Good);
    CHECK(verdict::gradeOf(79.9f) == verdict::Grade::Fair);
    CHECK(verdict::gradeOf(50.0f) == verdict::Grade::Fair);
    CHECK(verdict::gradeOf(49.9f) == verdict::Grade::Poor);
    CHECK(verdict::colourFor(90.0f) == KhensuTheme::accentGreen);
    CHECK(verdict::colourFor(60.0f) == KhensuTheme::accentAmber);
    CHECK(verdict::colourFor(10.0f) == KhensuTheme::accentRed);
}

TEST_CASE("The verdict counts the list the score was built from (OVH-1)", "[ovh1][verdict]")
{
    const std::vector<Recommendation> recs{rec("a", 80.0f), rec("b", 66.0f), rec("c", 40.0f), rec("d", 10.0f),
                                           rec("e", 0.0f, RecommendationType::Strength)};
    const auto c = verdict::countFindings(recs);
    CHECK(c.severe == 2);
    CHECK(c.moderate == 1);
    CHECK(c.minor == 1);
    CHECK(c.strengths == 1);
    CHECK(c.issues() == 4);
}

TEST_CASE("Start here names the issue Fix top issue fixes (OVH-1)", "[ovh1][verdict]")
{
    // The queue lists the most severe first; the first one cannot be rendered,
    // so Fix top issue fixes the second - and that is what the card names.
    const std::vector<IIssues::Issue> queue{issue("stereo-width", 90.0f, /*renderable*/ false),
                                            issue("dc-offset", 60.0f, /*renderable*/ true),
                                            issue("lra", 20.0f, true)};
    const auto start = verdict::startHere(queue);
    CHECK(start.recId == "dc-offset");
    CHECK(start.fixable);
    CHECK(start.text == "finding dc-offset");

    // Settled issues are skipped.
    const std::vector<IIssues::Issue> settled{issue("dc-offset", 60.0f, true, IIssues::State::Done),
                                              issue("lra", 20.0f, true)};
    CHECK(verdict::startHere(settled).recId == "lra");

    // Nothing renderable: name the top open issue, claim no fix.
    const std::vector<IIssues::Issue> noFix{issue("stereo-width", 90.0f, false)};
    const auto review = verdict::startHere(noFix);
    CHECK(review.recId == "stereo-width");
    CHECK_FALSE(review.fixable);

    CHECK(verdict::startHere({}).text.isEmpty());
}

TEST_CASE("The deep pass asks the lifecycle, and probes only without one (OVH-1)", "[ovh1][service-wait]")
{
    int probes = 0;
    const std::function<bool()> deadProbe = [&] {
        ++probes;
        return false;
    };
    service_ready::EngineGate gate;
    // Not judged yet: the probe answers.
    CHECK_FALSE(service_ready::engineAvailable(&gate, deadProbe));
    CHECK(probes == 1);
    // Running - even when a quick probe would miss a busy engine.
    gate.publish(service_ready::EngineGate::State::Running);
    CHECK(service_ready::engineAvailable(&gate, deadProbe));
    CHECK(probes == 1);
    gate.publish(service_ready::EngineGate::State::Starting);
    CHECK_FALSE(service_ready::engineAvailable(&gate, [] { return true; }));
    gate.publish(service_ready::EngineGate::State::Down);
    CHECK_FALSE(service_ready::engineAvailable(&gate, [] { return true; }));
    // No lifecycle (tests, headless tools): the probe.
    CHECK(service_ready::engineAvailable(nullptr, [] { return true; }));
}

TEST_CASE("The Dashboard words a fast-only result by its real reason (OVH-1)", "[ovh1][status]")
{
    using S = DeepPassState;
    const juce::String off = service_status::deepPassLine(S::Off);
    const juce::String skipped = service_status::deepPassLine(S::SkippedByUser);
    const juce::String failed = service_status::deepPassLine(S::Failed);
    const juce::String down = service_status::deepPassLine(S::EngineUnavailable);
    CHECK(off.contains("off"));
    CHECK(skipped.contains("skipped"));
    CHECK(failed.contains("failed"));
    CHECK(down.contains("engine is ready")); // FN2-2b: a fast check, deeper checks to come
    CHECK_FALSE(off.contains("engine"));
    CHECK_FALSE(failed.contains("not available"));
    CHECK(service_status::deepPassLine(S::Complete) == "Analysis complete.");
    CHECK_FALSE(service_status::deepPassOffersRerun(S::Off));
    CHECK_FALSE(service_status::deepPassOffersRerun(S::Complete));
    CHECK(service_status::deepPassOffersRerun(S::EngineUnavailable));
    CHECK(service_status::deepPassOffersRerun(S::Failed));
}

TEST_CASE("AnalysisService: the engine's state is the lifecycle's, and a skip re-runs once it is Running (OVH-1)",
          "[ovh1][analysis]")
{
    CountingWork work;
    SessionService sessions;
    AnalysisService analysis{work, sessions};
    analysis.skipPipelineForTesting();
    analysis.attachFeedbackManager(nullptr);
    Session session;
    session.id = "s";
    session.name = "t";
    session.target.run_deep_analysis = true;
    sessions.adopt(std::move(session));

    // The gate the deep pass reads follows the lifecycle's status.
    analysis.setEngineStatus(PythonStatus::Starting);
    CHECK(analysis.engineGate()->state() == service_ready::EngineGate::State::Starting);

    // A run whose engine inputs went unanswered: fast only, worded by reason.
    analysis.beginAnalysis();
    analysis.deliverStageForTesting("deep_analysis:skipped", 2, 6);
    AnalysisResult r;
    r.python_analysis_ran = false;
    r.inputs_failed = analysis_inputs::serviceInputs(false);
    analysis.deliverResultForTesting(r);
    CHECK(analysis.deepPassState() == DeepPassState::EngineUnavailable);
    const int run = analysis.analysisRunToken();

    // Still starting: nothing re-runs.
    analysis.setEngineStatus(PythonStatus::Starting);
    CHECK(analysis.analysisRunToken() == run);
    // Running - with no unhealthy -> healthy edge needed: re-run, in place
    // (FN2-2b) - the fast result stays on screen meanwhile.
    analysis.setEngineStatus(PythonStatus::Running);
    CHECK(analysis.engineGate()->state() == service_ready::EngineGate::State::Running);
    CHECK(analysis.analysisRunToken() == run + 1);
    CHECK(analysis.resultsGate() == AnalysisService::ResultsGate::Open);
    CHECK(analysis.upgradeInFlight());

    // The re-run is unanswered again: kept on screen, no loop.
    analysis.deliverResultForTesting(r);
    analysis.setEngineStatus(PythonStatus::Running);
    CHECK(analysis.analysisRunToken() == run + 1);

    // A deep job the engine answered with a failure is "failed", not "unavailable".
    analysis.beginAnalysis();
    analysis.deliverStageForTesting("deep_analysis:failed", 2, 6);
    AnalysisResult failed;
    failed.python_analysis_ran = false;
    analysis.deliverResultForTesting(failed);
    CHECK(analysis.deepPassState() == DeepPassState::Failed);
}

TEST_CASE("AnalysisService: deep analysis switched off reads as off (OVH-1)", "[ovh1][analysis]")
{
    CountingWork work;
    SessionService sessions;
    AnalysisService analysis{work, sessions};
    analysis.skipPipelineForTesting();
    analysis.attachFeedbackManager(nullptr);
    Session session;
    session.id = "s";
    session.target.run_deep_analysis = false;
    sessions.adopt(std::move(session));
    analysis.beginAnalysis();
    AnalysisResult r;
    r.python_analysis_ran = false;
    analysis.deliverResultForTesting(r);
    CHECK(analysis.deepPassState() == DeepPassState::Off);
}

TEST_CASE("AnalysisService: the saved and shown readiness use one weighting choice (OVH-1)", "[ovh1][analysis]")
{
    CountingWork work;
    SessionService sessions;
    AnalysisService analysis{work, sessions};
    CHECK(analysis.scoreWeightProfile().empty());
    analysis.setScoreWeightProfile("edm");
    CHECK(analysis.scoreWeightProfile() == "edm");
}

TEST_CASE("Progress strips show a skipped deep pass as skipped, not done (OVH-1)", "[ovh1][hud]")
{
    IntakeProgressHud hud;
    hud.updateStage("fast", 1, 6);
    hud.updateStage("deep_analysis:skipped", 2, 6);
    hud.markAnalysisFinished();
    CHECK(hud.showsDone());
    CHECK(hud.slotState(0) == IntakeProgressHud::Slot::Done);
    for (int i = 1; i <= 4; ++i)
        CHECK(hud.slotState(i) == IntakeProgressHud::Slot::Skipped);

    // A new run starts clean.
    hud.updateStage("fast", 1, 6);
    CHECK(hud.slotState(1) == IntakeProgressHud::Slot::Pending);

    PipelineProgressBar bar;
    bar.updateStage("fast", 1, 6);
    bar.updateStage("deep_analysis:skipped", 2, 6);
    CHECK(bar.slotState(0) == PipelineProgressBar::SlotState::Done);
    for (int i = 1; i <= 4; ++i)
        CHECK(bar.slotState(i) == PipelineProgressBar::SlotState::Skipped);
    bar.updateStage("stems:completed", 6, 6);
    CHECK(bar.slotState(2) == PipelineProgressBar::SlotState::Skipped); // not overwritten as done
}

TEST_CASE("The intake strip is one calm line unless Engineer Detail is on (UI-10)", "[ui10][hud]")
{
    IntakeProgressHud hud;
    hud.setDetailed(false);
    CHECK(hud.preferredHeight() == IntakeProgressHud::kCompactHeight);
    hud.updateStage("fast", 1, 6);
    CHECK(hud.statusWord() == "Analyzing");
    hud.setDetailed(true);
    CHECK(hud.preferredHeight() == IntakeProgressHud::kHeight);
}

TEST_CASE("Motion: a tween eases from the old value to the new one over its duration (UI-7)", "[ui7][motion]")
{
    motion::Tween t{10.0f, 50.0f, 1000.0, 200};
    CHECK(t.valueAt(1000.0) == Catch::Approx(10.0f));
    CHECK(t.valueAt(1200.0) == Catch::Approx(50.0f));
    CHECK(t.valueAt(5000.0) == Catch::Approx(50.0f));
    const float mid = t.valueAt(1100.0);
    CHECK(mid > 30.0f); // ease-out: past halfway at half time
    CHECK(mid < 50.0f);
    CHECK(t.finishedAt(1200.0));
    CHECK_FALSE(t.finishedAt(1199.0));
    CHECK(motion::easeOutCubic(0.0f) == 0.0f);
    CHECK(motion::easeOutCubic(1.0f) == 1.0f);
    CHECK(motion::easeOutCubic(2.0f) == 1.0f);
}

TEST_CASE("ValueAnimator jumps to its first value and eases later ones (UI-7)", "[ui7][motion]")
{
    int frames = 0;
    ValueAnimator a{[&] { ++frames; }};
    a.setTarget(42.0f);
    CHECK(a.value() == 42.0f); // nothing to move from
    CHECK_FALSE(a.isAnimating());
    a.setTarget(80.0f);
    CHECK(a.target() == 80.0f);
    CHECK(a.isAnimating());
    a.setTarget(10.0f, /*animate*/ false);
    CHECK(a.value() == 10.0f);
    CHECK_FALSE(a.isAnimating());
}

TEST_CASE("The top strip is session state, never a measurement (VIZ-P1)", "[ui3][rollup]")
{
    SessionStateStrip::State st;
    auto figures = SessionStateStrip::figuresFor(st);
    for (const auto& f : figures)
        CHECK(f.value.isEmpty()); // no session: every figure reads "-"

    st.hasSession = true;
    st.analysis = SessionStateStrip::State::Analysis::Ready;
    figures = SessionStateStrip::figuresFor(st);
    CHECK(figures[SessionStateStrip::kVersion].value == "Original");
    CHECK(figures[SessionStateStrip::kIssues].value == "Nothing open");
    CHECK(figures[SessionStateStrip::kFixCheck].value.isEmpty()); // no check yet
    for (const auto& f : figures) {
        CHECK_FALSE(f.value.contains("LUFS")); // measurements live in the tiles / transport
        CHECK_FALSE(f.label.containsIgnoreCase("crest"));
        CHECK_FALSE(f.label.containsIgnoreCase("correlation"));
    }

    st.severe = 1;
    st.checked = true;
    st.worse = 1;
    figures = SessionStateStrip::figuresFor(st);
    CHECK(figures[SessionStateStrip::kIssues].tone == SessionStateStrip::Tone::Severe);
    CHECK(figures[SessionStateStrip::kFixCheck].value == "1 worse");
    CHECK(figures[SessionStateStrip::kFixCheck].tone == SessionStateStrip::Tone::Moderate);
}
