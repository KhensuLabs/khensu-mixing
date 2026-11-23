#pragma once
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

// Shared scaffolding extracted from the per-domain Recommenders
// (LoudnessRecommender, StereoRecommender, DynamicsRecommender, etc.), which
// each defined byte-for-byte identical severity-scoring and Recommendation-
// construction lambdas. Kept here so a change to the severity formula or to
// Recommendation's required fields is made once instead of in every file.
namespace khensu::recommendations::common {

/// 0-100 severity on an ABSOLUTE scale: |measured - threshold| / fullScale.
/// Every severity goes through a fixed full scale (FIX-10): dividing the
/// error by the threshold made severity depend on where the threshold happens
/// to sit - 1 dB over -11 LUFS scored 9 but 1 dB over -23 LUFS scored 4, a -1
/// dB/oct tilt target saturated at 100 for a 1.5 dB/oct miss, and a 0-1 score
/// 0.10 short scored 33 against a 0.30 threshold but 14 against 0.70.
/// `fullScale` is the error that counts as maximally severe for that metric.
inline float severityFromError(float measured, float threshold, float fullScale) noexcept
{
    return std::clamp(100.0f * std::abs(measured - threshold) / std::max(fullScale, 1e-4f), 0.0f, 100.0f);
}

/// 0-100 severity for a frequency: the distance between `measuredHz` and
/// `thresholdHz` in OCTAVES over `fullScaleOctaves`. Pitch and brightness are
/// heard as ratios, so the same error is the same ratio, not the same number
/// of Hz - 500 Hz is over half an octave at a 1 kHz edge and a tenth of one
/// at 6.5 kHz. Values under 1 Hz (an unmeasured 0) are read as 1 Hz.
inline float severityFromOctaves(float measuredHz, float thresholdHz, float fullScaleOctaves) noexcept
{
    const float octaves = std::abs(std::log2(std::max(measuredHz, 1.0f) / std::max(thresholdHz, 1.0f)));
    return std::clamp(100.0f * octaves / std::max(fullScaleOctaves, 1e-4f), 0.0f, 100.0f);
}

/// Full-scale errors for severityFromError / severityFromOctaves (shared by
/// recommenders and the scorecard so both rank problems identically).
namespace severity_scale {
inline constexpr float kLufs = 12.0f;  ///< 12 dB off the genre window = maximal
inline constexpr float kLra = 10.0f;   ///< LU
inline constexpr float kCrest = 6.0f;  ///< dB
inline constexpr float kTilt = 4.0f;   ///< dB/octave
inline constexpr float kWidth = 0.5f;  ///< 0..1 width units
/// Octaves past the genre's centroid window. 1.5 puts a typical 3.4 kHz genre
/// at maximal near 12 kHz or 900 Hz, and keeps small misses where dividing by
/// the threshold had them (10% over: 10 -> 9; 25% under: 25 -> 28).
inline constexpr float kCentroidOctaves = 1.5f;
/// A 0..1 energy share (low-end balance), same span as kWidth: half the whole
/// range past the edge is maximal. The old divisor was the edge itself, which
/// sat between 0.20 and 0.95 depending on the genre.
inline constexpr float kBandShare = 0.5f;
/// A 0..1 analysis score (hooks, memorability, arc quality, heaviness). 0.5
/// was already the divisor at the default targets (memorability threshold
/// 0.50, arc floor 0.50), so those severities are unchanged.
inline constexpr float kUnitScore = 0.5f;
/// DC offset, as a mean sample value: 0.05 (-26 dBFS) past the 0.002
/// threshold is maximal.
inline constexpr float kDcOffset = 0.05f;
} // namespace severity_scale

/// A band around a 0..1 target that every target can reach (FIX-9): the edges
/// are one fixed change in the odds p / (1 - p) either way, so they stay
/// inside 0..1 for any target. A fixed +/-0.20 band could not fire above a
/// 0.85 target (the score cannot pass 1.05) or below a 0.15 one. `ratio` is
/// the odds factor: 7/3 gives exactly +/-0.20 at a 0.50 target, 3 gives
/// +/-0.25.
inline float unitOddsCeiling(float target, float ratio) noexcept
{
    const float t = std::clamp(target, 0.0f, 1.0f);
    const float hi = t * ratio;
    const float sum = hi + (1.0f - t);
    return sum > 0.0f ? hi / sum : t;
}
inline float unitOddsFloor(float target, float ratio) noexcept
{
    const float t = std::clamp(target, 0.0f, 1.0f);
    const float sum = t + (1.0f - t) * ratio;
    return sum > 0.0f ? t / sum : t;
}
/// A firing threshold for a 0..1 score, kept where a score can sit on either
/// side of it (a threshold of 1.0 fires on every mix, 0.0 on none).
inline float reachableUnitThreshold(float t) noexcept { return std::clamp(t, 0.05f, 0.95f); }
/// TUNE-8: the bands are wide because the scores are not yet validated by
/// listeners. At the old +/-0.20 (extremity) and +/-0.25 (distortion) bands
/// the notes fired on 65% and 43% of 203 released tracks (80% of them sit
/// above the hand-set extremity threshold); at odds 10 and 8, on 16% and 12%.
inline constexpr float kHeavinessExtremityOdds = 10.0f; ///< 0.09..0.91 at 0.50
inline constexpr float kHeavinessDistortionOdds = 8.0f; ///< 0.11..0.89 at 0.50

/// Fixed-point float formatting used throughout recommendation text.
inline std::string formatFixed(float v, int decimals)
{
    char buf[16];
    const char* fmt = decimals >= 2 ? "%.2f" : (decimals == 1 ? "%.1f" : "%.0f");
    std::snprintf(buf, sizeof(buf), fmt, static_cast<double>(v));
    return buf;
}
inline std::string f0(float v) { return formatFixed(v, 0); }
inline std::string f1(float v) { return formatFixed(v, 1); }
inline std::string f2(float v) { return formatFixed(v, 2); }

/// Builds a Recommendation with the boilerplate fields common to every
/// recommender's `make()` lambda. `confidence` and `stemOrBus` are explicit
/// (not defaulted) because they vary meaningfully by domain - e.g. Loudness
/// recommendations carry confidence 0.90, Stereo 0.80, Hook 0.70.
inline Recommendation makeRecommendation(const std::string& fileId, const char* code, float severity, float confidence,
                                         const std::string& stemOrBus, ProcessingStage stage, int chainPosition,
                                         bool isTasteDecision = false, bool isTargetRelative = false,
                                         bool safeToAutoApply = false,
                                         RecommendationType type = RecommendationType::Technical)
{
    Recommendation r;
    r.id = std::string(code) + "_" + fileId;
    r.type = type;
    r.severity = severity;
    r.confidence = confidence;
    // The legacy per-domain `confidence` is, at best, measurement precision -
    // record it as such. Interventions remain unvalidated until an evaluation
    // corpus exists (2026-07-15 safety audit P0/P2).
    r.measurement_confidence = confidence;
    r.intervention_validated = false;
    r.stem_or_bus = stemOrBus;
    r.is_target_relative = isTargetRelative;
    r.safe_to_auto_apply = safeToAutoApply;
    r.stage = stage;
    r.chain_position = chainPosition;
    r.is_taste_decision = isTasteDecision;
    return r;
}

} // namespace khensu::recommendations::common
