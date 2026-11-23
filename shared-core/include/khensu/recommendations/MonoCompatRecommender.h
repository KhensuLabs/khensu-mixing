#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <algorithm>
#include <string>
#include <vector>

namespace khensu {

class MonoCompatRecommender {
  public:
    /// TUNE-3: comb filtering (one channel a fraction of a millisecond late)
    /// drags the correlation of every band above 500 Hz toward zero at once,
    /// while a wide reverb or a stereo synth lowers one band. So the test is
    /// the MEAN of the midrange, upper-mid and air correlations: under 0.15
    /// it fires on 8% of released tracks (209 lossless originals) and on 94%
    /// of a 0.5 ms comb; it reaches moderate under 0.05 (4% of releases).
    /// The old per-band test (any band under 0 / -0.2) alarmed on 12% and
    /// caught 67%. The low end is left to sub_bass_too_wide, which this
    /// finding used to repeat (low_end_stereo_pct over 30%).
    static constexpr float kHighBandCorrelationFloor = 0.15f;
    static constexpr float kBassCorrelationFloor = 0.10f;
    static constexpr float kSeverityAtFloor = 20.0f;
    static constexpr float kSeverityPerUnit = 200.0f; // 0.10 below the floor = moderate (40)

    static float highBandMean(const AnalysisResult::MonoCompatData& mc) noexcept
    {
        float sum = 0.0f;
        int n = 0;
        for (const char* band : {"midrange", "upper_mid", "air"}) {
            const auto it = mc.per_band_correlation.find(band);
            if (it == mc.per_band_correlation.end()) continue;
            sum += it->second;
            ++n;
        }
        return n >= 2 ? sum / static_cast<float>(n) : 1.0f;
    }

    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& /*genre*/,
                                          const CreativeTarget& /*target*/) const
    {
        std::vector<Recommendation> out;
        if (!result.mono_compat.has_value()) return out;
        const auto& mc = *result.mono_compat;

        const float hi = highBandMean(mc);
        std::string worst_band;
        float worst_corr = 1.0f;
        for (const char* band : {"midrange", "upper_mid", "air"}) {
            const auto it = mc.per_band_correlation.find(band);
            if (it != mc.per_band_correlation.end() && it->second < worst_corr) {
                worst_corr = it->second;
                worst_band = band;
            }
        }
        const auto bassIt = mc.per_band_correlation.find("bass");
        const float bass = bassIt != mc.per_band_correlation.end() ? bassIt->second : 1.0f;

        auto makeRec = [&](const std::string& subCode, float sev, const std::string& what, const std::string& action) {
            Recommendation r;
            r.id = "mono_compat_warning_" + subCode + "_" + result.file_id;
            r.type = RecommendationType::Technical;
            r.severity = sev;
            r.confidence = 0.80f;
            r.stem_or_bus = "Master";
            r.is_target_relative = false;
            r.safe_to_auto_apply = false;
            r.stage = ProcessingStage::MixBus;
            r.chain_position = 6;
            r.is_taste_decision = false;
            r.what = what;
            r.why_helps = "A mono-compatible mix keeps its body and clarity on phones, smart speakers, "
                          "club PA systems and any device that plays in mono.";
            r.suggested_action = action;
            r.engineer_note = "Mono compatibility matters for phones, smart speakers and club PA. "
                              "Low correlation across every band at once usually means a timing or polarity "
                              "problem between the channels rather than deliberate width.";
            r.confidence_reason = "Per-band left/right correlation (band-pass filter + Pearson), "
                                  "calibrated on 209 released tracks and injected comb filtering.";
            r.trade_off = "Narrowing a widened part trades a little stereo width for a solid mono fold-down.";
            r.how_to_verify = "Sum the mix to mono: the vocal, snare and lead parts should keep their level "
                              "and tone. A hollow, phasey or quieter mono sum indicates cancellation.";
            r.alternative_strategies = {
                "Re-align a delayed or polarity-flipped stereo pair (a sample-accurate delay or a polarity switch).",
                "Replace a Haas-style widener with mid/side width that sums cleanly to mono.",
            };
            return r;
        };

        if (hi < kHighBandCorrelationFloor) {
            const float sev =
                std::clamp(kSeverityAtFloor + (kHighBandCorrelationFloor - hi) * kSeverityPerUnit, kSeverityAtFloor, 90.0f);
            std::string what = "Left and right are barely correlated above 500 Hz (mean correlation " +
                               std::to_string(hi).substr(0, 4) + ", lowest in the " + worst_band +
                               " band) - the mids and highs may thin out or comb-filter in mono.";
            std::string action = "Check for a delayed or phase-inverted channel, a wide chorus or a Haas-style "
                                 "widener on a main part; fold the mix to mono and listen for hollow, phasey mids.";
            out.push_back(makeRec("band_correlation", sev, what, action));
        }

        if (bass < kBassCorrelationFloor) {
            const float sev =
                std::clamp(kSeverityAtFloor + (kBassCorrelationFloor - bass) * kSeverityPerUnit, kSeverityAtFloor, 90.0f);
            std::string what = "Stereo correlation in the bass band is " + std::to_string(bass).substr(0, 4) +
                               " - bass notes may cancel in mono playback.";
            std::string action = "Check stereo-imaging on the bass bus. Reduce or remove stereo processing below "
                                 "the problem band.";
            out.push_back(makeRec("bass", sev, what, action));
        }

        return out;
    }
};

} // namespace khensu
