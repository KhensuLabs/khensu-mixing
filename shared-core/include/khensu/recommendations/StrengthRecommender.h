#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <khensu/recommendations/RecommenderCommon.h>
#include <khensu/recommendations/DynamicsRecommender.h>
#include <khensu/recommendations/LoudnessRecommender.h>
#include <khensu/recommendations/StereoRecommender.h>
#include <string>
#include <vector>

namespace khensu {

/// Emits GREEN notes for things the mix does RIGHT (spec §4).
///
/// Every other recommender only speaks when something is wrong, so a clean
/// mix produced an empty Mix Notes list that reads as "no information" rather
/// than "you nailed these". Strengths are computed from the same measured
/// values and genre targets as the issue recommenders - they are earned, not
/// decorative. severity is always 0 (they sort to the bottom) and type is
/// RecommendationType::Strength (rendered green by RecommendationCard).
class StrengthRecommender {
  public:
    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& genre,
                                          const CreativeTarget& /*target*/) const
    {
        std::vector<Recommendation> out;
        using namespace khensu::recommendations::common;

        auto make = [&](const char* code, const std::string& what, const std::string& note) {
            auto r = makeRecommendation(result.file_id, code, 0.0f, 0.90f, "Master", ProcessingStage::Master, 99, false,
                                        false, false, RecommendationType::Strength);
            r.what = what;
            r.why_helps = "Confirmed strengths tell you what NOT to touch - protecting "
                          "what already works is half of finishing a mix.";
            r.suggested_action = "Nothing to do - keep it this way through further revisions.";
            r.engineer_note = note;
            r.how_to_verify = "Re-check this metric after any further processing to make "
                              "sure later changes didn't undo it.";
            return r;
        };

        const auto& l = result.loudness;
        // A silent file's -70/-200 sentinels are not levels (AUDIT-069):
        // "true peak is safe" on silence is not an earned strength.
        const bool measurable = l.lufs_integrated > LoudnessRecommender::kSilentLufs;

        if (measurable && l.lufs_integrated >= genre.lufs_range_min && l.lufs_integrated <= genre.lufs_range_max) {
            out.push_back(make("strength_lufs_on_target",
                               "Integrated loudness (" + f1(l.lufs_integrated) + " LUFS) sits inside the " +
                                   f1(genre.lufs_range_min) + " to " + f1(genre.lufs_range_max) + " LUFS genre window.",
                               "Streaming normalization will neither squash nor starve this master - "
                               "it plays back at the level you mixed it."));
        }

        if (measurable && l.true_peak_dbtp <= genre.true_peak_max) {
            out.push_back(make("strength_true_peak_safe",
                               "True peak (" + f1(l.true_peak_dbtp) + " dBTP) is at or under the " +
                                   f1(genre.true_peak_max) + " dBTP ceiling.",
                               "This leaves the usual headroom for lossy encoding, which lowers the risk "
                               "of inter-sample clipping after encode - it is not a guarantee, so check "
                               "the encoded file if the master is very dense."));
        }

        if (l.lra_measured && l.lra >= genre.lra_range_min && l.lra <= genre.lra_range_max) {
            out.push_back(make("strength_dynamics_in_pocket",
                               "Loudness range (" + f1(l.lra) + " LU) is inside the genre's " +
                                   f1(genre.lra_range_min) + " to " + f1(genre.lra_range_max) + " LU pocket.",
                               "The mix breathes: sections contrast without the quiet parts falling away. "
                               "Resist the urge to add more bus compression."));
        }

        // CORE-06: "phase clean" is only earned by stereo, non-silent material
        // whose correlation clears the SAME threshold the phase_issue card
        // uses - it used to fire on mono and silent sources, and next to a red
        // phase card for correlations between -0.3 and 0.
        const bool judgeableStereo = measurable && result.channel_layout.channel_count >= 2 &&
                                     !StereoRecommender::isMonoSource(result);
        if (judgeableStereo && !result.phase.has_polarity_issue &&
            result.stereo.correlation >= AnalysisResult::PhaseData::kPhaseIssueCorrelation) {
            out.push_back(make("strength_phase_clean",
                               "No polarity/phase-cancellation issues detected between channels.",
                               "The stereo image sums to mono without hollowing out - club systems and "
                               "phone speakers get the same mix."));
        }

        // Crest factor (not dynamic_range_db, which the text never described)
        // against the same genre floor over_compressed uses (AUDIT-070). Not
        // above the band either: crest_above_genre_target fires there, and a
        // "healthy headroom" strength beside it read as a contradiction on a
        // real song (2026-11-14).
        if (const float crestFloor = DynamicsRecommender::overCompressedFloorDb(genre);
            measurable && result.dynamics.crest_factor_db >= crestFloor &&
            result.dynamics.crest_factor_db - genre.crest_factor_target_db <= DynamicsRecommender::kCrestToleranceDb) {
            out.push_back(make("strength_transient_headroom",
                               "Crest factor is " + f1(result.dynamics.crest_factor_db) +
                                   " dB - healthy transient headroom for the genre (>= " + f1(crestFloor) + " dB).",
                               "Drums still punch through the limiter. This is the number that usually "
                               "dies first in a loudness war; yours survived."));
        }

        return out;
    }
};

} // namespace khensu
