#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <khensu/recommendations/RecommenderCommon.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace khensu {

class DynamicsRecommender {
  public:
    /// Genre-relative crest tolerance: within +/- this of the target is on-character.
    static constexpr float kCrestToleranceDb = 3.0f;

    /// Crest factor below which a mix counts as over-compressed FOR ITS
    /// GENRE: the lower edge of the genre's crest band (AUDIT-070). A fixed
    /// 6 dB floor flagged on-target hyperpop (target 6 dB) as crushed.
    static float overCompressedFloorDb(const GenreEntry& genre) noexcept
    {
        return genre.crest_factor_target_db - kCrestToleranceDb;
    }

    /// under_compressed fires when the gated LRA passes this multiple of the
    /// genre's LRA ceiling. One definition for the recommender, the Mix
    /// Scorecard and the evidence trail.
    static constexpr float kUnderCompressedLraRatio = 1.5f;
    static float underCompressedLraThreshold(const GenreEntry& genre) noexcept
    {
        return genre.lra_range_max * kUnderCompressedLraRatio;
    }

    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& genre,
                                          const CreativeTarget& /*target*/) const
    {
        std::vector<Recommendation> out;
        const auto& d = result.dynamics;

        using namespace khensu::recommendations::common;
        auto make = [&](const char* code, float sevVal, ProcessingStage stg, int pos, bool taste = false) {
            return makeRecommendation(result.file_id, code, sevVal, 0.80f, "Master", stg, pos, taste);
        };

        // over_compressed: crest below the genre band's lower edge
        // (target - tolerance). It replaces the old crest_below_genre_target,
        // which fired at exactly the same point and would only duplicate it.
        const float crestFloor = overCompressedFloorDb(genre);
        const std::string genreLabel = genre.name.empty() ? std::string("genre") : genre.name;
        const bool overCompressed = d.crest_factor_db < crestFloor;
        if (overCompressed) {
            auto r = make("over_compressed", severityFromError(d.crest_factor_db, crestFloor, severity_scale::kCrest), ProcessingStage::MixBus, 5);
            r.what = "Crest factor is " + f1(d.crest_factor_db) + " dB - below the " + f1(crestFloor) + " dB floor for " +
                     genreLabel + " (target " + f1(genre.crest_factor_target_db) + " dB), indicating over-compression.";
            r.why_helps = "A crest factor this far under the genre's target means the peaks are crushed toward "
                          "the RMS level, removing the transient impact that gives music its "
                          "energy and punch. The mix sounds squashed, fatiguing, and lifeless. "
                          "Restoring transient headroom makes it more engaging at any volume.";
            r.suggested_action = "Restore transients: decrease compressor ratio to 1.5:1 and raise "
                                 "threshold to -10 dB.";
            r.engineer_note = "Check every stage - bus compressor, limiter, and individual track "
                              "compressors. Often the limiter is set too aggressively. "
                              "Try raising the limiter threshold by 1-2 dB and reducing master gain.";
            r.how_to_verify = "Re-measure crest factor - should be above " + f1(crestFloor) + " dB. "
                              "The mix should sound punchy and dynamic at moderate listening levels.";
            r.suggested_parameters["comp_ratio"] = 1.5f;
            r.suggested_parameters["comp_threshold_db"] = -10.0f;
            r.suggested_parameters["comp_makeup_gain_db"] = 0.0f;
            out.push_back(std::move(r));
        }

        // under_compressed: gated LRA > lra_range_max x 1.5. Compared against
        // the measured LRA (same EBU Tech 3342 definition as the genre
        // target) - the old ungated 100 ms P95-P10 spread is far larger and
        // fired on any song with a break or fade. Never alongside
        // over_compressed: "lower the ratio" next to "add glue compression"
        // is contradictory advice for one mix.
        const float under_thresh = underCompressedLraThreshold(genre);
        const auto& loud = result.loudness;
        if (!overCompressed && loud.lra_measured && loud.lra > under_thresh) {
            const float ratio = std::clamp(genre.compression_ratio_typical, 2.0f, 4.0f);
            auto r = make("under_compressed", severityFromError(loud.lra, under_thresh, severity_scale::kLra), ProcessingStage::MixBus, 5);
            r.what = "Loudness range is " + f1(loud.lra) + " LU - above the " + f1(under_thresh) +
                     " LU ceiling, "
                     "indicating insufficient cohesion/glue compression.";
            r.why_helps = "Excessive dynamic range sounds uncontrolled and loose compared to "
                          "genre references. A gentle glue compressor brings all elements "
                          "together, creating the sense of a cohesive mix rather than individual "
                          "tracks playing simultaneously.";
            r.suggested_action = "Glue the mix: add a " + f1(ratio) +
                                 ":1 compressor on the mix bus "
                                 "with a -18 dB threshold, slow attack (30-50 ms), and auto release.";
            r.engineer_note = "Use subtle settings - a well-placed glue compressor should be almost "
                              "inaudible when bypassed but make the mix feel tighter when engaged. "
                              "2-3 dB of GR at the loudest moments is the sweet spot.";
            r.how_to_verify = "Loudness range should be below " + f1(under_thresh) +
                              " LU after the adjustment. The mix should sound more cohesive.";
            r.suggested_parameters["comp_ratio"] = ratio;
            r.suggested_parameters["comp_threshold_db"] = -18.0f;
            r.suggested_parameters["comp_makeup_gain_db"] = 2.0f;
            out.push_back(std::move(r));
        }

        // --- Crest factor above the genre target ---
        // The low side is over_compressed above (same genre band). A
        // wall-of-sound shoegaze mix and a very dynamic ambient mix can both
        // be "correct" at very different crest factors, so the band is
        // per-genre, not absolute.
        const float crestDiff = d.crest_factor_db - genre.crest_factor_target_db;
        if (crestDiff > kCrestToleranceDb) {
            auto r = make("crest_above_genre_target",
                          // CORE-05: from the tolerance edge, not the target.
                          severityFromError(d.crest_factor_db, genre.crest_factor_target_db + kCrestToleranceDb,
                                            severity_scale::kCrest),
                          ProcessingStage::MixBus, 5, true);
            r.what = "Crest factor is " + f1(d.crest_factor_db) + " dB - the " + genreLabel + " target is " +
                     f1(genre.crest_factor_target_db) + " dB.";
            r.why_helps = "Crest factor (peak-to-RMS distance) is a big part of a genre's dynamic "
                          "character - a dense, heavily-limited genre reads wrong with too much "
                          "headroom left in, and a dynamic/airy genre reads wrong when squashed flat.";
            // Peak control, not slower bus compression: crest is a short-term
            // peak-to-RMS measure, so a limiter or clipper lowers it without
            // flattening the section-to-section dynamics (LRA). Bus
            // compression advice here contradicted lra_too_compressed on the
            // same song (real-song check 2026-11-14).
            r.suggested_action = "Shave the short peaks with a fast limiter or clipper (1-3 dB of gain reduction "
                                 "on the loudest hits) to bring crest factor down toward " +
                                 f1(genre.crest_factor_target_db) + " dB.";
            r.engineer_note = "This is a genre-character check, not a loudness-safety one - "
                              "headroom left in is never a technical fault on its own. Peak control "
                              "leaves the song's loud-to-quiet contrast (LRA) alone; slower bus "
                              "compression would flatten it.";
            r.how_to_verify = "Re-measure crest factor - target within +/-" + f1(kCrestToleranceDb) + " dB of " +
                              f1(genre.crest_factor_target_db) + ".";
            out.push_back(std::move(r));
        }

        return out;
    }
};

} // namespace khensu
