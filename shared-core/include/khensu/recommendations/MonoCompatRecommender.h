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

class MonoCompatRecommender {
  public:
    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& /*genre*/,
                                          const CreativeTarget& /*target*/) const
    {
        std::vector<Recommendation> out;
        if (!result.mono_compat.has_value()) return out;
        const auto& mc = *result.mono_compat;

        bool low_end_wide = mc.low_end_stereo_pct > 30.0f;
        bool band_fail = false;
        std::string worst_band;
        float worst_corr = 1.0f;

        for (const auto& kv : mc.per_band_correlation) {
            float threshold = 0.4f;
            if (kv.first == "midrange" || kv.first == "upper_mid") {
                threshold = 0.0f;
            } else if (kv.first == "air") {
                threshold = -0.2f;
            }

            if (kv.second < threshold && kv.second < worst_corr) {
                band_fail = true;
                worst_band = kv.first;
                worst_corr = kv.second;
            }
        }

        auto makeRec = [&](const std::string& subCode, float sev, const std::string& what, const std::string& action) {
            Recommendation r;
            r.id = "mono_compat_warning_" + subCode + "_" + result.file_id;
            r.type = RecommendationType::Technical;
            r.severity = sev;
            r.confidence = 0.80f;
            r.stem_or_bus = "Master";
            r.is_target_relative = false;
            r.safe_to_auto_apply = false;
            r.stage = ProcessingStage::MixBus;
            r.chain_position = 6;
            r.is_taste_decision = false;
            r.what = what;
            r.why_helps = "Mono-compatible low end ensures the mix sounds full on phones, "
                          "club PA systems, and any device playing in mono.";
            r.suggested_action = action;
            r.engineer_note = "Mono compat is critical for streaming on phones and for club PA. "
                              "Most engineers mono-ize below 100 Hz as standard practice.";
            r.confidence_reason = "Per-band correlation computed via bandpass filtering + pearson correlation.";
            r.trade_off = "Mono-izing the low end may slightly reduce perceived stereo width of bass elements.";
            r.how_to_verify = "Sum the mix to mono and check that kick, bass, and sub are still audible "
                              "and punchy. A significant volume drop on these elements indicates mono cancellation.";
            r.alternative_strategies = {
                "Use the Brauer Motion or Infected Mushroom Stereoizer only on mid/high frequencies.",
                "Use an MS encoder, process only the side channel, and keep bass in mid.",
            };
            return r;
        };

        if (low_end_wide) {
            float low_sev = std::clamp((mc.low_end_stereo_pct - 30.0f) * 2.0f, 30.0f, 100.0f);
            std::string what = "Low-end stereo content is " + std::to_string(int(mc.low_end_stereo_pct)) +
                               "% uncorrelated - bass energy may collapse or cancel in mono.";
            std::string action = "Use a mid-side EQ or bass mono plugin to mono-ize content below 100-120 Hz.";
            out.push_back(makeRec("low_end", low_sev, what, action));
        }

        if (band_fail) {
            float band_sev = std::clamp((0.4f - worst_corr) * 200.0f, 30.0f, 100.0f);
            std::string what = "Stereo correlation in the " + worst_band + " band is " +
                               std::to_string(worst_corr).substr(0, 4) +
                               " - may cause phase cancellation in mono playback.";
            std::string action = "Check stereo-imaging on the " + worst_band +
                                 " bus. Reduce or remove stereo processing below the problem band.";
            out.push_back(makeRec("band_correlation", band_sev, what, action));
        }

        return out;
    }
};

} // namespace khensu
