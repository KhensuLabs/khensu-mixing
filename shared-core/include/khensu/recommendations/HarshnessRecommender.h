#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace khensu {

class HarshnessRecommender {
  public:
    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& /*genre*/,
                                          const CreativeTarget& /*target*/) const
    {
        std::vector<Recommendation> out;
        if (!result.harshness.has_value()) return out;
        const auto& h = *result.harshness;

        if (h.score <= 0.35f) return out;

        const float sev = std::clamp(h.score * 100.0f, 0.0f, 100.0f);
        const float peak_hz = h.peak_hz.value_or(0.0f);

        Recommendation r;
        r.id = "harshness_detected_" + result.file_id;
        r.type = RecommendationType::Technical;
        r.severity = sev;
        r.confidence = 0.75f;
        r.stem_or_bus = "Master";
        r.is_target_relative = false;
        r.safe_to_auto_apply = false;
        r.stage = ProcessingStage::Master;
        r.chain_position = 7;
        r.is_taste_decision = false;
        r.what = "Harshness detected in the 2.5-5 kHz region (score: " + std::to_string(int(h.score * 100)) + "/100).";
        r.why_helps = "Reducing 2.5-5 kHz harshness improves long-term listening comfort "
                      "and translates better on earbuds and consumer systems.";
        r.suggested_action = "Apply gentle high-shelf or bell EQ cut around " +
                             (peak_hz > 0.0f ? std::to_string(int(peak_hz)) + " Hz." : "3.5 kHz.") +
                             " A de-esser on the master bus can also tame transient harshness.";
        r.engineer_note = "Harshness score " + std::to_string(h.score).substr(0, 4) +
                          " - indicates elevated energy in the 'ear fatigue' region.";
        r.confidence_reason = "Harshness computed from 2.5-5 kHz RMS-to-mid-band ratio. Moderate certainty.";
        r.trade_off = "Cutting this region may reduce clarity and intelligibility of hi-hats, "
                      "percussion, and consonants. Cut by no more than 2-3 dB.";
        r.how_to_verify = "Listen on consumer earbuds for 90 seconds at moderate volume. "
                          "If listening fatigue sets in before 90s, harshness may still be present.";
        r.alternative_strategies = {
            "Use multiband compression to control only transient energy in the 2.5-5 kHz band.",
            "Check individual channel contributions - often a specific instrument is the source.",
        };
        out.push_back(std::move(r));
        return out;
    }
};

} // namespace khensu
