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
            if (risk <= 0.60f) continue;

            const float sev = std::clamp((risk - 0.60f) * 250.0f, 20.0f, 100.0f);

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
