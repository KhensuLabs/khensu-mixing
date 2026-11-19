// desktop-app/Source/analysis/ResultDerivedData.h
// Recommendations + MixReport + Scorecard for one final AnalysisResult,
// computed ONCE and shared (audit 2026-11-06 §10.3). AppController (persisted
// readiness) and DashboardScreen (tiles, verdict, scorecard) used to run the
// recommendation engine and the report builder separately on the message
// thread for every result. Both now go through
// AnalysisResultModel::derived<ResultDerivedData>(key, ...), so the second
// caller reuses the first one's work.
//
// The recommendations come from RecommendationEngine::recommendationsFor():
// for a multi-file batch that is the merged per-file list the aggregator
// built (CORE-01), not a re-run on the averaged aggregate.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/recommendations/MixReportBuilder.h>
#include <khensu/recommendations/MixScorecard.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/recommendations/ScoreWeightProfile.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/targets/GenreEntry.h>
#include <string>
#include <vector>

namespace khensu {

struct ResultDerivedData {
    std::vector<Recommendation> recommendations;
    MixReport report;
    MixScorecard scorecard;

    static ResultDerivedData compute(const AnalysisResult& r, const GenreEntry& genre, const CreativeTarget& target,
                                     const ScoreWeightProfile& weights)
    {
        ResultDerivedData d;
        d.recommendations = RecommendationEngine{}.recommendationsFor(r, genre, target);
        d.report = buildMixReport(d.recommendations, r, genre, target, weights);
        d.scorecard = buildMixScorecard(r, genre, d.recommendations, weights);
        return d;
    }

    /// Cache key: every input besides the result itself.
    static std::string keyFor(const std::string& sessionId, const GenreEntry& genre, const CreativeTarget& target,
                              const std::string& weightProfileId)
    {
        return sessionId + "|" + genre.id + "|" + target.primary_genre + "|" + target.playback_intent + "|" +
               weightProfileId;
    }
};

} // namespace khensu
