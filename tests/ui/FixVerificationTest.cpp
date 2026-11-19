// tests/ui/FixVerificationTest.cpp
// FEAT-1 "Did it work?": the comparison of the original's analysis with an
// accepted render's (fix_verification::compare) and the AnalysisService flow
// that runs it - checked per render, reused on undo/redo, cleared by a new
// baseline or a session change.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "IBackgroundWork.h"
#include "analysis/FixVerification.h"
#include "session/AnalysisService.h"
#include "session/SessionService.h"

using khensu::AnalysisResult;
using khensu::Recommendation;
using khensu::RecommendationType;
namespace fv = khensu::fix_verification;

namespace {
Recommendation finding(const std::string& code, float severity, const std::string& fileId = "f1")
{
    Recommendation r;
    r.id = code + "_" + fileId;
    r.what = code + " detected";
    r.severity = severity;
    r.type = RecommendationType::Technical;
    return r;
}

AnalysisResult resultWith(std::vector<Recommendation> recs, float lufs = -10.0f)
{
    AnalysisResult r;
    r.file_id = "f1";
    r.loudness.lufs_integrated = lufs;
    r.loudness.true_peak_dbtp = -0.2f;
    r.recommendations = std::move(recs);
    return r;
}
} // namespace

TEST_CASE("FixVerification: each finding is resolved, improved, unchanged, worse or new (FEAT-1)", "[feat1]")
{
    auto strength = finding("phase_clean", 0.0f);
    strength.type = RecommendationType::Strength;
    const auto before = resultWith({finding("true_peak_over", 80.0f), finding("too_bright", 60.0f),
                                    finding("narrow_image", 40.0f), finding("muddy_low_mids", 30.0f), strength},
                                   -8.0f);
    const auto after = resultWith({finding("too_bright", 35.0f),     // improved (25 points)
                                   finding("narrow_image", 42.0f),   // within tolerance: unchanged
                                   finding("muddy_low_mids", 50.0f), // worse
                                   finding("over_compressed", 55.0f)},
                                  -10.5f);

    const auto r = fv::compare(before, after, "D:/song_fix.wav");
    CHECK(r.renderPath == "D:/song_fix.wav");
    CHECK(r.count(fv::Outcome::Resolved) == 1);
    CHECK(r.count(fv::Outcome::Improved) == 1);
    CHECK(r.count(fv::Outcome::Unchanged) == 1);
    CHECK(r.count(fv::Outcome::Worse) == 1);
    CHECK(r.count(fv::Outcome::New) == 1);
    CHECK(r.find("phase_clean") == nullptr); // strengths are not findings

    REQUIRE(r.find("true_peak_over") != nullptr);
    CHECK(r.find("true_peak_over")->outcome == fv::Outcome::Resolved);
    CHECK(r.find("true_peak_over")->severityAfter == 0.0f);
    REQUIRE(r.find("over_compressed") != nullptr);
    CHECK(r.find("over_compressed")->severityBefore == 0.0f);

    // Reader order: resolved first, new last.
    CHECK(r.findings.front().outcome == fv::Outcome::Resolved);
    CHECK(r.findings.back().outcome == fv::Outcome::New);

    // Metric deltas, skipping unmeasured (-99) values.
    REQUIRE_FALSE(r.metrics.empty());
    CHECK(r.metrics.front().name == "Loudness");
    CHECK_THAT(r.metrics.front().delta(), Catch::Matchers::WithinAbs(-2.5, 1e-4));
    CHECK(fv::summary(r) == "1 resolved, 1 improved, 1 unchanged, 1 worse, 1 new");
}

TEST_CASE("FixVerification: a finding the re-analysis could not measure is not resolved (FIX-1)", "[feat1][fix1]")
{
    namespace in = khensu::analysis_inputs;
    auto hook = finding("hook_density_low", 70.0f);
    hook.requires_inputs = in::kDeep;
    auto kick = finding("kick_bass_collision", 65.0f);
    kick.requires_inputs = in::kDrums;
    auto before = resultWith({finding("true_peak_over", 80.0f), hook, kick});
    before.python_analysis_ran = true;
    before.drums = AnalysisResult::DrumData{};

    // The fix was checked while the engine was busy: the fast pass only.
    const auto fastOnly = resultWith({});
    REQUIRE(fastOnly.inputs_available() == in::kNone);
    const auto r = fv::compare(before, fastOnly, "D:/song_fix.wav");
    REQUIRE(r.find("true_peak_over") != nullptr);
    CHECK(r.find("true_peak_over")->outcome == fv::Outcome::Resolved); // needs the fast pass only
    REQUIRE(r.find("hook_density_low") != nullptr);
    CHECK(r.find("hook_density_low")->outcome == fv::Outcome::NotChecked);
    REQUIRE(r.find("kick_bass_collision") != nullptr);
    CHECK(r.find("kick_bass_collision")->outcome == fv::Outcome::NotChecked);
    CHECK(r.count(fv::Outcome::Resolved) == 1);
    CHECK(fv::summary(r) == "1 resolved, 2 could not check");
    CHECK(r.findings.back().outcome == fv::Outcome::NotChecked); // after everything that was measured

    // Deep pass back, drum scan failed: only the drum finding is unknown.
    auto deepOnly = resultWith({});
    deepOnly.python_analysis_ran = true;
    const auto r2 = fv::compare(before, deepOnly);
    CHECK(r2.find("hook_density_low")->outcome == fv::Outcome::Resolved);
    CHECK(r2.find("kick_bass_collision")->outcome == fv::Outcome::NotChecked);

    // Both inputs measured and the findings gone: resolved.
    auto full = deepOnly;
    full.drums = AnalysisResult::DrumData{};
    const auto r3 = fv::compare(before, full);
    CHECK(r3.count(fv::Outcome::Resolved) == 3);
    CHECK(r3.count(fv::Outcome::NotChecked) == 0);

    // A finding still raised is compared as before, whatever else is missing.
    auto stillThere = resultWith({hook});
    stillThere.python_analysis_ran = true;
    CHECK(fv::compare(before, stillThere).find("hook_density_low")->outcome == fv::Outcome::Unchanged);
}

TEST_CASE("FixVerification: a finding the original could not measure is not called new (FIX-1)", "[feat1][fix1]")
{
    namespace in = khensu::analysis_inputs;
    // The original was analysed without the deep pass; the render with it.
    const auto before = resultWith({finding("too_bright", 60.0f)});
    auto hook = finding("hook_density_low", 70.0f);
    hook.requires_inputs = in::kDeep;
    auto after = resultWith({finding("too_bright", 60.0f), hook, finding("over_compressed", 55.0f)});
    after.python_analysis_ran = true;

    const auto r = fv::compare(before, after);
    REQUIRE(r.find("hook_density_low") != nullptr);
    CHECK(r.find("hook_density_low")->outcome == fv::Outcome::NotChecked);
    CHECK(r.find("hook_density_low")->onlyAfter);
    CHECK(r.find("hook_density_low")->severityAfter == 70.0f);
    CHECK(r.find("over_compressed")->outcome == fv::Outcome::New); // fast-pass finding: really new
    CHECK(r.find("over_compressed")->onlyAfter);
    CHECK_FALSE(r.find("too_bright")->onlyAfter);
}

TEST_CASE("FixVerification: the same audio checks as unchanged; nothing either way says so", "[feat1]")
{
    const auto a = resultWith({finding("too_bright", 60.0f)});
    const auto same = fv::compare(a, a);
    CHECK(same.count(fv::Outcome::Unchanged) == 1);
    CHECK(fv::summary(fv::compare(resultWith({}), resultWith({}))) == "No findings either way");

    // A finding raised twice (per section) is judged by its worst instance.
    const auto twice = resultWith({finding("too_bright", 20.0f), finding("too_bright", 70.0f)});
    const auto fixed = resultWith({finding("too_bright", 40.0f)});
    CHECK(fv::compare(twice, fixed).find("too_bright")->outcome == fv::Outcome::Improved);
}

namespace {
struct NoWork final : khensu::IBackgroundWork {
    void runInBackground(std::function<void()>) override {}
    void runLongInBackground(std::function<void()>) override {}
    uint64_t sessionEpoch() const noexcept override { return 0; }
    bool isShuttingDown() const noexcept override { return false; }
};
} // namespace

TEST_CASE("AnalysisService checks each accepted render once and clears on a new baseline (FEAT-1)", "[feat1]")
{
    NoWork work;
    khensu::SessionService sessions;
    khensu::AnalysisService analysis{work, sessions};
    analysis.attachFeedbackManager(nullptr);
    int notifications = 0;
    int key = 0;
    analysis.addVerificationListener(&key, [&] { ++notifications; });

    khensu::Session session;
    session.id = "s1";
    session.name = "t";
    khensu::AudioFileRef ref;
    ref.id = "f1";
    ref.path = "Z:/does-not-exist/original.wav";
    session.files.push_back(ref);
    sessions.adopt(std::move(session));

    // Nothing to compare with before the original is analysed.
    analysis.verifyRender("Z:/does-not-exist/fix1.wav");
    CHECK_FALSE(analysis.verificationRunning());
    CHECK(analysis.currentVerification() == nullptr);

    analysis.deliverResultForTesting(resultWith({finding("true_peak_over", 80.0f), finding("too_bright", 60.0f)}));
    REQUIRE(analysis.hasAnalysis());

    analysis.verifyRender("Z:/does-not-exist/fix1.wav");
    CHECK(analysis.verificationRunning());
    CHECK(analysis.currentVerification() == nullptr);
    analysis.deliverVerificationForTesting(resultWith({finding("too_bright", 60.0f)}));
    CHECK_FALSE(analysis.verificationRunning());
    REQUIRE(analysis.currentVerification() != nullptr);
    CHECK(analysis.currentVerification()->find("true_peak_over")->outcome == fv::Outcome::Resolved);
    CHECK(analysis.currentVerification()->renderPath == "Z:/does-not-exist/fix1.wav");

    // Back to the original: no check. Redo to the render: the stored check, no re-run.
    analysis.verifyRender({});
    CHECK(analysis.currentVerification() == nullptr);
    analysis.verifyRender("Z:/does-not-exist/fix1.wav");
    CHECK_FALSE(analysis.verificationRunning());
    REQUIRE(analysis.currentVerification() != nullptr);

    // A partial result of the render is not a check.
    analysis.verifyRender("Z:/does-not-exist/fix2.wav");
    auto partial = resultWith({});
    partial.is_partial = true;
    analysis.deliverVerificationForTesting(partial);
    CHECK(analysis.verificationRunning());
    analysis.deliverVerificationForTesting(resultWith({}));
    CHECK(fv::summary(*analysis.currentVerification()) == "2 resolved");

    // A new analysis of the original is a new baseline: earlier checks go.
    analysis.deliverResultForTesting(resultWith({finding("too_bright", 60.0f)}));
    CHECK(analysis.currentVerification() == nullptr);
    CHECK(notifications > 0);

    analysis.resetForSession();
    CHECK(analysis.currentVerification() == nullptr);
    CHECK_FALSE(analysis.verificationRunning());
    analysis.removeVerificationListener(&key);
}

TEST_CASE("AnalysisService re-checks a fix once the engine is back when the check went unanswered (FIX-1)",
          "[feat1][fix1][fix2]")
{
    namespace in = khensu::analysis_inputs;
    NoWork work;
    khensu::SessionService sessions;
    khensu::AnalysisService analysis{work, sessions};
    analysis.attachFeedbackManager(nullptr);
    juce::StringArray statuses;
    analysis.onVerificationStatus = [&](const juce::String& text, bool) { statuses.add(text); };

    khensu::Session session;
    session.id = "s1";
    session.name = "t";
    khensu::AudioFileRef ref;
    ref.id = "f1";
    ref.path = "Z:/does-not-exist/original.wav";
    session.files.push_back(ref);
    sessions.adopt(std::move(session));

    // The original had its deep pass and raised a finding that needs it.
    auto hook = finding("hook_weak", 70.0f);
    hook.requires_inputs = in::kDeep;
    auto before = resultWith({hook, finding("too_bright", 60.0f)});
    before.python_analysis_ran = true;
    analysis.deliverResultForTesting(before);
    const int run = analysis.analysisRunToken();

    // The render was analysed while the engine was busy: no deep pass.
    analysis.verifyRender("Z:/does-not-exist/fix1.wav");
    auto unanswered = resultWith({});
    unanswered.inputs_failed = in::serviceInputs(false);
    analysis.deliverVerificationForTesting(unanswered);
    REQUIRE(analysis.currentVerification() != nullptr);
    CHECK(analysis.currentVerification()->find("hook_weak")->outcome == fv::Outcome::NotChecked);
    CHECK(analysis.currentVerification()->find("too_bright")->outcome == fv::Outcome::Resolved);
    CHECK(analysis.currentVerification()->inputsFailed == in::serviceInputs(false));
    CHECK(statuses[statuses.size() - 1].contains("1 could not check"));

    // The engine is back: the check runs again, the original is left alone.
    analysis.reanalyzeIfServiceRecovered();
    CHECK(analysis.analysisRunToken() == run);
    CHECK(analysis.verificationRunning());
    auto answered = resultWith({});
    answered.python_analysis_ran = true;
    analysis.deliverVerificationForTesting(answered);
    REQUIRE(analysis.currentVerification() != nullptr);
    CHECK(analysis.currentVerification()->find("hook_weak")->outcome == fv::Outcome::Resolved);
    CHECK(analysis.currentVerification()->inputsFailed == 0u);

    // A complete check is not repeated.
    analysis.reanalyzeIfServiceRecovered();
    CHECK_FALSE(analysis.verificationRunning());

    // Unanswered twice in a row is not retried a second time by itself.
    analysis.verifyRender("Z:/does-not-exist/fix2.wav");
    analysis.deliverVerificationForTesting(unanswered);
    analysis.reanalyzeIfServiceRecovered();
    REQUIRE(analysis.verificationRunning());
    analysis.deliverVerificationForTesting(unanswered);
    analysis.reanalyzeIfServiceRecovered();
    CHECK_FALSE(analysis.verificationRunning());
}
