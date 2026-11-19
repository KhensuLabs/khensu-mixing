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

class ArtifactRecommender {
  public:
    static constexpr int kMinClippedFrames = 100;
    static constexpr float kNoiseFloorLimitDb = -50.0f;
    static constexpr float kNoiseFloorMusicDb = -40.0f;
    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& /*genre*/,
                                          const CreativeTarget& /*target*/) const
    {
        std::vector<Recommendation> out;
        const auto& a = result.artifacts;

        using namespace khensu::recommendations::common;
        auto make = [&](const char* code, float sevVal, ProcessingStage stg, int pos, bool taste = false) {
            return makeRecommendation(result.file_id, code, sevVal, 0.90f, "Master", stg, pos, taste);
        };

        // MEAS-4: a few clipped samples in a whole track are inaudible; on
        // released tracks a 100-frame minimum keeps false alarms at 1.4% and
        // still catches 97% of a 6 dB hard clip.
        if (a.has_clipping && a.clipping_frame_count >= kMinClippedFrames) {
            auto r = make("clipping_detected", 95.0f, ProcessingStage::MixChannel, 1);
            r.what = "Digital clipping detected - " + std::to_string(a.clipping_frame_count) + " clipped frames found.";
            r.why_helps = "Clipping creates harsh, audible distortion that cannot be cleanly "
                          "removed in post. It introduces inter-sample peaks that cause further "
                          "distortion when encoded to MP3/AAC, and loudness normalisation does not "
                          "remove it - the distortion is baked into every playback.";
            r.suggested_action = "Reduce the level of the clipping stage until peaks are below 0 dBFS. "
                                 "If clipping is intentional (limiter), lower the limiter input by 1-2 dB.";
            r.engineer_note = "Trace the clipping to its source - is it the limiter output, a "
                              "bus compressor, or a hot channel? Clip gain editing at the source "
                              "is better than post-processing the master.";
            r.how_to_verify = "Inspect the waveform - no sample should reach exactly 0 dBFS. "
                              "Re-measure after corrections; clipping frame count should be 0.";
            r.safe_to_auto_apply = false;
            out.push_back(std::move(r));
        }

        // MEAS-4: a floor read above -40 dBFS is the track's quietest music,
        // not noise (on released tracks the median such reading was -38 dBFS).
        // Reporting only -50..-40 cut false alarms from 29% to 11% and still
        // finds 76% of added -45 dBFS hiss.
        if (a.noise_floor_db > kNoiseFloorLimitDb && a.noise_floor_db <= kNoiseFloorMusicDb) {
            const float over = a.noise_floor_db - (-50.0f);
            auto r = make("noise_floor_high",
                          std::clamp(100.0f * std::abs(a.noise_floor_db + 50.0f) / std::max(std::abs(-50.0f), 1e-4f),
                                     0.0f, 100.0f),
                          ProcessingStage::Edit, 1);
            r.what =
                "Noise floor is " + f1(a.noise_floor_db) + " dBFS - " + f1(over) + " dB above the -50 dBFS threshold.";
            r.why_helps = "A high noise floor is audible as hiss or hum in quiet sections and "
                          "between phrases. It makes the mix sound amateur and unprofessional, "
                          "especially on headphones. Loudness normalisation can raise it further.";
            r.suggested_action = "Apply noise reduction to stems at the source (before the mix bus). "
                                 "Use a high-pass filter to roll off hum below 30 Hz. "
                                 "For broadband hiss, use spectral repair or noise reduction software.";
            r.engineer_note = "Identify which stem is the noisiest (record the mix in solo mode). "
                              "Noise reduction is best applied to individual tracks rather than the "
                              "master bus, where it smears transients.";
            r.how_to_verify = "Measure RMS in a truly silent passage - noise floor should be "
                              "below -50 dBFS.";
            out.push_back(std::move(r));
        }

        // Only a steady offset (MEAS-4): a whole-file mean over the threshold
        // fired on 14.6% of released tracks, half of them sub-sonic content
        // swinging both ways; steady offsets are 6.6%, and an injected 5 mV
        // (0.005) offset is still caught on 90% of tracks.
        const float dc = std::abs(a.dc_offset);
        if (dc >= AnalysisResult::ArtifactData::kDcOffsetThreshold && a.dc_offset_steady) {
            auto r = make("dc_offset",
                          severityFromError(dc, AnalysisResult::ArtifactData::kDcOffsetThreshold,
                                            severity_scale::kDcOffset),
                          ProcessingStage::Edit, 1);
            char mean[32];
            std::snprintf(mean, sizeof(mean), "%+.4f", static_cast<double>(a.dc_offset));
            r.what = std::string("DC offset: the waveform is centred at ") + mean +
                     " instead of zero (mean sample value).";
            r.why_helps = "A DC offset wastes headroom on one side of the waveform, makes edits and fades "
                          "click, and pushes compressors and limiters asymmetrically.";
            r.suggested_action = "Remove it with a DC-removal filter or a high-pass at 5-20 Hz on the "
                                 "source track (or the mix if the offset is on every track).";
            r.engineer_note = "DC usually comes from one track - a faulty interface input, an "
                              "asymmetric saturator or a sample library. Fix it there; a high-pass "
                              "on the master also works but hides which track carried it.";
            r.how_to_verify = "Re-measure: the mean sample value should read under 0.002, and the "
                              "waveform should sit centred on the zero line.";
            r.safe_to_auto_apply = false;
            out.push_back(std::move(r));
        }

        return out;
    }
};

} // namespace khensu
