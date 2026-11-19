#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace khensu {

/// Flags a flat loudness arc when the loudest and quietest detected sections
/// are within 2 LU of each other (>= 3 sections required for a meaningful arc judgement).
///
/// A flat arc suggests chorus/drop doesn't stand out energetically above verse/intro.
/// Marked as a taste_decision because flat arcs are intentional in ambient/drone genres.
class LoudnessArcRecommender {
  public:
    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& /*genre*/,
                                          const CreativeTarget& /*target*/) const
    {
        std::vector<Recommendation> out;
        if (!result.section_analysis.has_value()) return out;
        const auto& arc = result.section_analysis->loudness_arc;

        // Need >= 3 sections with valid LUFS before making a structural arc judgement.
        if (arc.section_count < 3) return out;
        // Only flag when the arc is genuinely flat; skip if delta is 0 (not computed).
        if (arc.chorus_verse_delta_lu >= 2.0f || arc.chorus_verse_delta_lu <= 0.0f) return out;

        const float raw_deficit = 2.0f - arc.chorus_verse_delta_lu; // 0-2 LU
        Recommendation r;
        r.id = "flat_loudness_arc_" + result.file_id;
        r.type = RecommendationType::Structural;
        r.severity = std::clamp(30.0f + (raw_deficit / 2.0f) * 40.0f, 30.0f, 70.0f);
        r.confidence = 0.65f;
        r.stem_or_bus = "Arrangement";
        r.is_target_relative = false;
        r.safe_to_auto_apply = false;
        r.stage = ProcessingStage::Arrangement;
        r.chain_position = 1;
        r.is_taste_decision = true; // flat arcs are intentional in ambient / drone genres
        r.what = "Loudness arc is flat: loudest section is only " +
                 std::to_string(arc.chorus_verse_delta_lu).substr(0, 4) + " LU above the quietest across " +
                 std::to_string(arc.section_count) +
                 " sections "
                 "(range: " +
                 std::to_string(arc.min_section_lufs).substr(0, 6) + " to " +
                 std::to_string(arc.max_section_lufs).substr(0, 6) +
                 " LUFS). "
                 "Chorus/drop may not have a clear energy advantage over verse.";
        r.why_helps = "A loudness arc of at least 2-4 LU between verse and chorus creates "
                      "a sense of build and release that sustains listener engagement. "
                      "A flat arc makes sections feel interchangeable and can reduce impact.";
        r.suggested_action = "Raise chorus/drop sections by 2-4 dB relative to verse using volume "
                             "automation. Alternatively, reduce verse arrangement density to create contrast.";
        r.engineer_note = "Per-section integrated LUFS from /analyze/sections endpoint. "
                          "Treat as a creative note - intentionally flat arcs are valid in ambient, "
                          "lo-fi, or drone contexts.";
        r.confidence_reason = "Loudness arc from section-level integrated LUFS. 65% confidence - "
                              "flat arcs may be stylistic in ambient / experimental genres.";
        r.trade_off = "Increasing loudness contrast requires arrangement changes or volume automation. "
                      "Creative revision, not a mixing fix.";
        r.how_to_verify = "Check section-level LUFS in a loudness metering tool. Chorus sections should "
                          "read at least 2 LUFS louder than the average verse. Listen for the chorus "
                          "feeling energetically bigger - not just texturally different.";
        r.alternative_strategies = {
            "Add layers (doubles, extra elements) to chorus sections to increase density.",
            "Use filter automation on verse elements (low-pass) to open space for the chorus.",
            "Sidechain the verse bassline to create rhythmic energy contrast.",
        };
        out.push_back(std::move(r));
        return out;
    }
};

} // namespace khensu
