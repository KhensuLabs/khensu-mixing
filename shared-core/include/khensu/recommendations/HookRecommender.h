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

class HookRecommender {
  public:
    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& genre,
                                          const CreativeTarget& target) const
    {
        if (!result.has_python_analysis()) return {};

        std::vector<Recommendation> out;
        const auto& hooks = result.python_data.hooks;

        using namespace khensu::recommendations::common;
        auto make = [&](const char* code, float sevVal, ProcessingStage stg, int pos, bool taste = false) {
            return makeRecommendation(result.file_id, code, sevVal, 0.70f, "Hook", stg, pos, taste, false, false,
                                      RecommendationType::Musical);
        };
        // 0-1 scores share one fixed full scale, so the same shortfall is the same
        // severity at every genre norm and target setting (FIX-10).
        auto sev = [](float measured, float threshold) {
            return severityFromError(measured, threshold, severity_scale::kUnitScore);
        };

        // no_hooks_detected - fixed severity 85
        // Every finding carries its own text: the screens show `what` and
        // `suggested_action`, and a blank one drew an empty card (2026-11-18).
        if (hooks.hook_count == 0) {
            auto r = make("no_hooks_detected", 85.0f, ProcessingStage::Arrangement, 1, true);
            r.what = "No hook segments detected in the audio.";
            r.why_helps = "Hooks are the main reason listeners replay a track. Tracks without an identifiable "
                          "hook tend to be replayed less.";
            r.suggested_action = "Introduce a recurring melodic, rhythmic or textural motif. Repetition with "
                                 "variation across sections builds hook identity.";
            r.how_to_verify = "Listen for recurring melodic or rhythmic segments. If no clear hook emerges after "
                              "two or three listens, the detection is likely right. This is a creative call, "
                              "common in ambient and experimental music.";
            out.push_back(std::move(r));
            return out; // no point checking density/strength if no hooks exist
        }

        // hook_density_low
        const float density_threshold = TargetInterpolator::hookDensityThreshold(genre, target);
        if (hooks.hook_density < density_threshold) {
            auto r = make("hook_density_low", sev(hooks.hook_density, density_threshold), ProcessingStage::Arrangement,
                          1, true);
            r.is_target_relative = (target.hook_emphasis != 50.0f);
            r.what = "Hook density is " + f2(hooks.hook_density) + ", below the target of " + f2(density_threshold) +
                     " for this setting.";
            r.why_helps = "More frequent hooks keep listeners engaged and drive replays.";
            r.suggested_action = "Repeat your strongest melodic or rhythmic motif more often. Consider calling the "
                                 "hook back in the bridge and outro.";
            r.how_to_verify = "Count the memorable moments in the first 90 seconds; most commercial genres land one "
                              "before the 45-second mark. A creative guideline, not a technical rule.";
            out.push_back(std::move(r));
        }

        // hook_strength_low
        const float strength_threshold = TargetInterpolator::hookStrengthThreshold(target);
        if (hooks.hook_strength < strength_threshold) {
            auto r = make("hook_strength_low", sev(hooks.hook_strength, strength_threshold),
                          ProcessingStage::Arrangement, 1, true);
            // time_range from weakest HookInstance (if any instances available)
            if (!hooks.instances.empty()) {
                const auto* weakest = &hooks.instances[0];
                for (const auto& inst : hooks.instances)
                    if (inst.strength < weakest->strength) weakest = &inst;
                r.time_range = TimeRange{weakest->start_sec, weakest->end_sec};
            }
            r.is_target_relative = (target.hook_emphasis != 50.0f);
            r.what = "Hook strength is " + f2(hooks.hook_strength) + ", below the target of " +
                     f2(strength_threshold) + ".";
            r.why_helps = "Hooks with clear melody, rhythm and tone are more likely to be recalled after the track "
                          "ends.";
            r.suggested_action = "Give the hook clearer tonal movement and more contrast with the verse: louder, "
                                 "brighter or rhythmically more pronounced.";
            r.how_to_verify = "Play the track to someone new to it and ask them to hum the main melody 30 minutes "
                              "later. A creative call - use your judgment.";
            out.push_back(std::move(r));
        }

        return out;
    }
};

} // namespace khensu
