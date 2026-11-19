// shared-core/include/khensu/recommendations/SectionFlowRecommender.h
// THREAD_CONTEXT(WorkerThread | AnyThread) - pure computation, no I/O.
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

/// Generates section-level mix recommendations by comparing per-section LUFS
/// values against each other and against genre loudness targets.
/// Catches: flat loudness arc (over-compressed), excessive arc (jarring jumps),
/// and an arrangement that never reaches the genre loudness floor.
class SectionFlowRecommender {
  public:
    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& genre,
                                          const CreativeTarget& /*target*/) const
    {
        std::vector<Recommendation> out;
        if (!result.section_analysis.has_value()) return out;
        const auto& sa = *result.section_analysis;
        if (sa.sections.size() < 2) return out;

        // Collect sections with valid LUFS (0.0f means not computed)
        std::vector<const SectionData*> validSections;
        for (const auto& s : sa.sections)
            if (s.lufs_integrated < -0.5f) validSections.push_back(&s);
        if (validSections.size() < 2) return out;

        using khensu::recommendations::common::f1;

        // Find loudest and quietest valid sections
        float maxLufs = validSections[0]->lufs_integrated;
        float minLufs = validSections[0]->lufs_integrated;
        const SectionData* loudestSec = validSections[0];
        const SectionData* quietestSec = validSections[0];
        for (auto* s : validSections) {
            if (s->lufs_integrated > maxLufs) {
                maxLufs = s->lufs_integrated;
                loudestSec = s;
            }
            if (s->lufs_integrated < minLufs) {
                minLufs = s->lufs_integrated;
                quietestSec = s;
            }
        }

        const float arc = maxLufs - minLufs; // positive = dynamic contrast (LU)

        // --- Rec 1: Too little arc (over-compressed arrangement) ---
        if (arc < 2.0f && validSections.size() >= 3) {
            Recommendation r;
            r.id = "section_arc_too_flat_" + result.file_id;
            r.type = RecommendationType::Technical;
            r.severity = std::clamp((2.0f - arc) * 20.0f, 0.0f, 100.0f);
            r.confidence = 0.75f;
            r.confidence_reason =
                "Measured from " + f1(static_cast<float>(validSections.size())) + " sections with valid LUFS.";
            r.stem_or_bus = "Master";
            r.stage = ProcessingStage::MixBus;
            r.chain_position = 5;
            r.section = "all";
            r.what = "Loudness arc across sections is only " + f1(arc) +
                     " LU -- sections sound uniformly loud with no dynamic contrast.";
            r.most_likely_cause = "Heavy bus compression or limiter gain riding flattening the arrangement energy.";
            r.why_helps = "A flat loudness arc means listeners cannot distinguish verses from "
                          "choruses by feel alone. Dynamic contrast (3-6 LU between quiet and loud "
                          "sections) creates emotional impact and makes drops and choruses hit harder.";
            r.suggested_action = "Use clip gain automation or fader rides to create at least 3 LU of "
                                 "contrast between quietest and loudest sections.";
            r.suggested_parameters["arc_target_lu"] = 4.0f;
            r.engineer_note = "Check verse vs. chorus level difference. If the compressor is flattening "
                              "the arc, try easing the attack or ratio during verse sections, or automate "
                              "the makeup gain.";
            r.how_to_verify = "Section LUFS arc should be >= 3 LU after automation adjustments.";
            r.trade_off = "Wider arc may slightly reduce streaming loudness normalisation scores.";
            out.push_back(std::move(r));
        }

        // --- Rec 2: Too much arc (jarring jumps) ---
        if (arc > 9.0f) {
            Recommendation r;
            r.id = "section_arc_too_wide_" + result.file_id;
            r.type = RecommendationType::Technical;
            r.severity = std::clamp((arc - 9.0f) * 8.0f, 0.0f, 100.0f);
            r.confidence = 0.70f;
            r.confidence_reason = "Arc of " + f1(arc) + " LU is above the 9 LU professional threshold.";
            r.stem_or_bus = "Master";
            r.stage = ProcessingStage::MixBus;
            r.chain_position = 5;
            r.section = loudestSec->label + " / " + quietestSec->label;
            r.what = "Loudness arc is " + f1(arc) + " LU -- the gap between " + loudestSec->label + " (" + f1(maxLufs) +
                     " LUFS) and " + quietestSec->label + " (" + f1(minLufs) + " LUFS) may feel jarring.";
            r.most_likely_cause = "Extreme fader automation or unbalanced section production energy.";
            r.why_helps = "Very large loudness jumps can feel abrupt and fatiguing, especially on "
                          "streaming where normalization may amplify the contrast further.";
            r.suggested_action = "Reduce gain on " + loudestSec->label + " or raise " + quietestSec->label + " by " +
                                 f1((arc - 7.0f) / 2.0f) + " dB each.";
            r.engineer_note = "A 4-8 LU arc is generally the sweet spot for most genres. Above 9 LU "
                              "risks listener fatigue on first plays.";
            r.how_to_verify = "Section LUFS arc should be under 9 LU across all sections.";
            // Anchors the Dashboard Timeline lane marker at the quieter of
            // the two sections driving this recommendation -- the one that
            // actually needs the fader/automation move -- rather than
            // spanning both disjoint sections into one misleadingly wide
            // range. r.section already names both by label for the text.
            r.time_range = TimeRange{quietestSec->start_sec, quietestSec->end_sec};
            out.push_back(std::move(r));
        }

        // --- Rec 3: Even the loudest section is below genre floor ---
        if (genre.lufs_range_min < -0.1f && maxLufs < genre.lufs_range_min - 1.0f) {
            Recommendation r;
            r.id = "section_peak_too_quiet_" + result.file_id;
            r.type = RecommendationType::Technical;
            r.severity = std::clamp((genre.lufs_range_min - maxLufs) * 10.0f, 10.0f, 80.0f);
            r.confidence = 0.80f;
            r.confidence_reason =
                "Genre floor: " + f1(genre.lufs_range_min) + " LUFS. Loudest section: " + f1(maxLufs) + " LUFS.";
            r.stem_or_bus = "Master";
            r.stage = ProcessingStage::Master;
            r.chain_position = 8;
            r.section = loudestSec->label;
            r.what = "Even the loudest section (" + loudestSec->label + ") reaches only " + f1(maxLufs) +
                     " LUFS -- below the genre floor of " + f1(genre.lufs_range_min) + " LUFS.";
            r.most_likely_cause = "Insufficient makeup gain after limiting, or limiter ceiling set too conservatively.";
            r.why_helps = "If the loudest section is below the genre loudness floor, the overall mix "
                          "will sound quiet and lack energy against reference tracks on streaming platforms.";
            r.suggested_action = "Raise the master output level -- the loudest section needs at least " +
                                 f1(genre.lufs_range_min - maxLufs) + " dB more gain before the limiter.";
            r.suggested_parameters["gain_db"] = genre.lufs_range_min - maxLufs;
            r.engineer_note = "Check if heavy limiting or aggressive gain staging is suppressing the peak. "
                              "Check the limiter's output ceiling and makeup gain.";
            r.how_to_verify = "Loudest section should reach at least " + f1(genre.lufs_range_min) + " LUFS.";
            r.time_range = TimeRange{loudestSec->start_sec, loudestSec->end_sec};
            out.push_back(std::move(r));
        }

        return out;
    }
};

} // namespace khensu
