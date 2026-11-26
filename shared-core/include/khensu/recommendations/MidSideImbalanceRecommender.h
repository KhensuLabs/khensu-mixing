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

/// Generates recommendations from the M/S frequency imbalance data produced
/// by the Python /analyze/extended endpoint (MidSideImbalanceData).
///
/// Two findings are flagged independently:
///  1. Sub-bass too wide  - side/mid below 100 Hz > 0.70 → mono bass risk (TUNE-5)
///  2. HF brightness      - side/mid above 4 kHz > 1.0 (FULL-3)
class MidSideImbalanceRecommender {
  public:
    static constexpr float kSideBrightRatio = 1.0f;
    /// TUNE-5: at 0.30 the gate fired on 9.6% of released tracks (209
    /// lossless originals) - all of them at moderate or worse. A sub moved to
    /// full width reads 0.79 or more on 95% of tracks; at 0.70 the finding
    /// fires on 3.8% of releases and still catches every injected wide sub.
    static constexpr float kSideHeavyRatio = 0.70f;
    /// STU-18: the crossover the fix monos the low end below - the
    /// BassMonoMaker's default and the plugin's kSubBassMonoHz
    /// (RecommendationDspMapper.h). The finding measures the side below
    /// 100 Hz and gives no frequency of its own, so the fix takes the whole
    /// measured band and the slope above it; the corpus's injected wide sub
    /// (scripts/measure_genre_norms.py, "sub_wide") is widened below 120 Hz too.
    static constexpr float kBassMonoHz = 120.0f;
    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& /*genre*/,
                                          const CreativeTarget& /*target*/) const
    {
        std::vector<Recommendation> out;
        if (!result.mid_side_imbalance.has_value()) return out;
        const auto& ms = *result.mid_side_imbalance;

        // --- Sub-bass too wide ---
        if (ms.side_heavy_ratio > kSideHeavyRatio) {
            const float raw_sev = (ms.side_heavy_ratio - kSideHeavyRatio) / 0.30f; // 0-1 above threshold
            Recommendation r;
            r.id = "sub_bass_too_wide_" + result.file_id;
            r.type = RecommendationType::Technical;
            r.severity = std::clamp(40.0f + raw_sev * 50.0f, 40.0f, 90.0f);
            r.confidence = 0.80f;
            r.stem_or_bus = "MixBus";
            r.is_target_relative = false;
            r.safe_to_auto_apply = false;
            r.stage = ProcessingStage::MixBus;
            r.chain_position = 6;
            r.is_taste_decision = false;
            // STU-18: renders as a BassMonoMaker (buildRecommendationChain)
            // and sets the plugin's bass mono (RecommendationDspMapper).
            r.suggested_parameters["bass_mono_hz"] = kBassMonoHz;
            r.what = "Sub-bass is too wide: side/mid energy ratio below 100 Hz is " +
                     std::to_string(ms.side_heavy_ratio).substr(0, 4) +
                     " (threshold: 0.70). "
                     "Low-frequency content should be anchored in the mid (mono) channel.";
            r.why_helps = "Sub-bass with a high side/mid ratio cancels when summed to mono, "
                          "reducing bass impact on club systems, phone speakers, and mono playback.";
            r.suggested_action = "Use M/S EQ to high-pass the side (S) channel at 80-120 Hz, "
                                 "keeping sub energy in the mid (mono) channel only.";
            r.engineer_note = "Side/mid sub ratio: " + std::to_string(ms.side_heavy_ratio).substr(0, 4) +
                              ". Target below 0.30 for reliable mono compatibility in the sub band.";
            r.confidence_reason = "M/S sub ratio computed from FFT energy below 100 Hz; deterministic.";
            r.trade_off = "Mono-izing the low end may reduce perceived bass width on headphones "
                          "slightly, but is standard practice for mono-compatible masters.";
            r.how_to_verify = "Sum to mono and compare bass level. Bass should lose < 3 dB on mono fold-down. "
                              "A sub-band correlation meter reading above 0.7 confirms safe mono compatibility.";
            r.alternative_strategies = {
                "Apply a dedicated bass-mono plugin (Brauer Motion, SPAN Plus correlation view).",
                "High-pass the side (S) channel at 100 Hz using M/S processing on the master bus.",
            };
            out.push_back(std::move(r));
        }

        // --- M/S brightness imbalance ---
        // FULL-3: the side brighter than the mid above 4 kHz. The old gate
        // (ratio > 1.5 and a composite severity > 0.5) sat above every
        // released track and caught 23% of a +10 dB side boost; at 1.0 it
        // fires on 3% of released tracks and catches about half.
        if (ms.side_bright_ratio > kSideBrightRatio) {
            Recommendation r;
            r.id = "ms_brightness_imbalance_" + result.file_id;
            r.type = RecommendationType::Technical;
            r.severity = std::clamp(20.0f + (ms.side_bright_ratio - kSideBrightRatio) * 80.0f, 20.0f, 80.0f);
            r.confidence = 0.70f;
            r.stem_or_bus = "MixBus";
            r.is_target_relative = false;
            r.safe_to_auto_apply = false;
            r.stage = ProcessingStage::MixBus;
            r.chain_position = 6;
            r.is_taste_decision = false;
            r.what = "Side channel is significantly brighter than the mid above 4 kHz "
                     "(side/mid ratio: " +
                     std::to_string(ms.side_bright_ratio).substr(0, 4) +
                     "; severity: " + std::to_string(ms.imbalance_severity).substr(0, 4) + ").";
            r.why_helps = "Excessive side-channel brightness above 4 kHz creates an unnatural "
                          "stereo field that fatigues on headphones and collapses oddly in mono.";
            r.suggested_action = "Apply a high-shelf cut on the side channel above 6-8 kHz. "
                                 "Aim to bring the side/mid HF ratio below 1.30.";
            r.engineer_note = "Side/mid ratio above 4 kHz: " + std::to_string(ms.side_bright_ratio).substr(0, 4) +
                              ". Imbalance severity: " + std::to_string(ms.imbalance_severity).substr(0, 4) + ".";
            r.confidence_reason = "Side/mid spectral ratio computed from FFT above 4 kHz; deterministic.";
            r.trade_off = "Cutting the side channel's high frequencies may reduce perceived "
                          "air and spaciousness on headphones. Apply gradually.";
            r.how_to_verify = "Check M/S balance on a spectrum analyser - side-channel HF energy "
                              "should be at or below the mid channel. Listen on headphones and "
                              "confirm the stereo field sounds natural rather than exaggerated.";
            r.alternative_strategies = {
                "Use M/S EQ to cut highs in the side channel above 8 kHz.",
                "Address individual wide-field elements (stereo reverbs, chorus) before the master bus.",
            };
            out.push_back(std::move(r));
        }

        return out;
    }
};

} // namespace khensu
