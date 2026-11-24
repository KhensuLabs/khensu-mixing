#include <khensu/recommendations/MixScorecard.h>
#include <khensu/recommendations/DynamicsRecommender.h>
#include <khensu/recommendations/RecommenderCommon.h>
#include <khensu/recommendations/SpectralRecommender.h>
#include <khensu/recommendations/StereoRecommender.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <algorithm>
#include <cmath>

namespace khensu {

using khensu::recommendations::common::severityFromError;
namespace severity_scale = khensu::recommendations::common::severity_scale;

namespace {

// --- Tonal Balance & EQ - SpectralRecommender's own rules, not a copy: the
// centroid and low-end severities are the ones its cards carry (FIX-9/10). ---
float scoreTonalBalance(const AnalysisResult& r, const GenreEntry& genre)
{
    const auto& s = r.spectral;
    const float centroidSev = SpectralRecommender::centroidSeverity(s.centroid_hz, genre);
    const float lowBalanceSev = SpectralRecommender::lowEndSeverity(s.balance_low, genre);

    const float penalty = std::clamp(0.6f * centroidSev + 0.4f * lowBalanceSev, 0.0f, 100.0f);
    return 100.0f - penalty;
}

// --- Dynamics & Compression - mirrors DynamicsRecommender's own thresholds ---
float scoreDynamics(const AnalysisResult& r, const GenreEntry& genre)
{
    const auto& d = r.dynamics;
    float penalty = 0.0f;
    // Genre-relative floor, the same one DynamicsRecommender uses (AUDIT-070:
    // a fixed 6 dB floor penalised on-target hyperpop masters).
    const float crestFloor = DynamicsRecommender::overCompressedFloorDb(genre);
    const bool overCompressed = d.crest_factor_db < crestFloor;
    if (overCompressed) penalty += 0.6f * severityFromError(d.crest_factor_db, crestFloor, severity_scale::kCrest);
    // Same rule as DynamicsRecommender's under_compressed: gated LRA, never
    // together with over-compression.
    const float overDynamicThresh = DynamicsRecommender::underCompressedLraThreshold(genre);
    const auto& l = r.loudness;
    if (!overCompressed && l.lra_measured && l.lra > overDynamicThresh)
        penalty += 0.4f * severityFromError(l.lra, overDynamicThresh, severity_scale::kLra);
    return 100.0f - std::clamp(penalty, 0.0f, 100.0f);
}

// --- Stereo Width & Depth - mirrors StereoRecommender's own thresholds ---
float scoreStereo(const AnalysisResult& r, const GenreEntry& genre)
{
    const auto& s = r.stereo;
    float penalty = 0.0f;
    // A mono source has no width to score (AUDIT-055) - only correlation.
    if (!StereoRecommender::isMonoSource(r)) {
        if (s.width < genre.stereo_width_min)
            penalty += 0.4f * severityFromError(s.width, genre.stereo_width_min, severity_scale::kWidth);
        else if (s.width > genre.stereo_width_max)
            penalty += 0.4f * severityFromError(s.width, genre.stereo_width_max, severity_scale::kWidth);
    }
    if (s.correlation < 0.0f) penalty += 0.6f * std::clamp(std::abs(s.correlation) * 100.0f, 0.0f, 100.0f);
    return 100.0f - std::clamp(penalty, 0.0f, 100.0f);
}

// --- Technical Integrity - Clipping (50%) + Noise & Artefacts (50%) ---
ScorecardCategory scoreTechnicalIntegrity(const AnalysisResult& r)
{
    const auto& a = r.artifacts;

    const float clipScore =
        a.has_clipping ? std::max(0.0f, 60.0f - std::min(60.0f, static_cast<float>(a.clipping_frame_count) * 0.5f))
                       : 100.0f;

    // Noise floor: -60 dBFS or quieter is clean; every 1 dB above that costs 2 pts.
    // The fast pass's AI-artifact heuristic is uncalibrated (AUDIT-010: it
    // read white/pink noise and typical mixes as 100% AI), so it is not
    // scored - this sub-score is the noise floor alone.
    const float noiseArtefactScore =
        std::clamp(100.0f - std::max(0.0f, (a.noise_floor_db + 60.0f) * 2.0f), 0.0f, 100.0f);

    ScorecardCategory cat;
    cat.id = "technical_integrity";
    cat.name = "Technical Integrity";
    cat.score = 0.5f * clipScore + 0.5f * noiseArtefactScore;
    cat.available = true;

    ScorecardCategory clip;
    clip.id = "clipping";
    clip.name = "Clipping";
    clip.weight = 0.5f;
    clip.score = clipScore;
    ScorecardCategory noise;
    noise.id = "noise_artefacts";
    noise.name = "Noise & Artefacts";
    noise.weight = 0.5f;
    noise.score = noiseArtefactScore;
    cat.sub_categories = {clip, noise};
    return cat;
}

// --- FX & Processing - harshness (extended analysis) ---
// The uncalibrated AI-artifact heuristic no longer contributes (AUDIT-010).
// Returns available=false when harshness data hasn't been computed yet.
ScorecardCategory scoreFxProcessing(const AnalysisResult& r)
{
    ScorecardCategory cat;
    cat.id = "fx_processing";
    cat.name = "FX & Processing";
    if (!r.harshness.has_value()) {
        cat.available = false;
        return cat;
    }
    const float harshnessPenalty = std::clamp(r.harshness->score, 0.0f, 1.0f) * 100.0f;
    cat.score = 100.0f - std::clamp(harshnessPenalty, 0.0f, 100.0f);
    cat.available = true;
    return cat;
}

// --- Frequency Masking (proxy) - resonance hotspots + mid/side imbalance (extended analysis) ---
// A true stem-vs-stem masking detector doesn't exist yet; this is an honest
// proxy from the two signals that most often correlate with masking-driven
// buildup, and is labelled as such in the UI.
ScorecardCategory scoreFrequencyMasking(const AnalysisResult& r)
{
    ScorecardCategory cat;
    cat.id = "frequency_masking";
    cat.name = "Frequency Masking";
    if (!r.resonance.has_value() && !r.mid_side_imbalance.has_value()) {
        cat.available = false;
        return cat;
    }
    float resonanceSev = 0.0f;
    if (r.resonance.has_value() && !r.resonance->hotspots.empty()) {
        float sumProminence = 0.0f;
        for (const auto& peak : r.resonance->hotspots)
            sumProminence += peak.prominence;
        const float avgProminence = sumProminence / static_cast<float>(r.resonance->hotspots.size());
        resonanceSev = std::clamp(avgProminence * 8.0f, 0.0f, 100.0f); // ~12dB avg prominence -> ~96 pts
    }
    float imbalanceSev = 0.0f;
    if (r.mid_side_imbalance.has_value())
        imbalanceSev = std::clamp(r.mid_side_imbalance->imbalance_severity, 0.0f, 1.0f) * 100.0f;

    cat.score = 100.0f - std::clamp(0.6f * resonanceSev + 0.4f * imbalanceSev, 0.0f, 100.0f);
    cat.available = true;
    return cat;
}

// --- Mix Balance - average severity of MixChannel/MixBus-stage recommendations ---
float scoreMixBalance(const std::vector<Recommendation>& recs)
{
    float sumSeverity = 0.0f;
    int count = 0;
    for (const auto& rec : recs) {
        if (rec.type == RecommendationType::Strength) continue; // green "what's right" notes are not issues (spec §4)
        if (rec.stage == ProcessingStage::MixChannel || rec.stage == ProcessingStage::MixBus) {
            sumSeverity += rec.severity;
            ++count;
        }
    }
    if (count == 0) return 100.0f;
    return 100.0f - std::clamp(sumSeverity / static_cast<float>(count), 0.0f, 100.0f);
}

} // namespace

MixScorecard buildMixScorecard(const AnalysisResult& result, const GenreEntry& genre,
                               const std::vector<Recommendation>& recommendations, const ScoreWeightProfile& weights)
{
    MixScorecard sc;

    ScorecardCategory fx = scoreFxProcessing(result);
    ScorecardCategory masking = scoreFrequencyMasking(result);

    ScorecardCategory mixBalance;
    mixBalance.id = "mix_balance";
    mixBalance.name = "Mix Balance";
    mixBalance.score = scoreMixBalance(recommendations);
    mixBalance.available = true;

    ScorecardCategory stereo;
    stereo.id = "stereo_width_depth";
    stereo.name = "Stereo Width & Depth";
    stereo.score = scoreStereo(result, genre);
    stereo.available = true;

    ScorecardCategory tonal;
    tonal.id = "tonal_balance_eq";
    tonal.name = "Tonal Balance & EQ";
    tonal.score = scoreTonalBalance(result, genre);
    tonal.available = true;

    ScorecardCategory dynamics;
    dynamics.id = "dynamics_compression";
    dynamics.name = "Dynamics & Compression";
    dynamics.score = scoreDynamics(result, genre);
    dynamics.available = true;

    ScorecardCategory technical = scoreTechnicalIntegrity(result);

    // Nominal weights (sum to 1.0 when every category is available, using stock defaults).
    fx.weight = weights.scorecard_fx_weight;
    mixBalance.weight = weights.scorecard_mix_balance_weight;
    stereo.weight = weights.scorecard_stereo_weight;
    tonal.weight = weights.scorecard_tonal_weight;
    dynamics.weight = weights.scorecard_dynamics_weight;
    masking.weight = weights.scorecard_masking_weight;
    technical.weight = weights.scorecard_technical_weight;

    sc.categories = {fx, mixBalance, stereo, tonal, dynamics, masking, technical};

    // Renormalise weights across only the available categories, so missing
    // extended-analysis data (FX & Processing / Frequency Masking need
    // /analyze/extended) reduces completeness rather than silently scoring
    // those categories as if they were perfect.
    float availableWeight = 0.0f;
    int availableCount = 0;
    for (const auto& c : sc.categories)
        if (c.available) {
            availableWeight += c.weight;
            ++availableCount;
        }

    float overall = 0.0f;
    for (auto& c : sc.categories) {
        if (!c.available) continue;
        const float normalizedWeight = availableWeight > 0.0f ? c.weight / availableWeight : 0.0f;
        overall += normalizedWeight * c.score;
    }
    sc.uncapped_score = std::clamp(overall, 0.0f, 100.0f);
    for (const auto& rec : recommendations) {
        if (rec.type == RecommendationType::Strength) continue;
        if (rec.severity >= MixScorecard::kSevereSeverity) ++sc.severe_open;
        else if (rec.severity >= MixScorecard::kModerateSeverity) ++sc.moderate_open;
    }
    float cap = 100.0f;
    if (sc.severe_open > 0) cap = MixScorecard::kSevereCap;
    else if (sc.moderate_open > 0) cap = MixScorecard::kModerateCap;
    sc.overall_score = std::min(sc.uncapped_score, cap);
    sc.completeness = static_cast<float>(availableCount) / static_cast<float>(sc.categories.size());

    // Strongest / Improve tags - only meaningful across categories that scored.
    const ScorecardCategory* best = nullptr;
    const ScorecardCategory* worst = nullptr;
    for (const auto& c : sc.categories) {
        if (!c.available) continue;
        if (best == nullptr || c.score > best->score) best = &c;
        if (worst == nullptr || c.score < worst->score) worst = &c;
    }
    for (auto& c : sc.categories) {
        if (best != nullptr && c.id == best->id) c.tag = ScorecardCategory::kTagStrongest;
        if (worst != nullptr && c.id == worst->id && worst->id != (best ? best->id : ""))
            c.tag = ScorecardCategory::kTagImprove;
    }

    // Readiness band (heuristic technical/genre alignment - not a quality or
    // audience claim; 2026-07-15 safety audit P0). The descriptor states what
    // the band means relative to the selected genre's technical targets.
    const float s = std::round(sc.overall_score);
    if (s >= 90.0f) {
        sc.tier_label = MixScorecard::kTierExceptional;
        sc.tier_audience = "Meets the heuristic technical targets for the selected genre";
    } else if (s >= 80.0f) {
        sc.tier_label = MixScorecard::kTierHighlySkilled;
        sc.tier_audience = "Close to the genre targets; a few dimensions off";
    } else if (s >= 65.0f) {
        sc.tier_label = MixScorecard::kTierSkilled;
        sc.tier_audience = "Several dimensions off the genre targets";
    } else if (s >= 50.0f) {
        sc.tier_label = MixScorecard::kTierDemoLevel;
        sc.tier_audience = "Many dimensions off target - review the flagged issues";
    } else if (s >= 30.0f) {
        sc.tier_label = MixScorecard::kTierDeveloping;
        sc.tier_audience = "Most dimensions far from the genre targets";
    } else {
        sc.tier_label = MixScorecard::kTierEarlyStage;
        sc.tier_audience = "Well outside the genre targets across most dimensions";
    }
    if (sc.capped()) {
        const int n = sc.severe_open > 0 ? sc.severe_open : sc.moderate_open;
        const std::string kind = sc.severe_open > 0 ? "severe" : "moderate";
        sc.tier_audience = "Capped by " + std::to_string(n) + " " + kind + (n == 1 ? " issue" : " issues") + " - " +
                           std::to_string(static_cast<int>(std::lround(sc.uncapped_score))) + " without " +
                           (n == 1 ? "it" : "them");
    }

    return sc;
}

} // namespace khensu
