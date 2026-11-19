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

class StereoRecommender {
  public:
    /// True for material with no stereo image to judge (AUDIT-055): a mono
    /// file, or a stereo file carrying identical channels (dual mono). Width
    /// advice on these is meaningless - "widen the mix" on a mono stem.
    /// channel_count 0 means the layout was not measured: not assumed mono.
    static bool isMonoSource(const AnalysisResult& result) noexcept
    {
        const int ch = result.channel_layout.channel_count;
        if (ch == 1) return true;
        return ch >= 2 && result.stereo.width <= 1e-4f && result.stereo.correlation >= 0.9999f;
    }

    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& genre,
                                          const CreativeTarget& /*target*/) const
    {
        std::vector<Recommendation> out;
        const auto& s = result.stereo;

        using namespace khensu::recommendations::common;
        auto make = [&](const char* code, float sevVal, ProcessingStage stg, int pos, bool taste = false) {
            return makeRecommendation(result.file_id, code, sevVal, 0.80f, "Master", stg, pos, taste);
        };

        const bool mono = isMonoSource(result);
        if (!mono && s.width < genre.stereo_width_min) {
            auto r = make("stereo_too_narrow", severityFromError(s.width, genre.stereo_width_min, severity_scale::kWidth), ProcessingStage::MixBus, 6);
            r.what =
                "Stereo width is " + f2(s.width) + " - below the " + f2(genre.stereo_width_min) + " genre minimum.";
            r.why_helps = "A narrow stereo image sounds thin and flat compared to genre references. "
                          "Widening creates the immersive, spatial feel the genre's listeners expect "
                          "without compromising mono compatibility when done correctly.";
            r.suggested_action = "Widen the stereo image using mid-side processing: boost the Side channel "
                                 "by 2-4 dB or apply a subtle Haas delay (6-12 ms) on the side path.";
            r.engineer_note = "Check correlation after widening - it should stay above +0.3 to remain "
                              "mono-compatible. Avoid widening the low end below 200 Hz.";
            r.how_to_verify = "Re-check width measurement - target range is " + f2(genre.stereo_width_min) + "-" +
                              f2(genre.stereo_width_max) + ".";
            out.push_back(std::move(r));
        }

        if (!mono && s.width > genre.stereo_width_max) {
            auto r = make("stereo_too_wide", severityFromError(s.width, genre.stereo_width_max, severity_scale::kWidth), ProcessingStage::MixBus, 6);
            r.what =
                "Stereo width is " + f2(s.width) + " - above the " + f2(genre.stereo_width_max) + " genre maximum.";
            r.why_helps = "Excessive width causes phase issues in mono (earbuds, phone speakers, "
                          "club systems in fold-to-mono mode) and sounds unnatural and fatiguing "
                          "on headphones. Reducing width improves translation across all playback.";
            r.suggested_action = "Narrow the stereo image using mid-side processing: attenuate the Side "
                                 "channel to bring width within the " +
                                 f2(genre.stereo_width_max) + " ceiling.";
            r.engineer_note = "Check if the width comes from a specific stem (synth pads, reverb returns). "
                              "Narrowing at the source is better than treating the whole master.";
            r.how_to_verify = "Re-check width - target range is " + f2(genre.stereo_width_min) + "-" +
                              f2(genre.stereo_width_max) +
                              ". Verify mono sum sounds identical to stereo (no comb filtering).";
            out.push_back(std::move(r));
        }

        // Same threshold as the analyzer's polarity flag and the "phase clean"
        // strength note (CORE-06), so a red and a green card never coexist.
        if (s.correlation < AnalysisResult::PhaseData::kPhaseIssueCorrelation) {
            const float severityVal = std::clamp(std::abs(s.correlation) * 100.0f, 0.0f, 100.0f);
            auto r = make("phase_issue", severityVal, ProcessingStage::MixChannel, 2);
            r.what = "Stereo correlation is " + f2(s.correlation) + " - negative values indicate phase cancellation.";
            r.why_helps = "Negative correlation means the left and right channels partially cancel "
                          "each other in mono. This causes bass and low-mids to disappear on "
                          "mono systems (phones, Bluetooth speakers, club PA in mono mode). "
                          "Fixing phase issues ensures the mix sounds full on every system.";
            r.suggested_action = "Check each stereo element in mono: identify the source of the "
                                 "cancellation (usually reverb, chorus, or a misaligned duplicate "
                                 "track) and flip polarity or delay-align to restore correlation above 0.";
            r.engineer_note = "Use a correlation meter and A/B between stereo and mono fold-down. "
                              "The problem is almost always a specific stem - solo each bus in mono to locate.";
            r.how_to_verify = "Correlation should be above 0 in the final master. Confirm by folding "
                              "to mono - the level should not drop by more than 3 dB.";
            r.suggested_parameters["correlation_target"] = 0.0f;
            out.push_back(std::move(r));
        }

        return out;
    }
};

} // namespace khensu
