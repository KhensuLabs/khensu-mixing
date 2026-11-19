// shared-core/include/khensu/recommendations/MaskingRecommender.h
#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <khensu/common/FrequencyRange.h>
#include <algorithm>
#include <string>
#include <vector>

namespace khensu {

/// Turns StemMaskingPanel's pair-collision scores into first-class
/// Recommendations instead of leaving the detector purely display-only and
/// disconnected from the copilot brain (2026-07-07 audit finding). Only
/// fires once the user has actually run "Analyze Masking" - result.masking
/// is nullopt otherwise, so tracks with no separated stems get no
/// masking-driven recommendations rather than a fabricated one.
class MaskingRecommender {
  public:
    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& /*genre*/,
                                          const CreativeTarget& /*target*/) const
    {
        std::vector<Recommendation> out;
        if (!result.masking.has_value()) return out;
        const auto& masking = *result.masking;

        // Below this, two stems sharing some spectral territory is normal
        // and not worth surfacing as an actionable recommendation.
        constexpr float kModerateThreshold = 0.5f;

        for (const auto& pair : masking.pairs) {
            if (pair.collision_score < kModerateThreshold) continue;

            // Localize the suggestion to whichever band contributes most to
            // the overlap, rather than only reporting the aggregate score.
            size_t worstBand = 0;
            float worstScore = -1.0f;
            for (size_t i = 0; i < pair.band_scores.size(); ++i) {
                if (pair.band_scores[i] > worstScore) {
                    worstScore = pair.band_scores[i];
                    worstBand = i;
                }
            }
            const float bandLoHz = worstBand < masking.bands_hz.size() ? masking.bands_hz[worstBand] : 0.0f;
            const float bandHiHz =
                (worstBand + 1) < masking.bands_hz.size() ? masking.bands_hz[worstBand + 1] : bandLoHz * 2.0f;
            const int pctRisk = static_cast<int>(pair.collision_score * 100.0f);

            Recommendation r;
            r.id = "masking_collision_" + pair.stem_a + "_" + pair.stem_b + "_" + result.file_id;
            r.type = RecommendationType::Technical;
            r.severity = std::clamp(pair.collision_score * 100.0f, 30.0f, 95.0f);
            r.confidence = 0.7f;
            r.confidence_reason = "Overlap coefficient of normalized per-band energy distributions "
                                  "between the two separated stems.";
            r.stem_or_bus = pair.stem_a + " + " + pair.stem_b;
            r.freq_range = FrequencyRange{bandLoHz, bandHiHz};
            r.most_likely_cause = pair.stem_a + " and " + pair.stem_b +
                                  " occupy overlapping frequency "
                                  "space, most concentrated around " +
                                  std::to_string((int)bandLoHz) + "-" + std::to_string((int)bandHiHz) + " Hz.";
            r.what = "High frequency collision risk (" + std::to_string(pctRisk) + "%) between " + pair.stem_a +
                     " and " + pair.stem_b + ".";
            r.why_helps = "Carving complementary space for each element reduces masking, improving "
                          "clarity and perceived separation between the two parts.";
            // Cut depth scales with how severe the collision actually is -
            // a borderline 0.5 collision gets a gentle -2 dB nudge, while a
            // near-total 1.0 overlap gets a full -6 dB carve, instead of a
            // single fixed cut regardless of severity.
            const float cutDb = -2.0f - std::clamp(pair.collision_score, 0.0f, 1.0f) * 4.0f;

            r.suggested_action = "Apply a complementary EQ cut on one of the two stems around " +
                                 std::to_string((int)bandLoHz) + "-" + std::to_string((int)bandHiHz) +
                                 " Hz (e.g. cut " + pair.stem_b + " to make room for " + pair.stem_a + ").";
            r.suggested_parameters = {{"eq_freq_hz", (bandLoHz + bandHiHz) / 2.0f}, {"eq_gain_db", cutDb}};
            r.is_target_relative = false;
            r.safe_to_auto_apply = false;
            r.stage = ProcessingStage::MixChannel;
            r.chain_position = 5;
            r.is_taste_decision = true;
            r.how_to_verify = "Solo both stems together, toggle the suggested EQ cut, and listen for "
                              "improved separation without losing the cut stem's presence.";
            r.trade_off = "Cutting one stem to make room for the other can thin it out if overdone - "
                          "start with a narrow, shallow cut and widen only if the collision persists.";
            r.engineer_note = "Frequency masking between two elements sharing the same band is one of "
                              "the most common causes of a 'muddy' or 'cluttered' mix - carving out "
                              "space for each element, rather than just turning one down, usually reads "
                              "as more transparent.";
            out.push_back(std::move(r));
        }
        return out;
    }
};

} // namespace khensu
