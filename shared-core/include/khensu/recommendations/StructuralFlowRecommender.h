#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <khensu/recommendations/RecommenderCommon.h>
#include <khensu/recommendations/TargetInterpolator.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace khensu {

class StructuralFlowRecommender {
  public:
    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& /*genre*/,
                                          const CreativeTarget& target) const
    {
        if (!result.has_python_analysis()) return {};

        std::vector<Recommendation> out;
        const auto& sf = result.python_data.structural_flow;

        using namespace khensu::recommendations::common;
        // 0-1 scores share one fixed full scale, so the same shortfall is the same
        // severity at every genre norm and target setting (FIX-10).
        auto sev = [](float measured, float threshold) {
            return severityFromError(measured, threshold, severity_scale::kUnitScore);
        };

        // A bigger desired chorus payoff raises the bar the arc must clear.
        const float threshold = TargetInterpolator::arcQualityThreshold(target);
        if (sf.arc_quality < threshold) {
            auto r = makeRecommendation(result.file_id, "structural_flow_poor", sev(sf.arc_quality, threshold), 0.70f,
                                        "", ProcessingStage::Arrangement, 1,
                                        /*isTasteDecision=*/true,
                                        /*isTargetRelative=*/false,
                                        /*safeToAutoApply=*/false, RecommendationType::Structural);
            // The screens show `what` and `suggested_action`; a blank one drew
            // an empty card (2026-11-18).
            r.what = "The track's dynamic arc is weak (score " + f2(sf.arc_quality) + ", target " + f2(threshold) +
                     "): tension and release may not build toward a satisfying peak.";
            r.why_helps = "A clear arc - build, peak and release - keeps listeners engaged across the whole track "
                          "and makes section changes feel purposeful.";
            r.suggested_action = "Review the arrangement: aim for 4-6 dB of perceived loudness contrast from verse "
                                 "to chorus, and automate volume, reverb sends and high-frequency content to build "
                                 "into transitions and release at drops or choruses.";
            r.how_to_verify = "Re-run analysis and check the Sections view: energy should rise into the peak "
                              "sections and fall after them.";
            out.push_back(std::move(r));
        }

        return out;
    }
};

} // namespace khensu
