#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <khensu/recommendations/TargetInterpolator.h>
#include <khensu/recommendations/RecommenderCommon.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace khensu {

class MemorabilityRecommender {
  public:
    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& /*genre*/,
                                          const CreativeTarget& target) const
    {
        if (!result.has_python_analysis()) return {};

        std::vector<Recommendation> out;
        const auto& mem = result.python_data.memorability;
        // TUNE-8: without a detected hook, earworm likelihood is 0 and
        // memorability only structure and tempo, so both fired on every such
        // track (a third of released ones) and repeated no_hooks_detected.
        if (result.python_data.hooks.hook_count == 0) return out;

        using namespace khensu::recommendations::common;
        auto make = [&](const char* code, float sevVal, bool isTargetRel, ProcessingStage stg, int pos,
                        bool taste = false) {
            return makeRecommendation(result.file_id, code, sevVal, 0.70f, "", stg, pos, taste, isTargetRel, false,
                                      RecommendationType::Musical);
        };
        // 0-1 scores share one fixed full scale, so the same shortfall is the same
        // severity at every genre norm and target setting (FIX-10).
        auto sev = [](float measured, float threshold) {
            return severityFromError(measured, threshold, severity_scale::kUnitScore);
        };

        const float mem_thresh = TargetInterpolator::memorabilityThreshold(target);
        // Every finding carries its own text: the screens show `what` and
        // `suggested_action`, and a blank one drew an empty card (2026-11-18).
        if (mem.score < mem_thresh) {
            auto r = make("memorability_low", sev(mem.score, mem_thresh), target.memorability_target != 50.0f,
                          ProcessingStage::Arrangement, 1, true);
            r.what = "Memorability score is " + f2(mem.score) + ", below the target of " + f2(mem_thresh) + ".";
            r.why_helps = "Memorability predicts how likely listeners are to recall and seek out a track after "
                          "first hearing it.";
            r.suggested_action = "Focus on one strong melodic or rhythmic signature, repeated with subtle "
                                 "variation across the track.";
            r.how_to_verify = "Play the track, then something else, then come back: does anything stick? Highly "
                              "subjective - a prompt for creative revision, not a technical fix.";
            out.push_back(std::move(r));
        }

        const float earworm_thresh = TargetInterpolator::earwormThreshold(target);
        if (mem.earworm_likelihood < earworm_thresh) {
            auto r = make("earworm_likelihood_low", sev(mem.earworm_likelihood, earworm_thresh),
                          target.memorability_target != 50.0f, ProcessingStage::Arrangement, 1, true);
            r.what = "Earworm likelihood is " + f2(mem.earworm_likelihood) + ", below the target of " +
                     f2(earworm_thresh) + ".";
            r.why_helps = "Earworm-prone melodies get stuck in the listener's head, which drives sharing and return "
                          "listens.";
            r.suggested_action = "Write a concise 4-8 note melodic hook with slight rhythmic syncopation. "
                                 "Unexpected intervals or a gap-fill pattern raise earworm potential.";
            r.how_to_verify = "Wait 30 minutes after listening and try to hum the main melody from memory. A "
                              "creative call - use your judgment.";
            out.push_back(std::move(r));
        }

        return out;
    }
};

} // namespace khensu
