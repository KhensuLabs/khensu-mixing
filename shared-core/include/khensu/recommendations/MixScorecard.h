#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/ScoreWeightProfile.h>
#include <string>
#include <vector>

namespace khensu {

/// A single weighted category in the Mix Scorecard (e.g. "Dynamics & Compression").
/// Weights are renormalised across only the categories that had enough data to
/// score (see MixScorecard::categories_analyzed) so the overall score never
/// silently double-counts or drops weight when extended analysis is missing.
struct ScorecardCategory {
    std::string id;                                ///< stable key, e.g. "dynamics"
    std::string name;                              ///< display name, e.g. "Dynamics & Compression"
    float weight = 0.0f;                           ///< 0-1, this category's share of the overall score
    float score = 0.0f;                            ///< 0-100
    bool available = true;                         ///< false if the backing data wasn't present
    std::vector<ScorecardCategory> sub_categories; ///< e.g. Technical Integrity -> Clipping / Noise

    static constexpr const char* kTagStrongest = "Strongest";
    static constexpr const char* kTagImprove = "Improve";
    std::string tag; ///< kTagStrongest / kTagImprove / "" - set by buildMixScorecard on the full set
};

/// Full Mix Scorecard: an overall 0-100 score with a named skill-tier band,
/// plus a weighted breakdown across seven pro-audio categories. Every number
/// here is derived from fields the analyzers/recommenders already compute -
/// this is a presentation/aggregation layer, not a new analysis pass.
struct MixScorecard {
    float overall_score = 0.0f; ///< 0-100, weighted heuristic technical/genre-alignment
    /// VIZ-P3: the weighted score before the open-finding cap. A severe finding
    /// caps overall_score at kSevereCap and a moderate one at kModerateCap, so
    /// "Near Target" never sits beside a severe issue; tier_audience then says
    /// why ("Capped by 2 severe issues - 86 without them").
    float uncapped_score = 0.0f;
    int severe_open = 0;   ///< non-strength findings at severity >= kSevereSeverity
    int moderate_open = 0; ///< at kModerateSeverity .. kSevereSeverity
    bool capped() const noexcept { return overall_score + 0.5f < uncapped_score; }
    static constexpr float kSevereSeverity = 66.0f;   ///< the Mix Notes / VerdictModel buckets
    static constexpr float kModerateSeverity = 33.0f;
    static constexpr float kSevereCap = 64.0f;   ///< below "Minor Issues" (65)
    static constexpr float kModerateCap = 89.0f; ///< below "Meets Targets" (90)
    std::string tier_label;     ///< bounded readiness band (kTier* below) - NOT a quality claim
    std::string tier_audience;  ///< bounded technical descriptor of the band (not an audience/quality claim)

    std::vector<ScorecardCategory> categories;

    /// 0-1: fraction of the 7 categories that had enough backing data to score
    /// (extended-analysis categories like Frequency Masking/FX & Processing
    /// need /analyze/extended data; if that hasn't run, they're excluded and
    /// this drops below 1.0).
    float completeness = 1.0f;

    // Bounded readiness bands (heuristic technical/genre alignment). These are
    // NOT quality judgments and deliberately avoid "professional/hit-record/
    // world-class" claims, which no blinded expert-evaluation corpus supports
    // (2026-07-15 safety audit, P0). They describe distance to the selected
    // genre's technical targets, nothing more.
    static constexpr const char* kTierEarlyStage = "Rough";
    static constexpr const char* kTierDeveloping = "Early Draft";
    static constexpr const char* kTierDemoLevel = "Needs Work";
    static constexpr const char* kTierSkilled = "Minor Issues";
    static constexpr const char* kTierHighlySkilled = "Near Target";
    static constexpr const char* kTierExceptional = "Meets Targets";
};

/// Builds the Mix Scorecard from an already-computed AnalysisResult and its
/// recommendations (pass the same vector produced by
/// RecommendationEngine::generate() - this does not run recommenders again).
/// `weights` defaults to the stock nominal category weights; pass a
/// genre-loaded ScoreWeightProfile to apply a user-tuned weighting profile.
MixScorecard buildMixScorecard(const AnalysisResult& result, const GenreEntry& genre,
                               const std::vector<Recommendation>& recommendations,
                               const ScoreWeightProfile& weights = {});

} // namespace khensu
