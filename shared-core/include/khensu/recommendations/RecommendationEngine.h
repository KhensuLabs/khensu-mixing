// shared-core/include/khensu/recommendations/RecommendationEngine.h
#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/MixReport.h>
#include <vector>

namespace khensu {

/// Runs every domain recommender against one AnalysisResult and returns the
/// merged, severity-sorted list. Fast-only results simply produce no deep
/// (hook/memorability/structural/heaviness) recommendations.
/// THREAD_CONTEXT(WorkerThread | AnyThread) - pure computation, no I/O.
class RecommendationEngine {
  public:
    /// FULL-3: the song-level findings whose models have no listener
    /// validation yet (hooks, memorability, heaviness, structure, the
    /// loudness arc - experimental in docs/manual.md) fired at moderate or
    /// worse on 25-59% of released tracks. Until a listener study backs them
    /// they are minor notes: their severity is capped here.
    static constexpr float kUnvalidatedSeverityCap = 35.0f;

    std::vector<Recommendation> generate(const AnalysisResult& result, const GenreEntry& genre,
                                         const CreativeTarget& target) const;

    /// The recommendations to display/score for `result` (CORE-01). For a
    /// multi-file batch aggregate (batch_results.size() > 1) this is the
    /// merged per-file list aggregateBatchResults built - each file's cards
    /// were generated from that file's real measurements and genre blend,
    /// whereas re-running generate() on the averaged aggregate invents a
    /// track nobody made. For a single-file result it is generate().
    std::vector<Recommendation> recommendationsFor(const AnalysisResult& result, const GenreEntry& genre,
                                                   const CreativeTarget& target) const;

    /// True when `result` is a multi-file batch aggregate.
    static bool isBatchAggregate(const AnalysisResult& result) noexcept { return result.batch_results.size() > 1; }

    MixReport generateReport(const AnalysisResult& result, const GenreEntry& genre, const CreativeTarget& target) const;
};

} // namespace khensu
