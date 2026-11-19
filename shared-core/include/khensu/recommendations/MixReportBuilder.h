#pragma once
#include <khensu/recommendations/MixReport.h>
#include <khensu/recommendations/ScoreWeightProfile.h>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <vector>

namespace khensu {

/// Build a complete MixReport from raw recommendations and analysis data.
/// Applies: confidence_threshold filtering, contradiction detection,
/// chain ordering, mastering readiness scoring, and no_action_needed evidence.
/// `weights` defaults to the stock category caps; pass a genre-loaded
/// ScoreWeightProfile to apply a user-tuned weighting profile.
MixReport buildMixReport(std::vector<Recommendation> recommendations, const AnalysisResult& result,
                         const GenreEntry& genre, const CreativeTarget& target, const ScoreWeightProfile& weights = {});

} // namespace khensu
