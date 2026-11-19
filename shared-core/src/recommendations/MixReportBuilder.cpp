// shared-core/src/recommendations/MixReportBuilder.cpp
#include <khensu/recommendations/MixReportBuilder.h>
#include <khensu/recommendations/DynamicsRecommender.h>
#include <khensu/recommendations/IssueCodes.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string_view>

namespace khensu {

// ---------------------------------------------------------------------------
// Conflict matrix - static table of known conflicting recommendation code pairs.
// IDs are in the format "<code>_<file_id>"; we match by stripping the file_id suffix.
// ---------------------------------------------------------------------------
struct CodePair {
    std::string_view a;
    std::string_view b;
    std::string_view reason;
};

static constexpr CodePair kConflictMatrix[] = {
    // Mutually exclusive loudness direction
    {"lufs_too_loud", "lufs_too_quiet", "Contradictory loudness direction"},
    // Mutually exclusive spectral direction
    {"centroid_too_bright", "centroid_too_dark", "Contradictory spectral balance direction"},
    // Mutually exclusive low-end direction
    {"low_end_heavy", "low_end_light", "Contradictory low-end balance direction"},
    // Mutually exclusive dynamics direction
    {"lra_too_compressed", "lra_too_dynamic", "Contradictory LRA direction"},
    {"over_compressed", "under_compressed", "Contradictory compression direction"},
    // Stereo direction
    {"stereo_too_wide", "stereo_too_narrow", "Contradictory stereo width direction"},
    // Compression advice affects LRA - addressing one influences the other
    {"lra_too_compressed", "over_compressed",
     "Reducing compression (for lra_too_compressed) and reducing compression "
     "(for over_compressed) are compatible, but fixing over_compressed first may resolve "
     "lra_too_compressed - address in chain_position order"},
    // Wide stereo + mono compat warning
    {"stereo_too_narrow", "phase_issue",
     "Widening stereo (stereo_too_narrow) may worsen existing phase issues - fix phase first"},
};

// Extract the recommendation code from an ID like "code_name_file_id" - the
// one code table (ENG-7, IssueCodes.h), longest match first.
static std::string_view extractCode(std::string_view id) noexcept
{
    return recommendations::issueCodePrefix(id);
}

// ---------------------------------------------------------------------------
// Mastering readiness score (0-100)
//
// Scoring thresholds (exact):
//   Headroom    : true_peak >= -0.5 dBTP → penalty = min(25, (tp + 0.5) * 25); else 0
//   Crest factor: score = min(20, max(0, (crest - 6) * 2)); penalty = 20 - score
//   Low-end     : delta = abs(balance_low - genre.low_end_weight);
//                 penalty = min(20, delta * 50)   [proportional; no threshold]
//   Stereo corr : correlation < 0.5 → penalty = min(15, (0.5 - corr) * 30); else 0
//   Crit. mix-stage recs (MixChannel|MixBus, severity>=60, not taste): 5 pts each, max 20
//   Final score : clamp(100 - sum_of_penalties, 0, 100)
// ---------------------------------------------------------------------------
static float computeReadiness(const std::vector<Recommendation>& recs, const AnalysisResult& result,
                              const GenreEntry& genre, const ScoreWeightProfile& weights,
                              std::vector<ReadinessPenaltyTerm>* breakdownOut = nullptr) noexcept
{
    float penalty = 0.0f;

    // (1) True peak / headroom
    const float tp = result.loudness.true_peak_dbtp;
    float headroomPenalty = 0.0f;
    if (tp >= -0.5f) headroomPenalty = std::min(weights.readiness_headroom_cap, (tp + 0.5f) * 25.0f);
    penalty += headroomPenalty;

    // (2) Crest factor
    // Measured from the genre's over-compression floor, not a fixed 6 dB
    // (AUDIT-070), so an on-target hyperpop master is not penalised.
    const float crest_score = std::min(
        weights.readiness_crest_cap,
        std::max(0.0f, (result.dynamics.crest_factor_db - DynamicsRecommender::overCompressedFloorDb(genre)) * 2.0f));
    const float crestPenalty = weights.readiness_crest_cap - crest_score;
    penalty += crestPenalty;

    // (3) Low-end balance deviation
    // Uses spectral.balance_low vs genre.low_end_weight (0-1 proportion);
    // 5 pts per 0.10 deviation from target (delta * 50).
    // Note: brief references lufs_sub/lufs_mid which are not in the data model;
    // balance_low deviation is the closest available proxy.
    const float low_delta = std::abs(result.spectral.balance_low - genre.low_end_weight);
    const float lowEndPenalty = std::min(weights.readiness_low_end_cap, low_delta * 50.0f);
    penalty += lowEndPenalty;

    // (4) Stereo correlation
    const float corr = result.stereo.correlation;
    float stereoPenalty = 0.0f;
    if (corr < 0.5f) stereoPenalty = std::min(weights.readiness_stereo_cap, (0.5f - corr) * 30.0f);
    penalty += stereoPenalty;

    // Critical mix-stage recs (MixChannel|MixBus, severity>=60, not taste): 5 pts each, capped
    int crit_count = 0;
    for (const auto& r : recs)
        if ((r.stage == ProcessingStage::MixChannel || r.stage == ProcessingStage::MixBus) && r.severity >= 60.0f &&
            !r.is_taste_decision)
            ++crit_count;
    const float criticalRecsPenalty =
        std::min(weights.readiness_critical_recs_cap, static_cast<float>(crit_count) * 5.0f);
    penalty += criticalRecsPenalty;

    // (6) Streaming loudness delta (CORE-10)
    // Spotify, YouTube, Tidal, Amazon Music: -14 LUFS. Apple Music: -16 LUFS.
    // The acceptable window is the streaming band widened by the genre's own
    // LUFS window: an EDM master sitting on its -8 LUFS target is turned down
    // by the platforms, not penalised as "not ready" (it used to lose the
    // whole cap while another card praised it as on target). Only distance
    // OUTSIDE that window costs points. A silent/unmeasured file's sentinel
    // is not a level.
    float streamingPenalty = 0.0f;
    const float lufs = result.loudness.lufs_integrated;
    if (std::isfinite(lufs) && lufs > -70.0f) {
        const float windowLo = std::min(-16.0f, genre.lufs_range_min);
        const float windowHi = std::max(-14.0f, genre.lufs_range_max);
        const float outside = lufs < windowLo ? windowLo - lufs : (lufs > windowHi ? lufs - windowHi : 0.0f);
        if (outside > 0.0f) streamingPenalty = std::min(weights.readiness_streaming_cap, outside * 2.5f);
    }
    penalty += streamingPenalty;

    if (breakdownOut != nullptr) {
        *breakdownOut = {
            {"Headroom", headroomPenalty},
            {"Crest Factor", crestPenalty},
            {"Low-End Balance", lowEndPenalty},
            {"Stereo Correlation", stereoPenalty},
            {"Critical Issues", criticalRecsPenalty},
            {"Streaming Delta", streamingPenalty},
        };
    }

    return std::clamp(100.0f - penalty, 0.0f, 100.0f);
}

// ---------------------------------------------------------------------------
// no_action_needed evidence strings
// ---------------------------------------------------------------------------
static void appendNoActionEvidence(std::vector<std::string>& out, const AnalysisResult& result, const GenreEntry& genre)
{
    auto fmt = [](float v, const char* unit) -> std::string {
        std::ostringstream ss;
        ss.precision(1);
        ss << std::fixed << v << " " << unit;
        return ss.str();
    };

    // Integrated LUFS within genre range
    if (result.loudness.lufs_integrated >= genre.lufs_range_min &&
        result.loudness.lufs_integrated <= genre.lufs_range_max) {
        out.push_back("Integrated LUFS: " + fmt(result.loudness.lufs_integrated, "LUFS") + " - within " + genre.id +
                      " range [" + fmt(genre.lufs_range_min, "") + ", " +
                      fmt(genre.lufs_range_max, "LUFS] - no action needed."));
    }

    // LRA within genre range
    if (result.loudness.lra_measured && result.loudness.lra >= genre.lra_range_min &&
        result.loudness.lra <= genre.lra_range_max) {
        out.push_back("LRA: " + fmt(result.loudness.lra, "LU") + " - within " + genre.id + " range [" +
                      fmt(genre.lra_range_min, "") + ", " + fmt(genre.lra_range_max, "LU] - no action needed."));
    }

    // True peak at or below ceiling
    if (result.loudness.true_peak_dbtp <= genre.true_peak_max) {
        out.push_back("True Peak: " + fmt(result.loudness.true_peak_dbtp, "dBTP") + " - at or below ceiling of " +
                      fmt(genre.true_peak_max, "dBTP - no action needed."));
    }

    // Healthy crest factor
    if (const float crestFloor = DynamicsRecommender::overCompressedFloorDb(genre);
        result.dynamics.crest_factor_db >= crestFloor) {
        out.push_back("Crest Factor: " + fmt(result.dynamics.crest_factor_db, "dB") +
                      " - healthy transient headroom for " + genre.id + " (>=" + fmt(crestFloor, "dB).") );
    }

    // Stereo correlation
    if (result.stereo.correlation >= 0.5f) {
        out.push_back("Stereo Correlation: " + fmt(result.stereo.correlation, "") +
                      " - good mono compatibility (>=0.5) - no action needed.");
    }

    // Low-end balance
    const float low_delta = std::abs(result.spectral.balance_low - genre.low_end_weight);
    if (low_delta <= 0.15f) {
        out.push_back("Low-end balance: " + fmt(result.spectral.balance_low, "") + " - within 0.15 of " + genre.id +
                      " target (" + fmt(genre.low_end_weight, ") - no action needed."));
    }
}

// ---------------------------------------------------------------------------
// buildMixReport - public API
// ---------------------------------------------------------------------------
MixReport buildMixReport(std::vector<Recommendation> recommendations, const AnalysisResult& result,
                         const GenreEntry& genre, const CreativeTarget& target, const ScoreWeightProfile& weights)
{
    MixReport report;
    report.file_id = result.file_id;
    report.genre_id = genre.id;

    // (1) Confidence threshold filtering
    if (target.confidence_threshold > 0.0f) {
        recommendations.erase(
            std::remove_if(recommendations.begin(), recommendations.end(),
                           [&](const Recommendation& r) { return r.confidence < target.confidence_threshold; }),
            recommendations.end());
    }

    // (2) Contradiction detection - populate conflicts and conflicts_with on both sides
    for (std::size_t i = 0; i < recommendations.size(); ++i) {
        for (std::size_t j = i + 1; j < recommendations.size(); ++j) {
            auto ca = extractCode(recommendations[i].id);
            auto cb = extractCode(recommendations[j].id);
            for (const auto& pair : kConflictMatrix) {
                if ((ca == pair.a && cb == pair.b) || (ca == pair.b && cb == pair.a)) {
                    ConflictPair cp;
                    cp.id_a = recommendations[i].id;
                    cp.id_b = recommendations[j].id;
                    cp.reason = std::string(pair.reason);
                    report.conflicts.push_back(cp);
                    recommendations[i].conflicts_with.push_back(recommendations[j].id);
                    recommendations[j].conflicts_with.push_back(recommendations[i].id);
                    break;
                }
            }
        }
    }

    // (3) Sort: severity DESC, then chain_position ASC (lower = fix first)
    std::stable_sort(recommendations.begin(), recommendations.end(),
                     [](const Recommendation& a, const Recommendation& b) {
                         if (a.severity != b.severity) return a.severity > b.severity;
                         return a.chain_position < b.chain_position;
                     });

    report.recommendations = std::move(recommendations);

    // (4) Mastering readiness score + label
    report.mastering_readiness_score =
        computeReadiness(report.recommendations, result, genre, weights, &report.penalty_breakdown);
    const float s = report.mastering_readiness_score;
    if (s >= 85.0f)
        report.readiness_label = MixReport::kLabelReady;
    else if (s >= 70.0f)
        report.readiness_label = MixReport::kLabelNearReady;
    else if (s >= 50.0f)
        report.readiness_label = MixReport::kLabelMixFixes;
    else
        report.readiness_label = MixReport::kLabelMajorIssues;

    // (5) no_action_needed evidence
    appendNoActionEvidence(report.no_action_needed, result, genre);

    return report;
}

} // namespace khensu
