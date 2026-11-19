#pragma once
#include <khensu/feedback/FeedbackManager.h>
#include <cmath>
#include <string>

namespace khensu {

/// User-adjustable category weights for the Mastering Readiness score and
/// the Mix Scorecard, saved per genre via FeedbackManager's existing
/// preference key/value store (same mechanism UserPreferenceModel uses),
/// namespaced by genre id so different genres/workspace modes keep
/// independent profiles. Defaults match the previously-hardcoded constants
/// in MixReportBuilder::computeReadiness / buildMixScorecard.
struct ScoreWeightProfile {
    // Mastering Readiness penalty caps (points deducted, max per category).
    float readiness_headroom_cap = 25.0f;
    float readiness_crest_cap = 20.0f;
    float readiness_low_end_cap = 20.0f;
    float readiness_stereo_cap = 15.0f;
    float readiness_critical_recs_cap = 20.0f;
    float readiness_streaming_cap = 15.0f;

    // Mix Scorecard nominal category weights (renormalized across whichever
    // categories are available for the current analysis).
    float scorecard_fx_weight = 0.15f;
    float scorecard_mix_balance_weight = 0.15f;
    float scorecard_stereo_weight = 0.15f;
    float scorecard_tonal_weight = 0.15f;
    float scorecard_dynamics_weight = 0.15f;
    float scorecard_masking_weight = 0.10f;
    float scorecard_technical_weight = 0.15f;

    /// Loads the saved profile for `genreId`. Each weight falls back to its
    /// current (default) value when the read fails or the stored value is
    /// non-finite or negative (CORE-04) - a DB busy timeout used to turn
    /// every cap and weight into 0 silently, so readiness read 100 "Pass"
    /// while the scorecard read 0. Returns false when any read failed.
    bool loadFromDatabase(FeedbackManager& fm, const std::string& genreId) noexcept
    {
        bool allOk = true;
        auto load = [&](const char* suffix, float& field) {
            const auto r = fm.getPreference("score_weight_" + std::string(suffix) + "_" + genreId, field);
            if (r.is_err()) {
                allOk = false;
                return;
            }
            if (sanitizedWeight(r.value())) field = r.value();
        };
        load("readiness_headroom_cap", readiness_headroom_cap);
        load("readiness_crest_cap", readiness_crest_cap);
        load("readiness_low_end_cap", readiness_low_end_cap);
        load("readiness_stereo_cap", readiness_stereo_cap);
        load("readiness_critical_recs_cap", readiness_critical_recs_cap);
        load("readiness_streaming_cap", readiness_streaming_cap);

        load("scorecard_fx_weight", scorecard_fx_weight);
        load("scorecard_mix_balance_weight", scorecard_mix_balance_weight);
        load("scorecard_stereo_weight", scorecard_stereo_weight);
        load("scorecard_tonal_weight", scorecard_tonal_weight);
        load("scorecard_dynamics_weight", scorecard_dynamics_weight);
        load("scorecard_masking_weight", scorecard_masking_weight);
        load("scorecard_technical_weight", scorecard_technical_weight);
        return allOk;
    }

    /// A stored weight is usable only when finite and non-negative.
    static bool sanitizedWeight(float v) noexcept { return std::isfinite(v) && v >= 0.0f; }

    void saveToDatabase(FeedbackManager& fm, const std::string& genreId) const noexcept
    {
        auto key = [&](const char* suffix) { return "score_weight_" + std::string(suffix) + "_" + genreId; };
        fm.setPreference(key("readiness_headroom_cap"), readiness_headroom_cap);
        fm.setPreference(key("readiness_crest_cap"), readiness_crest_cap);
        fm.setPreference(key("readiness_low_end_cap"), readiness_low_end_cap);
        fm.setPreference(key("readiness_stereo_cap"), readiness_stereo_cap);
        fm.setPreference(key("readiness_critical_recs_cap"), readiness_critical_recs_cap);
        fm.setPreference(key("readiness_streaming_cap"), readiness_streaming_cap);

        fm.setPreference(key("scorecard_fx_weight"), scorecard_fx_weight);
        fm.setPreference(key("scorecard_mix_balance_weight"), scorecard_mix_balance_weight);
        fm.setPreference(key("scorecard_stereo_weight"), scorecard_stereo_weight);
        fm.setPreference(key("scorecard_tonal_weight"), scorecard_tonal_weight);
        fm.setPreference(key("scorecard_dynamics_weight"), scorecard_dynamics_weight);
        fm.setPreference(key("scorecard_masking_weight"), scorecard_masking_weight);
        fm.setPreference(key("scorecard_technical_weight"), scorecard_technical_weight);
    }
};

} // namespace khensu
