#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <khensu/recommendations/RecommenderCommon.h>
#include <khensu/recommendations/TonalFixModel.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace khensu {

class SpectralRecommender {
  public:
    // The tonal rules, defined once: the recommender below, the Mix
    // Scorecard and the evidence trail all read these, so a card, its score
    // and its "why" cannot disagree about where a rule fires or how bad it is.

    /// Centroid window: the genre centroid +25% / -25%.
    static constexpr float kCentroidCeilingRatio = 1.25f;
    static constexpr float kCentroidFloorRatio = 0.75f;
    /// Tilt within +/- this of the genre target is on-character.
    static constexpr float kTiltToleranceDbPerOctave = 2.0f;
    /// Low-end band (FIX-9). The measurement is the share of energy below
    /// 250 Hz, and a share is not a level: "target +/- 0.20" was 3.7 dB of
    /// balance at a 0.50 target, 8 dB at 0.75, and at or past 1.0 for every
    /// genre with a target of 0.80 or more - 15 bass-led genres (trap, phonk,
    /// dubstep, miami bass...) where low_end_heavy could not fire. The band is
    /// one fixed change in the balance between the lows and everything else:
    /// the odds low / (1 - low) may move by this factor either way. 7/3 is
    /// 3.7 dB, which is exactly +/- 0.20 at a 0.50 target, and the edges stay
    /// inside 0..1 for every target. Not yet tuned against released tracks
    /// (MEAS-1).
    static constexpr float kLowEndOddsRatio = 7.0f / 3.0f;

    /// MEAS-4: the shelves the tonal fixes render, and how far past the edge
    /// they aim - halfway from the edge to the genre target, so a fixed mix
    /// lands inside the window instead of on its rim.
    static constexpr float kLowShelfHz = 150.0f;
    static constexpr float kHighShelfHz = 4000.0f;
    /// FIX-A: the tilt fix's shelf corner - mid-spectrum, so the shelf turns
    /// the slope across all ten octaves instead of only the top three.
    static constexpr float kTiltShelfHz = 1000.0f;
    static constexpr float kAimTowardTarget = 0.5f;

    /// Gain for a tonal fix from the track's own octave spectrum (TonalFixModel),
    /// or `fallback` when no spectrum was measured; clamped to [lo, hi] dB.
    static float modelledGain(float modelled, float fallback, float lo, float hi) noexcept
    {
        return std::clamp(std::isfinite(modelled) ? modelled : fallback, lo, hi);
    }

    /// The high-shelf gain toward `aim` Hz: from the FFT bins when the result
    /// has them (the measured centroid's own domain), else the octave bands.
    static float centroidShelfGain(const AnalysisResult& result, float aim, double lo, double hi)
    {
        const auto& s = result.spectral;
        const float fromBins = tonal_fix::highShelfGainForCentroidFft(s.fft_mid_magnitudes, result.sample_rate,
                                                                      s.centroid_hz, aim, kHighShelfHz, lo, hi);
        if (std::isfinite(fromBins)) return fromBins;
        return tonal_fix::highShelfGainForCentroid(s.rms_per_octave_db, s.centroid_hz, aim, kHighShelfHz, lo, hi);
    }

    static float centroidCeilingHz(const GenreEntry& genre) noexcept
    {
        return genre.spectral_centroid_hz * kCentroidCeilingRatio;
    }
    static float centroidFloorHz(const GenreEntry& genre) noexcept
    {
        return genre.spectral_centroid_hz * kCentroidFloorRatio;
    }

    /// Low-end share above which low_end_heavy fires: 0.50 -> 0.70,
    /// 0.75 -> 0.875, 0.90 -> 0.955.
    static float lowEndCeiling(const GenreEntry& genre) noexcept
    {
        const float target = std::clamp(genre.low_end_weight, 0.0f, 1.0f);
        const float lows = target * kLowEndOddsRatio;
        const float sum = lows + (1.0f - target);
        return sum > 0.0f ? lows / sum : target;
    }
    /// Low-end share below which low_end_light fires - the mirror image:
    /// 0.50 -> 0.30, 0.75 -> 0.5625, 0.90 -> 0.794.
    static float lowEndFloor(const GenreEntry& genre) noexcept
    {
        const float target = std::clamp(genre.low_end_weight, 0.0f, 1.0f);
        const float sum = target + (1.0f - target) * kLowEndOddsRatio;
        return sum > 0.0f ? target / sum : target;
    }

    /// Severity of the centroid finding for this mix; 0 inside the window.
    static float centroidSeverity(float centroidHz, const GenreEntry& genre) noexcept
    {
        using namespace khensu::recommendations::common;
        if (centroidHz > centroidCeilingHz(genre))
            return severityFromOctaves(centroidHz, centroidCeilingHz(genre), severity_scale::kCentroidOctaves);
        if (centroidHz < centroidFloorHz(genre))
            return severityFromOctaves(centroidHz, centroidFloorHz(genre), severity_scale::kCentroidOctaves);
        return 0.0f;
    }
    /// Severity of the low-end finding for this mix; 0 inside the band.
    static float lowEndSeverity(float balanceLow, const GenreEntry& genre) noexcept
    {
        using namespace khensu::recommendations::common;
        if (balanceLow > lowEndCeiling(genre))
            return severityFromError(balanceLow, lowEndCeiling(genre), severity_scale::kBandShare);
        if (balanceLow < lowEndFloor(genre))
            return severityFromError(balanceLow, lowEndFloor(genre), severity_scale::kBandShare);
        return 0.0f;
    }

    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& genre,
                                          const CreativeTarget& /*target*/) const
    {
        std::vector<Recommendation> out;
        const auto& s = result.spectral;

        using namespace khensu::recommendations::common;
        auto make = [&](const char* code, float sevVal, ProcessingStage stg, int pos, bool taste = false) {
            return makeRecommendation(result.file_id, code, sevVal, 0.80f, "Master", stg, pos, taste);
        };

        const float bright_thresh = centroidCeilingHz(genre);
        const float dark_thresh = centroidFloorHz(genre);

        if (s.centroid_hz > bright_thresh) {
            auto r = make("centroid_too_bright", centroidSeverity(s.centroid_hz, genre), ProcessingStage::Master, 7);
            const float aim = bright_thresh - kAimTowardTarget * (bright_thresh - genre.spectral_centroid_hz);
            const float gain = modelledGain(centroidShelfGain(result, aim, -12.0, 0.0),
                                            -std::clamp((s.centroid_hz - bright_thresh) * 0.0015f, 0.5f, 5.0f),
                                            -12.0f, -0.5f);
            r.what = "Spectral centroid is " + f0(s.centroid_hz) + " Hz - " + f0(s.centroid_hz - bright_thresh) +
                     " Hz above the " + f0(bright_thresh) + " Hz genre ceiling.";
            r.why_helps = "A bright spectral centroid means the perceived 'weight' of the mix "
                          "sits too high in the frequency spectrum. It translates as harshness, "
                          "fatigue, and thinness on consumer speakers, and sounds out of "
                          "character for the genre.";
            r.suggested_action = "Reduce high end: apply a High Shelf cut of " + f1(gain) + " dB at 4 kHz.";
            r.engineer_note = "Check the source of brightness - a single overly bright synth "
                              "or reverb return is often the culprit. Better to treat the source "
                              "than add a master EQ cut.";
            r.how_to_verify = "Re-measure centroid - target below " + f0(bright_thresh) + " Hz.";
            r.suggested_parameters["eq_type"] = 1.0f;
            r.suggested_parameters["eq_freq_hz"] = kHighShelfHz;
            r.suggested_parameters["eq_gain_db"] = gain;
            r.suggested_parameters["eq_q"] = 0.707f;
            out.push_back(std::move(r));
        }

        if (s.centroid_hz < dark_thresh) {
            auto r = make("centroid_too_dark", centroidSeverity(s.centroid_hz, genre), ProcessingStage::Master, 7);
            const float aim = dark_thresh + kAimTowardTarget * (genre.spectral_centroid_hz - dark_thresh);
            const float gain = modelledGain(centroidShelfGain(result, aim, 0.0, 12.0),
                                            std::clamp((dark_thresh - s.centroid_hz) * 0.002f, 0.5f, 4.0f), 0.5f,
                                            12.0f);
            r.what = "Spectral centroid is " + f0(s.centroid_hz) + " Hz - " + f0(dark_thresh - s.centroid_hz) +
                     " Hz below the " + f0(dark_thresh) + " Hz genre floor.";
            r.why_helps = "A dark centroid makes the mix sound muddy, buried, and lacking "
                          "presence on consumer playback. High frequencies carry vocal clarity "
                          "and instrumental definition - adding air lifts the mix out of the mud.";
            r.suggested_action = "Boost high end: apply a High Shelf boost of " + f1(gain) + " dB at 4 kHz.";
            r.engineer_note = "Verify the mastering chain has no hidden low-pass filters or "
                              "overactive de-essers pulling the air frequencies. A gentle air-band "
                              "boost at 10-16 kHz can also help without affecting the midrange.";
            r.how_to_verify = "Re-measure centroid - target above " + f0(dark_thresh) + " Hz.";
            r.suggested_parameters["eq_type"] = 1.0f;
            r.suggested_parameters["eq_freq_hz"] = kHighShelfHz;
            r.suggested_parameters["eq_gain_db"] = gain;
            r.suggested_parameters["eq_q"] = 0.707f;
            out.push_back(std::move(r));
        }

        // --- Spectral tilt vs. genre target ---
        // Complements the single-scalar centroid check with the overall
        // tonal-balance *shape*: a mix can have an on-target centroid while
        // still sounding wrong-shaped for the genre (e.g. a hyperpop mix
        // that's bright on average but missing the genre's characteristic
        // near-flat/rising top end, or a shoegaze mix that's darker overall
        // but rolling off too early to read as a wall of sound).
        const float tiltDiff = s.tilt_db_per_octave - genre.spectral_tilt_target_db_per_octave;
        if (std::abs(tiltDiff) > kTiltToleranceDbPerOctave) {
            const bool tooBright = tiltDiff > 0.0f;
            auto r = make(tooBright ? "tilt_too_bright" : "tilt_too_dark",
                          // CORE-05: severity from the tolerance edge that fired the card
                          // (target +/- tolerance), like every other recommender - measured
                          // from the target it jumped from 0 to ~50 at the edge.
                          severityFromError(s.tilt_db_per_octave,
                                            genre.spectral_tilt_target_db_per_octave +
                                                (tooBright ? kTiltToleranceDbPerOctave : -kTiltToleranceDbPerOctave),
                                            severity_scale::kTilt),
                          ProcessingStage::Master, 7, true);
            const std::string genreLabel = genre.name.empty() ? std::string("genre") : genre.name;
            r.what = "Overall tonal balance slopes at " + f1(s.tilt_db_per_octave) + " dB/octave - the " + genreLabel +
                     " target is " + f1(genre.spectral_tilt_target_db_per_octave) + " dB/octave.";
            r.why_helps = "Spectral tilt describes the mix's overall brightness curve, not just a "
                          "single centroid point - matching the genre's characteristic tilt shape "
                          "(e.g. a bright, near-flat top end for hyperpop vs. a darker, rolled-off "
                          "top end for shoegaze/ambient) is part of what makes a mix read as "
                          "belonging to its genre.";
            r.suggested_action =
                tooBright ? "Broad high-shelf cut across the top end to bring the overall slope down toward " +
                                f1(genre.spectral_tilt_target_db_per_octave) + " dB/octave."
                          : "Broad high-shelf boost (or reduce excessive low-end/low-mid buildup) to bring the "
                            "overall slope up toward " +
                                f1(genre.spectral_tilt_target_db_per_octave) + " dB/octave.";
            r.engineer_note = "Unlike the centroid checks above, this looks at the whole 10-band "
                              "curve's regression slope - a broad, gentle shelf across several "
                              "octaves is usually the right tool, not a narrow bell.";
            r.how_to_verify = "Re-measure spectral tilt - target within +/-" + f1(kTiltToleranceDbPerOctave) +
                              " dB/octave of " + f1(genre.spectral_tilt_target_db_per_octave) + ".";
            r.is_taste_decision = true;
            // FIX-A: a rendered fix - the 1 kHz high shelf that lands the
            // tilt halfway from the window's edge to the target, solved from
            // the track's own FFT bins. Without the bins it stays advice.
            {
                const float edge = genre.spectral_tilt_target_db_per_octave +
                                   (tooBright ? kTiltToleranceDbPerOctave : -kTiltToleranceDbPerOctave);
                const float aim = edge + kAimTowardTarget * (genre.spectral_tilt_target_db_per_octave - edge);
                const float gain = tonal_fix::highShelfGainForTiltFft(s.fft_mid_magnitudes, result.sample_rate,
                                                                      s.tilt_db_per_octave, aim, kTiltShelfHz,
                                                                      tooBright ? -12.0 : 0.0,
                                                                      tooBright ? 0.0 : 12.0);
                if (std::isfinite(gain) && std::abs(gain) >= 0.5f) {
                    r.suggested_action = std::string(tooBright ? "Cut" : "Boost") + " a broad High Shelf by " +
                                         f1(std::abs(gain)) + " dB at 1 kHz to bring the slope to about " +
                                         f1(aim) + " dB/octave.";
                    r.suggested_parameters["eq_type"] = 1.0f;
                    r.suggested_parameters["eq_freq_hz"] = kTiltShelfHz;
                    r.suggested_parameters["eq_gain_db"] = gain;
                    r.suggested_parameters["eq_q"] = 0.707f;
                }
            }
            out.push_back(std::move(r));
        }

        // Low-end balance: both edges come from lowEndCeiling / lowEndFloor so
        // each rule can fire for every genre (FIX-9).
        const float heavy_thresh = lowEndCeiling(genre);
        const float light_thresh = lowEndFloor(genre);

        if (s.balance_low > heavy_thresh) {
            auto r = make("low_end_heavy", lowEndSeverity(s.balance_low, genre), ProcessingStage::MixBus, 3);
            const float aim = heavy_thresh - kAimTowardTarget * (heavy_thresh - genre.low_end_weight);
            const float gain = modelledGain(
                tonal_fix::lowShelfGainForShare(s.rms_per_octave_db, s.balance_low, aim, kLowShelfHz, -15.0, 0.0),
                -std::clamp((s.balance_low - heavy_thresh) * 8.0f, 0.5f, 6.0f), -15.0f, -0.5f);
            r.what = "Low-end energy share is " + f1(s.balance_low * 100.0f) + "% - the genre target is " +
                     f1(genre.low_end_weight * 100.0f) + "%.";
            r.why_helps = "Excessive low-end weight causes muddy, boomy translation on "
                          "earbuds and laptop speakers, and can cause phase issues in mono. "
                          "Reducing sub buildup improves clarity on small speakers without "
                          "losing warmth on full-range systems.";
            r.suggested_action = "Control low-end buildup: apply a Low Shelf cut of " + f1(gain) + " dB at 150 Hz.";
            r.engineer_note = "High-pass filter individual bass-heavy stems before applying a "
                              "master bus shelf. Check if the sub-bass is leaking into the "
                              "mid-band (check 200-400 Hz range too).";
            r.how_to_verify = "Low-end energy share should be at or below " + f1(heavy_thresh * 100.0f) +
                              "% (genre target " + f1(genre.low_end_weight * 100.0f) + "%).";
            r.suggested_parameters["eq_type"] = 2.0f;
            r.suggested_parameters["eq_freq_hz"] = kLowShelfHz;
            r.suggested_parameters["eq_gain_db"] = gain;
            r.suggested_parameters["eq_q"] = 0.707f;
            out.push_back(std::move(r));
        }

        if (s.balance_low < light_thresh) {
            auto r = make("low_end_light", lowEndSeverity(s.balance_low, genre), ProcessingStage::MixBus, 3);
            const float aim = light_thresh + kAimTowardTarget * (genre.low_end_weight - light_thresh);
            const float gain = modelledGain(
                tonal_fix::lowShelfGainForShare(s.rms_per_octave_db, s.balance_low, aim, kLowShelfHz, 0.0, 15.0),
                std::clamp((light_thresh - s.balance_low) * 10.0f, 0.5f, 4.5f), 0.5f, 15.0f);
            r.what = "Low-end energy share is " + f1(s.balance_low * 100.0f) + "% - below the " +
                     f1(light_thresh * 100.0f) + "% genre floor.";
            r.why_helps = "Insufficient low-end weight makes the mix sound thin and anemic, "
                          "especially compared to genre references. Bass presence is a "
                          "fundamental expectation for most modern genres and improves "
                          "perceived energy and fullness.";
            r.suggested_action = "Add warmth: apply a Low Shelf boost of " + f1(gain) + " dB at 150 Hz.";
            r.engineer_note = "Check if the bass and kick have been high-passed too aggressively. "
                              "A common cause is an overly tight HPF on the bass guitar or synth "
                              "bass removing the fundamental.";
            r.how_to_verify = "Low-end energy share should be at or above " + f1(light_thresh * 100.0f) +
                              "% (genre target " + f1(genre.low_end_weight * 100.0f) + "%).";
            r.suggested_parameters["eq_type"] = 2.0f;
            r.suggested_parameters["eq_freq_hz"] = kLowShelfHz;
            r.suggested_parameters["eq_gain_db"] = gain;
            r.suggested_parameters["eq_q"] = 0.707f;
            out.push_back(std::move(r));
        }

        // --- Cross-Stem Low-End Collision Recommendation ---
        if (result.drums.has_value() && result.drums->kick_bass_clash) {
            // FULL-3: kick and bass fundamentals within 10 Hz on 34% of
            // released masters - often a kick tuned to the key on purpose.
            // A minor note to check, not an alarm.
            auto r = make("kick_bass_collision", 30.0f, ProcessingStage::MixBus, 4);
            r.what = "Low-end frequency collision detected between Kick and Bass. Proximity: " +
                     f1(result.drums->kick_bass_proximity_hz) + " Hz.";
            r.why_helps = "When the kick and bass fundamental frequencies overlap too closely (within 10 Hz), "
                          "they mask each other, creating a muddy low-end and causing phase cancellation.";
            r.suggested_action = "Carve space: apply a narrow EQ cut (-3 dB, Q=2.0) on the Bass stem at the Kick's "
                                 "fundamental frequency, or sidechain-compress the Bass using the Kick as the trigger.";
            r.engineer_note =
                "Identify which element owns the sub-bass region. If the kick owns 50-60 Hz, "
                "high-pass the bass slightly higher or dip the bass at 55 Hz to let the kick punch through.";
            r.how_to_verify =
                "The kick transient should feel more defined, and the low-end mud should decrease in mono.";
            out.push_back(std::move(r));
        }

        // --- Cross-Stem Midrange Masking (Vocals vs. Guitars/Keys) Recommendation ---
        if (result.resonance.has_value()) {
            for (const auto& peak : result.resonance->hotspots) {
                if (peak.hz >= 1000.0f && peak.hz <= 3000.0f && peak.prominence > 4.0f) {
                    auto r = make("midrange_vocal_masking", 60.0f, ProcessingStage::MixBus, 4);
                    r.what = "Prominent midrange resonance detected at " + f0(peak.hz) +
                             " Hz (Prominence: " + f1(peak.prominence) + " dB).";
                    r.why_helps = "Midrange build-ups in the 1-3 kHz region often mask the lead vocal, "
                                  "making it sound buried and reduced in clarity.";
                    r.suggested_action = "Dampen guitar/synth masking: apply a dynamic EQ cut or narrow bell cut "
                                         "of -2.5 dB at " +
                                         f0(peak.hz) + " Hz on clashing backing instruments.";
                    r.engineer_note = "Carving a small pocket in the backing instruments at this frequency "
                                      "will allow the vocal presence to pop out without having to raise "
                                      "the vocal fader.";
                    r.how_to_verify = "Vocals should sound more front-of-mix and clear without sibilance increasing.";
                    r.suggested_parameters["eq_type"] = 0.0f; // Bell
                    r.suggested_parameters["eq_freq_hz"] = peak.hz;
                    r.suggested_parameters["eq_gain_db"] = -2.5f;
                    r.suggested_parameters["eq_q"] = 2.0f; // Narrow
                    out.push_back(std::move(r));
                    break; // Just report the strongest one to avoid spamming
                }
            }
        }

        return out;
    }
};

} // namespace khensu
