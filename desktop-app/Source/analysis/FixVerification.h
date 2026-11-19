// desktop-app/Source/analysis/FixVerification.h
// FEAT-1 "Did it work?": compares the analysis of the original with the
// analysis of an accepted render and says, per finding, whether it was
// resolved, improved, unchanged, got worse or is new - with the per-metric
// deltas behind it. Pure and header-only (unit-tested in
// tests/ui/FixVerificationTest.cpp); AnalysisService runs the analyses.
// THREAD_CONTEXT(AnyThread) - plain data and functions.
#pragma once
#include <cstdint>
#include <khensu/analysis/AnalysisInputs.h>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/recommendations/Recommendation.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace khensu::fix_verification {

/// NotChecked (FIX-1): one of the two analyses lacked the input the finding is
/// derived from (the deep pass, the drum scan, ...), so its absence says
/// nothing. It is last: the reader order below is the enum order.
enum class Outcome { Resolved, Improved, Unchanged, Worse, New, NotChecked };

struct FindingCheck {
    std::string code;  ///< issueCodeOf(rec, file id): the same issue in both analyses
    std::string title; ///< the finding's plain-English "what" (the newer one when both exist)
    Outcome outcome = Outcome::Unchanged;
    float severityBefore = 0.0f; ///< 0 when the finding is new
    float severityAfter = 0.0f;  ///< 0 when it was resolved
    bool onlyAfter = false;      ///< raised by the checked version only (new, or not comparable)
    /// The checked version's own recommendation when onlyAfter: its advice and
    /// explanation, so the queue can show them before an analysis adopts it.
    Recommendation recAfter;
};

struct MetricDelta {
    std::string name; ///< "Loudness", "True peak", ...
    std::string unit; ///< "LUFS", "dBTP", ...
    float before = 0.0f;
    float after = 0.0f;
    float delta() const noexcept { return after - before; }
};

struct Result {
    std::string renderPath; ///< the version that was checked
    std::vector<FindingCheck> findings;
    std::vector<MetricDelta> metrics;
    /// The render's AnalysisResult::inputs_failed: what its analysis asked the
    /// engine for and did not get. Non-zero means checking again once the
    /// engine is back can turn "could not check" into an answer.
    std::uint32_t inputsFailed = 0;

    int count(Outcome o) const noexcept
    {
        int n = 0;
        for (const auto& f : findings)
            if (f.outcome == o) ++n;
        return n;
    }
    /// The check for issue `code`, or nullptr when neither analysis raised it.
    const FindingCheck* find(const std::string& code) const noexcept
    {
        for (const auto& f : findings)
            if (f.code == code) return &f;
        return nullptr;
    }
};

/// Severity points (of 100) a finding must move to count as better or worse:
/// re-measuring the same audio moves a severity by a few points, which is
/// noise, not a change.
inline constexpr float kSeverityTolerance = 5.0f;

inline const char* outcomeLabel(Outcome o) noexcept
{
    switch (o) {
    case Outcome::Resolved:
        return "Resolved";
    case Outcome::Improved:
        return "Improved";
    case Outcome::Unchanged:
        return "Unchanged";
    case Outcome::Worse:
        return "Worse";
    case Outcome::New:
        return "New";
    case Outcome::NotChecked:
        return "Could not check";
    }
    return "";
}

/// Findings keyed by issue code; strengths are not findings.
inline std::map<std::string, const Recommendation*> findingsByCode(const AnalysisResult& r)
{
    std::map<std::string, const Recommendation*> out;
    for (const auto& rec : r.recommendations) {
        if (rec.type == RecommendationType::Strength) continue;
        const std::string code = issueCodeOf(rec, r.file_id);
        auto it = out.find(code);
        // Two findings with one code (a per-section repeat): keep the worst.
        if (it == out.end() || rec.severity > it->second->severity) out[code] = &rec;
    }
    return out;
}

inline Outcome classify(float before, float after) noexcept
{
    if (after < before - kSeverityTolerance) return Outcome::Improved;
    if (after > before + kSeverityTolerance) return Outcome::Worse;
    return Outcome::Unchanged;
}

/// True when `analysis` had every optional input `rec` was derived from, so
/// "this analysis did not raise it" means "measured and fine" (FIX-1).
inline bool couldMeasure(const AnalysisResult& analysis, const Recommendation& rec) noexcept
{
    return (rec.requires_inputs & ~analysis.inputs_available()) == 0;
}

/// `before`: the analysis the findings were raised on (the original);
/// `after`: the analysis of the accepted render. Findings come out in the
/// order a reader wants them: resolved, improved, unchanged, worse, new,
/// then the ones that could not be checked.
///
/// A finding one side did not raise is only Resolved / New when that side
/// had the inputs to raise it. A render checked while the engine was busy
/// has no deep pass, and every deep finding would otherwise read "Resolved".
inline Result compare(const AnalysisResult& before, const AnalysisResult& after, const std::string& renderPath = {})
{
    Result out;
    out.renderPath = renderPath;
    out.inputsFailed = after.inputs_failed;
    const auto a = findingsByCode(before);
    const auto b = findingsByCode(after);
    for (const auto& [code, recA] : a) {
        FindingCheck c;
        c.code = code;
        c.severityBefore = recA->severity;
        if (auto it = b.find(code); it != b.end()) {
            c.title = it->second->what;
            c.severityAfter = it->second->severity;
            c.outcome = classify(c.severityBefore, c.severityAfter);
        } else {
            c.title = recA->what;
            c.outcome = couldMeasure(after, *recA) ? Outcome::Resolved : Outcome::NotChecked;
        }
        out.findings.push_back(std::move(c));
    }
    for (const auto& [code, recB] : b) {
        if (a.count(code) != 0) continue;
        FindingCheck c;
        c.code = code;
        c.title = recB->what;
        c.recAfter = *recB;
        c.severityAfter = recB->severity;
        c.onlyAfter = true;
        c.outcome = couldMeasure(before, *recB) ? Outcome::New : Outcome::NotChecked;
        out.findings.push_back(std::move(c));
    }
    std::stable_sort(out.findings.begin(), out.findings.end(), [](const FindingCheck& x, const FindingCheck& y) {
        return static_cast<int>(x.outcome) < static_cast<int>(y.outcome);
    });

    auto metric = [&](const char* name, const char* unit, float va, float vb) {
        if (std::isfinite(va) && std::isfinite(vb) && va > -98.0f && vb > -98.0f)
            out.metrics.push_back({name, unit, va, vb});
    };
    metric("Loudness", "LUFS", before.loudness.lufs_integrated, after.loudness.lufs_integrated);
    metric("True peak", "dBTP", before.loudness.true_peak_dbtp, after.loudness.true_peak_dbtp);
    metric("Loudness range", "LU", before.loudness.lra, after.loudness.lra);
    metric("Crest factor", "dB", before.dynamics.crest_factor_db, after.dynamics.crest_factor_db);
    metric("Tonal tilt", "dB/oct", before.spectral.tilt_db_per_octave, after.spectral.tilt_db_per_octave);
    metric("Stereo width", "", before.stereo.width, after.stereo.width);
    metric("Correlation", "", before.stereo.correlation, after.stereo.correlation);
    return out;
}

/// One line for a status label: "3 resolved, 1 improved, 2 unchanged, 1 new,
/// 2 could not check".
inline std::string summary(const Result& r)
{
    std::string s;
    for (Outcome o : {Outcome::Resolved, Outcome::Improved, Outcome::Unchanged, Outcome::Worse, Outcome::New,
                      Outcome::NotChecked}) {
        const int n = r.count(o);
        if (n == 0) continue;
        if (!s.empty()) s += ", ";
        std::string label = outcomeLabel(o);
        label[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(label[0])));
        s += std::to_string(n) + " " + label;
    }
    return s.empty() ? std::string("No findings either way") : s;
}

} // namespace khensu::fix_verification
