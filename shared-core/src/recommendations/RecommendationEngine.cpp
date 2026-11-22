// shared-core/src/recommendations/RecommendationEngine.cpp
#include <khensu/analysis/AnalysisInputs.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/recommendations/LoudnessRecommender.h>
#include <khensu/recommendations/SpectralRecommender.h>
#include <khensu/recommendations/DynamicsRecommender.h>
#include <khensu/recommendations/StereoRecommender.h>
#include <khensu/recommendations/ArtifactRecommender.h>
#include <khensu/recommendations/HookRecommender.h>
#include <khensu/recommendations/MemorabilityRecommender.h>
#include <khensu/recommendations/StructuralFlowRecommender.h>
#include <khensu/recommendations/HeavinessRecommender.h>
#include <khensu/recommendations/HarshnessRecommender.h>
#include <khensu/recommendations/MonoCompatRecommender.h>
#include <khensu/recommendations/TranslationRiskRecommender.h>
#include <khensu/recommendations/ResonanceRecommender.h>
#include <khensu/recommendations/MidSideImbalanceRecommender.h>
#include <khensu/recommendations/LoudnessArcRecommender.h>
#include <khensu/recommendations/SectionFlowRecommender.h>
#include <khensu/recommendations/MaskingRecommender.h>
#include <khensu/recommendations/StrengthRecommender.h>
#include <khensu/recommendations/MixReportBuilder.h>
#include <khensu/recommendations/RecommendationEvidence.h>
#include <algorithm>
#include <array>
#include <string_view>

namespace khensu {

namespace {

// --- Measurement-trust down-weighting: the recommender consumes measurement
// confidence (the trust map is /analyze/measurement_quality) ---
//
// Maps recommendation id prefixes (makeRecommendation builds ids as
// "<code>_<fileId>") to the trust-map metric the detection is built on. Only
// codes whose *driving measurement* is one of the trust-mapped metrics appear
// here; LRA, spectral, stereo etc. have no trust entry yet and pass through
// untouched rather than being guessed at.
struct TrustBinding {
    std::string_view code_prefix;
    std::string_view metric;
};
// AUDIT-070: under_compressed and strength_dynamics_in_pocket are driven by
// the gated LRA (which has no trust entry), not by crest, so they were bound
// to the wrong measurement; crest_above_genre_target and the (now crest-driven)
// strength_transient_headroom were missing.
constexpr std::array<TrustBinding, 8> kTrustBindings{{
    {"lufs_too_loud", "loudness_lufs"},
    {"lufs_too_quiet", "loudness_lufs"},
    {"strength_lufs_on_target", "loudness_lufs"},
    {"true_peak_over", "true_peak_dbtp"},
    {"strength_true_peak_safe", "true_peak_dbtp"},
    {"over_compressed", "crest_factor_db"},
    {"crest_above_genre_target", "crest_factor_db"},
    {"strength_transient_headroom", "crest_factor_db"},
}};

// Severity multipliers. "tentative" keeps most of the urgency (the measurement
// is probably right, just imprecise); "unreliable" halves it - the issue may
// not exist at all, but silently dropping the card would hide a *possible*
// real problem, which is worse than showing it de-emphasised.
constexpr float kTentativeSeverityScale = 0.8f;
constexpr float kUnreliableSeverityScale = 0.5f;

void applyMeasurementTrust(std::vector<Recommendation>& recs, const AnalysisResult& result)
{
    if (result.measurement_trust.empty()) return;

    for (auto& r : recs) {
        for (const auto& binding : kTrustBindings) {
            if (r.id.rfind(binding.code_prefix, 0) != 0) continue;
            const auto it = result.measurement_trust.find(std::string(binding.metric));
            if (it == result.measurement_trust.end()) break;
            const auto& trust = it->second;

            if (trust.quality_flag == "tentative" || trust.quality_flag == "unreliable") {
                r.severity *= trust.quality_flag == "unreliable" ? kUnreliableSeverityScale
                                                                 : kTentativeSeverityScale;
                // The trust map's confidence is a *measurement* precision -
                // exactly what measurement_confidence documents. Never raise
                // it above what the recommender already claimed.
                r.measurement_confidence = std::min(r.measurement_confidence, trust.confidence);
                std::string caveat = "Down-weighted: the underlying " + std::string(binding.metric) +
                                     " measurement is " + trust.quality_flag +
                                     (trust.note.empty() ? "" : " " + trust.note) + ".";
                r.confidence_reason = r.confidence_reason.empty() ? caveat
                                                                  : r.confidence_reason + " " + caveat;
                if (trust.quality_flag == "unreliable" && r.abstained_reason.empty())
                    r.abstained_reason = "The " + std::string(binding.metric) +
                                         " measurement behind this is unreliable" +
                                         (trust.note.empty() ? "" : " (" + trust.note + ")") +
                                         " - treat as a hint, not a diagnosis.";
            }
            break; // first matching binding wins; ids map to exactly one metric
        }
    }
}

} // namespace

std::vector<Recommendation> RecommendationEngine::generate(const AnalysisResult& result, const GenreEntry& genre,
                                                           const CreativeTarget& target) const
{

    namespace in = analysis_inputs;
    std::vector<Recommendation> out;
    // `needs` (REF-1): the optional inputs the recommender reads. Without
    // them it raises nothing, so the fix check must not read a missing
    // finding as a resolved one (FIX-1). Recommenders that only read the
    // in-process fast pass need nothing.
    auto append = [&out](std::vector<Recommendation> recs, in::Mask needs = in::kNone) {
        for (auto& r : recs) {
            r.requires_inputs |= needs;
            out.push_back(std::move(r));
        }
    };

    // Experimental song-level models: kept, but never above a minor note.
    auto appendUnvalidated = [&append](std::vector<Recommendation> recs, in::Mask needs) {
        for (auto& r : recs)
            if (r.severity > kUnvalidatedSeverityCap) {
                r.severity = kUnvalidatedSeverityCap;
                r.confidence_reason += (r.confidence_reason.empty() ? "" : " ") +
                                       std::string("Experimental: not yet checked against listeners, so shown "
                                                   "as a minor note.");
            }
        append(std::move(recs), needs);
    };

    append(LoudnessRecommender{}.recommend(result, genre, target));
    {
        // Two spectral findings read more than the spectrum: the kick/bass
        // collision needs the drum scan, the vocal-masking one the resonances.
        auto spectral = SpectralRecommender{}.recommend(result, genre, target);
        for (auto& r : spectral) {
            if (r.id.rfind("kick_bass_collision", 0) == 0) r.requires_inputs |= in::kDrums;
            if (r.id.rfind("midrange_vocal_masking", 0) == 0) r.requires_inputs |= in::kResonance;
        }
        append(std::move(spectral));
    }
    append(DynamicsRecommender{}.recommend(result, genre, target));
    append(StereoRecommender{}.recommend(result, genre, target));
    append(ArtifactRecommender{}.recommend(result, genre, target));
    appendUnvalidated(HookRecommender{}.recommend(result, genre, target), in::kDeep);
    appendUnvalidated(MemorabilityRecommender{}.recommend(result, genre, target), in::kDeep);
    appendUnvalidated(StructuralFlowRecommender{}.recommend(result, genre, target), in::kDeep);
    appendUnvalidated(HeavinessRecommender{}.recommend(result, genre, target), in::kDeep);
    append(HarshnessRecommender{}.recommend(result, genre, target), in::kHarshness);
    append(MonoCompatRecommender{}.recommend(result, genre, target), in::kMonoCompat);
    append(TranslationRiskRecommender{}.recommend(result, genre, target), in::kTranslationRisk);
    append(ResonanceRecommender{}.recommend(result, genre, target), in::kResonance);
    append(MidSideImbalanceRecommender{}.recommend(result, genre, target), in::kMidSide);
    appendUnvalidated(LoudnessArcRecommender{}.recommend(result, genre, target), in::kSections);
    appendUnvalidated(SectionFlowRecommender{}.recommend(result, genre, target), in::kSections);
    append(MaskingRecommender{}.recommend(result, genre, target), in::kMasking);
    // Green "what's right" notes (spec §4) - severity 0, so the sort below
    // naturally places them after every real issue.
    append(StrengthRecommender{}.recommend(result, genre, target));

    // Before sorting, so a down-weighted severity also demotes the card's
    // position rather than leaving a "shaky" recommendation ranked as if its
    // measurement were solid.
    applyMeasurementTrust(out, result);
    // CE-TRU-3: the measurements, thresholds and threshold sources behind
    // every card, for the finding card and the evidence report.
    attachEvidence(out, result, genre);

    std::stable_sort(out.begin(), out.end(),
                     [](const Recommendation& a, const Recommendation& b) { return a.severity > b.severity; });
    return out;
}

std::vector<Recommendation> RecommendationEngine::recommendationsFor(const AnalysisResult& result,
                                                                     const GenreEntry& genre,
                                                                     const CreativeTarget& target) const
{
    if (isBatchAggregate(result)) return result.recommendations;
    return generate(result, genre, target);
}

MixReport RecommendationEngine::generateReport(const AnalysisResult& result, const GenreEntry& genre,
                                               const CreativeTarget& target) const
{
    auto recs = recommendationsFor(result, genre, target);
    return buildMixReport(std::move(recs), result, genre, target);
}

} // namespace khensu
