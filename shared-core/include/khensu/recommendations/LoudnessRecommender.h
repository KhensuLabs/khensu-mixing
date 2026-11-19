#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <khensu/recommendations/RecommenderCommon.h>
#include <khensu/recommendations/PlaybackIntentTargets.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace khensu {

class LoudnessRecommender {
  public:
    /// Severity of a true-peak finding whose master stays at or under 0 dBTP
    /// (minor: encoder headroom, not an over) - MEAS-4.
    static constexpr float kTruePeakHeadroomSeverity = 20.0f;
    /// The quietest mainstream normalisation reference (Apple Music, the
    /// headphone intent). A master under its genre's loudness window but at or
    /// above this level is a mastering choice, not a fault: normalised players
    /// play it at about the level of a louder master. 38.7% of released tracks
    /// sit under their genre's floor, 14.8% under this level (MEAS-4).
    static constexpr float kQuietChoiceFloorLufs = -16.0f;
    /// Severity of a quiet-but-normalisable master (a minor note); below
    /// kQuietChoiceFloorLufs the severity rises from here.
    static constexpr float kQuietChoiceSeverity = 20.0f;

    /// Severity of lufs_too_quiet for a master at `lufs` under `lufsMin`.
    /// With a playback intent the window is the user's own, graded in full;
    /// with the genre window, the stretch between the genre floor and
    /// kQuietChoiceFloorLufs is a minor note.
    static float quietSeverity(float lufs, float lufsMin, bool intentWindow) noexcept
    {
        using namespace recommendations::common;
        if (intentWindow || lufsMin <= kQuietChoiceFloorLufs)
            return severityFromError(lufs, lufsMin, severity_scale::kLufs);
        if (lufs >= kQuietChoiceFloorLufs) return kQuietChoiceSeverity;
        return kQuietChoiceSeverity + (100.0f - kQuietChoiceSeverity) / 100.0f *
                                          severityFromError(lufs, kQuietChoiceFloorLufs, severity_scale::kLufs);
    }
    /// BS.1770 absolute gate: at or below this the programme is silence.
    static constexpr float kSilentLufs = -70.0f;
    /// A boost toward the -1 dBTP ceiling stops this far short of it. Aimed
    /// at the ceiling exactly, the app's loudness fix was rejected by its own
    /// post-render guardrail at -0.94 dBTP (found by VER-3's loop run): the
    /// native analyzer's true-peak filter differed from the service's. MEAS-5
    /// gave both the same filter; the margin stays for what the render itself
    /// adds (the limiter's own overshoot, the written file's sample format).
    static constexpr float kTruePeakMeasurementMarginDb = 0.3f;

    std::vector<Recommendation> recommend(const AnalysisResult& result, const GenreEntry& genre,
                                          const CreativeTarget& target) const
    {
        std::vector<Recommendation> out;
        const auto& l = result.loudness;

        // Silent / unmeasured (AUDIT-069): integrated loudness at the -70
        // LUFS absolute gate (or the -99 "not measured" default) and the -200
        // true-peak sentinel are not levels - "raise gain ~54 dB" on a silent
        // file is not advice. Abstain from every loudness recommendation.
        if (l.lufs_integrated <= kSilentLufs) return out;
        // A chosen playback intent (club / broadcast / headphone) picks the
        // integrated-loudness window and true-peak ceiling. Streaming (the
        // Intake default), reference and unset keep the genre's window.
        const auto intent = loudnessOverrideForIntent(target.playback_intent);
        const float lufsMin = intent ? intent->lufs - intent->tolerance_lu : genre.lufs_range_min;
        const float lufsMax = intent ? intent->lufs + intent->tolerance_lu : genre.lufs_range_max;
        const float tpMax = intent ? intent->true_peak_dbtp : genre.true_peak_max;
        const std::string windowName = intent ? std::string(intent->standard) + " (" + intent->intent + " intent)"
                                              : std::string("genre");

        using namespace khensu::recommendations::common;
        auto make = [&](const char* code, float sevVal, ProcessingStage stg, int pos, bool taste = false) {
            return makeRecommendation(result.file_id, code, sevVal, 0.90f, "Master", stg, pos, taste);
        };

        if (l.lufs_integrated > lufsMax) {
            const float delta = l.lufs_integrated - lufsMax;
            auto r = make("lufs_too_loud", severityFromError(l.lufs_integrated, lufsMax, severity_scale::kLufs), ProcessingStage::Master, 8);
            r.what = "Integrated loudness is " + f1(l.lufs_integrated) + " LUFS - " + f1(delta) + " dB above the " +
                     f1(lufsMax) + " LUFS " + windowName + " ceiling.";
            r.why_helps = "Content louder than the platform ceiling triggers gain-normalisation "
                          "that makes the master sound squashed. Reducing output level preserves "
                          "punch and translates cleanly across streams and broadcast.";
            r.suggested_action = "Reduce master output gain by " + f1(delta) + " dB.";
            r.engineer_note = "Lower the master bus fader or limiter output ceiling by " + f1(delta) +
                              " dB. Re-check true-peak headroom after the change.";
            r.how_to_verify = "Re-measure with a loudness analyser - integrated LUFS should sit "
                              "between " +
                              f1(lufsMin) + " and " + f1(lufsMax) + " LUFS.";
            r.suggested_parameters["reduce_gain_db"] = delta;
            out.push_back(std::move(r));
        }

        if (l.lufs_integrated < lufsMin) {
            const float gain_needed = lufsMin - l.lufs_integrated;
            // -1.0 dBTP: consistent with the true_peak_over guidance below and
            // safe for lossy-codec inter-sample headroom (the previous -0.1
            // ceiling contradicted the file's own streaming-safety advice).
            const float kSafePeakCeiling = std::min(-1.0f, tpMax);
            const float headroom = kSafePeakCeiling - kTruePeakMeasurementMarginDb - l.true_peak_dbtp;
            const float safe_gain = std::min(gain_needed, std::max(0.0f, headroom));
            auto r = make("lufs_too_quiet", quietSeverity(l.lufs_integrated, lufsMin, intent.has_value()),
                          ProcessingStage::Master, 8);
            r.what = "Integrated loudness is " + f1(l.lufs_integrated) + " LUFS - " + f1(gain_needed) +
                     " dB below the " + f1(lufsMin) + " LUFS " + windowName + " floor.";
            if (!intent && l.lufs_integrated >= kQuietChoiceFloorLufs) {
                r.why_helps = "Quieter than this genre is usually mastered, but at or above " +
                              f1(kQuietChoiceFloorLufs) +
                              " LUFS normalised players play it at about the level of a louder master - "
                              "a choice, not a fault. Raise it only for the genre's density, or for "
                              "players that do not normalise.";
            } else {
                r.why_helps = "Masters that are too quiet get normalised up, which amplifies any "
                              "low-level noise or hiss. Raising level within the genre window improves "
                              "perceived energy and consistency on streaming platforms.";
            }
            if (safe_gain < 0.05f) {
                // No headroom at all: "raise by 0.0 dB" is not advice. Peaks
                // must be controlled before any level can be added.
                r.suggested_action = "Control peaks first: limit or clip-gain the transients until true peak "
                                     "sits under " +
                                     f1(kSafePeakCeiling) + " dBTP, then raise the level by up to " +
                                     f1(gain_needed) + " dB.";
            } else {
                r.suggested_action = "Raise master output by " + f1(safe_gain) +
                                     " dB "
                                     "(headroom-limited; desired gain is " +
                                     f1(gain_needed) + " dB).";
            }
            r.engineer_note = "Increase limiter input or master fader. Measured true peak is " + f1(l.true_peak_dbtp) +
                              " dBTP - max safe boost is " + f1(safe_gain) + " dB before clipping risk.";
            r.how_to_verify = "Integrated LUFS should land between " + f1(lufsMin) + " and " +
                              f1(lufsMax) + " LUFS after the change.";
            r.suggested_parameters["increase_gain_db"] = safe_gain;
            r.suggested_parameters["desired_gain_db"] = gain_needed;
            out.push_back(std::move(r));
        }

        if (l.lra_measured && l.lra < genre.lra_range_min) {
            auto r = make("lra_too_compressed", severityFromError(l.lra, genre.lra_range_min, severity_scale::kLra), ProcessingStage::MixBus, 5);
            r.what = "Loudness range (LRA) is " + f1(l.lra) + " LU - below the " + f1(genre.lra_range_min) +
                     " LU minimum for this genre.";
            r.why_helps = "Excessive compression collapses the dynamic contrast that gives a "
                          "mix energy and feel. Relaxing bus compression restores the natural "
                          "push-and-pull that makes drops and choruses hit harder.";
            r.suggested_action = "Ease bus compression: raise the compressor attack and/or reduce "
                                 "ratio until LRA is above " +
                                 f1(genre.lra_range_min) + " LU.";
            r.engineer_note = "Check master bus and mix bus compressor settings. A pumping or "
                              "over-compressed sound is often caused by release time set too short.";
            r.how_to_verify =
                "Re-measure LRA - target range is " + f1(genre.lra_range_min) + "-" + f1(genre.lra_range_max) + " LU.";
            out.push_back(std::move(r));
        }

        if (l.lra_measured && l.lra > genre.lra_range_max) {
            auto r = make("lra_too_dynamic", severityFromError(l.lra, genre.lra_range_max, severity_scale::kLra), ProcessingStage::MixBus, 5);
            r.what = "Loudness range (LRA) is " + f1(l.lra) + " LU - above the " + f1(genre.lra_range_max) +
                     " LU ceiling for this genre.";
            r.why_helps = "Excessive dynamic range makes quiet sections feel too sparse and "
                          "loud sections too jarring, especially on streaming where normalization "
                          "compounds the effect.";
            r.suggested_action = "Tighten dynamics: increase mix bus compression or apply clip gain "
                                 "automation to pull down peaks until LRA is below " +
                                 f1(genre.lra_range_max) + " LU.";
            r.engineer_note = "Consider gentle glue compression on the mix bus (2:1 ratio, "
                              "slow attack, auto release) and parallel clip gain on the loudest sections.";
            r.how_to_verify =
                "Re-measure LRA - target range is " + f1(genre.lra_range_min) + "-" + f1(genre.lra_range_max) + " LU.";
            out.push_back(std::move(r));
        }

        if (l.true_peak_dbtp > tpMax) {
            const float over = l.true_peak_dbtp - tpMax;
            // MEAS-4: 85% of released masters measured in the corpus (lossless
            // originals included) peak above -1 dBTP and 62% above 0 dBTP. Under
            // the ceiling but at or below 0 dBTP the master is legal and only
            // short of encoder headroom - a minor note; above 0 dBTP it clips on
            // any conversion, and severity grows with the overshoot.
            const float sev = l.true_peak_dbtp <= 0.0f
                                  ? kTruePeakHeadroomSeverity
                                  : std::min(95.0f, 50.0f + 15.0f * l.true_peak_dbtp);
            auto r = make("true_peak_over", sev, ProcessingStage::Master, 9);
            r.what = "True-peak level is " + f1(l.true_peak_dbtp) + " dBTP - " + f1(over) + " dB above the " +
                     f1(tpMax) + " dBTP ceiling (BS.1770-4 4x oversampled).";
            r.why_helps = "True-peak overs cause inter-sample clipping on lossy encoders "
                          "(MP3, AAC, OGG). Spotify recommends keeping true peak at or below -1 dBTP, "
                          "and Apple Digital Masters guidance asks for headroom against encoder "
                          "clipping - the files are not rejected, but the encoded stream can distort.";
            r.suggested_action = "Apply a true-peak limiter with ceiling set to " + f1(tpMax) + " dBTP.";
            r.engineer_note = "Verify the limiter uses oversampled true-peak detection (not just "
                              "sample-peak). A -1 dBTP ceiling is the common streaming recommendation; "
                              "very loud, dense masters may need -2 dBTP to stay clean after encoding.";
            r.how_to_verify = "Run BS.1770-4 compliant loudness analysis - true-peak reading "
                              "should be <= " +
                              f1(tpMax) + " dBTP.";
            r.suggested_parameters["true_peak_ceiling_dbtp"] = tpMax;
            if (intent) r.suggested_parameters["target_lufs"] = intent->lufs;
            out.push_back(std::move(r));
        }

        return out;
    }
};

} // namespace khensu
