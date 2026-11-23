#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <algorithm>
#include <string>
#include <vector>

namespace khensu {

class TranslationRiskRecommender {
  public:
    /// TUNE-8: the phone score runs high on released masters (median 0.43,
    /// 85th percentile 0.62) and the others low (car / club median ~0.01), so
    /// one 0.60 edge was the phone alone: 17% of 203 released tracks, 31% of a
    /// +10 dB low shelf, 2% of a -12 dB low cut. Phone 0.65 and 0.45 elsewhere:
    /// 18%, 24% and 17%. The score is weakly sensitive either way.
    static constexpr float kPhoneRiskEdge = 0.65f;
    static constexpr float kOtherRiskEdge = 0.45f;
    static float riskEdge(const std::string& env) { return env == "phone" ? kPhoneRiskEdge : kOtherRiskEdge; }

    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& /*genre*/,
                                          const CreativeTarget& /*target*/) const
    {
        std::vector<Recommendation> out;
        if (!result.translation_risk.has_value()) return out;
        const auto& tr = *result.translation_risk;

        static const std::pair<const char*, const char*> kEnvDesc[] = {
            {"earbuds", "earbuds / consumer in-ears"},
            {"phone", "phone speaker"},
            {"car", "car audio system"},
            {"club", "club PA system"},
            {"studio", "studio reference monitoring"},
        };

        for (const auto& [envKey, envLabel] : kEnvDesc) {
            auto it = tr.per_environment.find(envKey);
            if (it == tr.per_environment.end()) continue;
            const float risk = it->second;
            const float edge = riskEdge(envKey);
            if (risk <= edge) continue;

            const float sev = std::clamp((risk - edge) * 250.0f, 20.0f, 100.0f);

            Recommendation r;
            r.id = std::string("translation_risk_high_") + envKey + "_" + result.file_id;
            r.type = RecommendationType::Technical;
            r.severity = sev;
            r.confidence = 0.70f;
            r.stem_or_bus = "Master";
            r.is_target_relative = false;
            r.safe_to_auto_apply = false;
            r.stage = ProcessingStage::MixBus;
            r.chain_position = 6;
            r.is_taste_decision = false;
            r.what = "High translation risk (" + std::to_string(int(risk * 100)) + "/100) for " +
                     std::string(envLabel) + ".";
            r.why_helps = "Addressing translation issues ensures the mix sounds intentional "
                          "on " +
                          std::string(envLabel) + ", where most listeners will hear it.";
            r.suggested_action = "Test the mix on " + std::string(envLabel) +
                                 " and address "
                                 "the most prominent issue (see mono_compat_warning and harshness_detected recs).";
            r.engineer_note = "Translation risk score is a composite estimate. Verify by actually "
                              "listening on the target device.";
            r.confidence_reason = "Computed from spectral centroid, balance_low, stereo correlation, "
                                  "and crest factor. Indicative, not definitive.";
            r.trade_off = "Optimizing for one playback environment may shift the mix "
                          "away from studio reference. Prioritize the most common playback context.";
            r.how_to_verify = "Play the mix on actual " + std::string(envLabel) +
                              " at typical listening level. Note any frequency imbalance or phasing.";
            r.alternative_strategies = {
                "A/B against a reference track on the same playback device.",
                "Use a speaker emulator plugin (e.g., Audified CR8) to simulate the target device in the DAW.",
            };
            out.push_back(std::move(r));
        }
        return out;
    }
};

} // namespace khensu
