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
#include <cstdio>
#include <string>
#include <vector>

namespace khensu {

class HeavinessRecommender {
  public:
    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& genre,
                                          const CreativeTarget& target) const
    {
        if (!result.has_python_analysis()) return {};

        std::vector<Recommendation> out;
        const auto& h = result.python_data.heaviness;

        using namespace khensu::recommendations::common;
        auto make = [&](const char* code, float sevVal, bool isTargetRel, ProcessingStage stg, int pos,
                        bool taste = false) {
            return makeRecommendation(result.file_id, code, sevVal, 0.70f, "Low End Bus", stg, pos, taste, isTargetRel,
                                      false, RecommendationType::Musical);
        };
        // 0-1 scores share one fixed full scale, so the same shortfall is the same
        // severity at every genre norm and target setting (FIX-10).
        auto sev = [](float measured, float threshold) {
            return severityFromError(measured, threshold, severity_scale::kUnitScore);
        };

        // heaviness_extremity_mismatch: outside an odds band around the threshold
        // (+/-0.20 at 0.50, reachable at every target - FIX-9).
        const float ext_thresh = TargetInterpolator::heavinessExtremityThreshold(genre, target);
        if (h.extremity > unitOddsCeiling(ext_thresh, kHeavinessExtremityOdds) ||
            h.extremity < unitOddsFloor(ext_thresh, kHeavinessExtremityOdds)) {
            const bool too_light = h.extremity < ext_thresh;
            auto r = make("heaviness_extremity_mismatch", sev(h.extremity, ext_thresh),
                          target.heaviness_target != 50.0f, ProcessingStage::MixBus, 4, true);
            r.what = std::string("Heaviness extremity score is ") + f2(h.extremity) + " - " +
                     (too_light ? "below" : "above") + " the " + f2(ext_thresh) + " target for this genre/intent.";
            r.why_helps = too_light
                              ? "The overall sonic weight sits lighter than the genre demands. Increasing sub-bass "
                                "presence and low-mid saturation will deliver the visceral impact listeners expect."
                              : "The mix is heavier than the target, which can sound fatiguing and reduce clarity "
                                "on smaller speakers. Trimming low-end weight improves translation.";
            r.suggested_action =
                too_light ? "Boost sub-bass (<=80 Hz) presence and/or add low-mid saturation to raise the "
                            "heaviness extremity score toward " +
                                f2(ext_thresh) + "."
                          : "High-pass or attenuate the sub-bass shelf (<=80 Hz) to reduce overall heaviness "
                            "extremity toward " +
                                f2(ext_thresh) + ".";
            r.engineer_note = "This is a taste decision - consider the intended playback context "
                              "(club PA vs. headphones) before acting.";
            r.how_to_verify = "Re-run deep analysis and check that heaviness extremity is between " +
                              f2(unitOddsFloor(ext_thresh, kHeavinessExtremityOdds)) + " and " +
                              f2(unitOddsCeiling(ext_thresh, kHeavinessExtremityOdds)) + ".";
            out.push_back(std::move(r));
        }

        // heaviness_weight_low: weight < threshold AND heaviness_target > 50
        const float wt_thresh = TargetInterpolator::heavinessWeightThreshold(genre, target);
        if (h.weight < wt_thresh && target.heaviness_target > 50.0f) {
            auto r = make("heaviness_weight_low", sev(h.weight, wt_thresh), true, ProcessingStage::MixBus, 4, true);
            r.what = "Sub-bass weight score is " + f2(h.weight) + " - below the " + f2(wt_thresh) +
                     " threshold needed "
                     "for the requested heaviness target.";
            r.why_helps = "Insufficient sub-bass weight makes heavy genres feel thin, especially "
                          "on systems with subwoofers or large studio monitors. More sub presence "
                          "anchors the low end and delivers the expected physical impact.";
            r.suggested_action = "Boost the sub-bass group (<=80 Hz) by 2-4 dB, or layer a sine-wave "
                                 "sub underneath the kick/bass to raise the weight score above " +
                                 f2(wt_thresh) + ".";
            r.engineer_note = "Check that the low-end bus is not over-compressed - a fast-attack "
                              "bus compressor can squash the sub transients that drive weight perception.";
            r.how_to_verify = "Re-run deep analysis - heaviness weight should exceed " + f2(wt_thresh) + ".";
            out.push_back(std::move(r));
        }

        // heaviness_distortion_mismatch: outside an odds band around the genre norm
        // (+/-0.25 at 0.50; reachable for a near-clean or near-saturated norm - FIX-9).
        if (h.distortion > unitOddsCeiling(genre.heaviness_distortion_norm, kHeavinessDistortionOdds) ||
            h.distortion < unitOddsFloor(genre.heaviness_distortion_norm, kHeavinessDistortionOdds)) {
            const bool too_clean = h.distortion < genre.heaviness_distortion_norm;
            auto r = make("heaviness_distortion_mismatch", sev(h.distortion, genre.heaviness_distortion_norm), false,
                          ProcessingStage::MixBus, 4, true);
            r.what = "Spectral distortion score is " + f2(h.distortion) + " - genre norm is " +
                     f2(genre.heaviness_distortion_norm) + ".";
            r.why_helps =
                too_clean
                    ? "The mix lacks the harmonic grit and saturation that defines this genre's character. "
                      "Adding controlled distortion creates the perceived density and aggression listeners expect."
                    : "Excess high-frequency distortion (harsh harmonics, clipping artefacts) reduces "
                      "clarity and causes listener fatigue. Cleaning up the distortion profile improves "
                      "translation and perceived quality.";
            r.suggested_action =
                too_clean ? "Add subtle saturation or tape emulation to the mix bus or individual stem groups "
                            "to raise the distortion score toward " +
                                f2(genre.heaviness_distortion_norm) + "."
                          : "Reduce drive on mix bus saturation/clipping and check for any unintentional "
                            "inter-sample overs to bring the distortion score closer to " +
                                f2(genre.heaviness_distortion_norm) + ".";
            r.engineer_note = "This is a taste decision - apply in context of the full mix at "
                              "listening volume rather than solo.";
            r.how_to_verify = "Re-run deep analysis - distortion score should be within +/-0.25 "
                              "of " +
                              f2(genre.heaviness_distortion_norm) + ".";
            out.push_back(std::move(r));
        }

        return out;
    }
};

} // namespace khensu
