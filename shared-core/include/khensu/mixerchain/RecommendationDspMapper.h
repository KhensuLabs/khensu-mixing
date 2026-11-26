#pragma once
// OVH-8: maps one finding's suggested_parameters onto the Copilot plugin's
// built-in DSP parameters (DspParamIds + the ids below), scaled by an
// `amount` 0..1. The plugin's "Apply top fix" / Auto-Apply and its Amount
// knob use it; the desktop's render path (RecommendationChain.cpp) speaks
// the same keys, and the amount scaling is the same as its
// scaledRecommendation(): "_db" keys that are not thresholds scale with the
// amount, comp_ratio / stereo_width move from 1 towards the value, every
// other key (frequencies, Q, thresholds, ceilings) is taken as is.
//
// Key -> parameter:
//   highpass_hz                      -> hpf_on, hpf_freq
//   eq_freq_hz + eq_gain_db (+eq_q)  -> a FREE EQ band (eqN_on off in `base`,
//                                       bands 1..6 in order): eq_type 2 = low
//                                       shelf, 1 = high shelf, else a bell
//                                       (eqN_type; kind Shape, stereo, static)
//   eq_center_hz + eq_cut_db (+eq_q) -> a free band as a notch (bell cut)
//   dyneq_freq_hz (+dyneq_q, dyneq_threshold_db, dyneq_max_cut_db) - the stem
//   masking plan (MaskingChain.h)    -> a free band in dynamic mode: a bell at
//                                       0 dB that cuts up to max_cut above the
//                                       threshold (5 ms / 120 ms, the service's
//                                       DynamicEQ defaults). The plugin keys it
//                                       on its own track: dyneq_key_stem (the
//                                       desktop keys on the leading stem) stays
//                                       in `unmappedKeys`. eq_freq_hz /
//                                       eq_gain_db of the same finding are the
//                                       static fallback and are not placed too.
//   comp_threshold_db / comp_ratio / comp_attack_ms / comp_release_ms
//                                    -> comp_on + the compressor
//   comp_makeup_gain_db, reduce_gain_db, increase_gain_db, limiter_gain_db
//                                    -> out_gain_db, RELATIVE to base
//   true_peak_ceiling_dbtp           -> lim_on, lim_thresh_db
//   deess_threshold_db / deess_freq_hz -> deess_on, deess_thresh_db, deess_freq,
//                                       deess_range_db (6 dB x amount)
//   transient_attack_db              -> ts_on, ts_attack_db (the service's
//                                       TransientShaper defaults otherwise)
//   saturation_drive_db              -> sat_on, sat_drive_db (warmth 0.5, mix 1,
//                                       trim 0: TapeSaturation's defaults)
//   stereo_width                     -> width_on, width_amount
//   bass_mono_hz                     -> bassmono_on, bassmono_freq
// Findings whose fix is the stage itself, with no parameters (STU-10):
//   sub_bass_too_wide                -> bassmono_on at 120 Hz (the
//                                       BassMonoMaker's default crossover)
//   stereo_too_wide / _narrow        -> width_amount solved from the finding's
//                                       "Stereo width" evidence: the side gain
//                                       that puts the measured width
//                                       (2S^2/(M^2+S^2)) kWidthAimInside inside
//                                       the genre edge it crossed, x amount
//                                       (a too-wide reading clamped at 1 is
//                                       read from its "Mid/side balance"
//                                       evidence instead, STU-16). The stage
//                                       spans x0..x2, so a side more than
//                                       halved is only part-restored.
// Everything else (denoise, reverb, phase / polarity, unknown keys) is
// reported in `unmappedKeys`.
// THREAD_CONTEXT(AnyThread) - pure function.

#include <khensu/mixerchain/DspChainMapper.h>
#include <khensu/recommendations/Recommendation.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace khensu::mixerchain {

/// Parameters the plugin added after DspParamIds (OVH-8). Appended to the
/// plugin's layout, so older project states load with their defaults.
struct DspParamIdsV3 {
    static constexpr const char* eqType = "eq_type";   ///< 0 bell, 1 low shelf, 2 high shelf
    static constexpr const char* eq2Type = "eq2_type";
    static constexpr const char* eq3Type = "eq3_type";
    static constexpr const char* deessOn = "deess_on";
    static constexpr const char* deessFreq = "deess_freq";
    static constexpr const char* deessThreshDb = "deess_thresh_db";
    static constexpr const char* deessRangeDb = "deess_range_db";
    static constexpr const char* fixAmount = "fix_amount"; ///< the Apply-top-fix Amount (0..1)
};

/// Shape values of eqN_type.
enum class EqBandShape { Bell = 0, LowShelf = 1, HighShelf = 2 };

/// eqN_kind (STU-10): 0 = the band is the shape eqN_type names; else a
/// notch, a 12 dB/oct high-pass or low-pass (eqN_type is then ignored).
/// Kept apart from eqN_type so the v3 choice parameter's steps, which
/// hosts' automation stores normalised, never change.
enum class EqBandKind { Shape = 0, Notch = 1, HighPass = 2, LowPass = 3 };
/// eqN_mode (STU-10): which signal the band filters.
enum class EqBandChannels { Stereo = 0, Mid = 1, Side = 2 };

/// The parameter ids of one built-in EQ band (STU-10: six bands). Bands 1-3
/// keep their v1/v3 ids; everything else was appended to the layout in
/// state v5, so an older project loads it at its default.
struct EqBandParamIds {
    int number;
    const char* on;
    const char* freq;
    const char* gain;
    const char* q;
    const char* type;           ///< EqBandShape
    const char* kind;           ///< EqBandKind
    const char* mode;           ///< EqBandChannels
    const char* dynOn;          ///< dynamic mode (self-keyed)
    const char* dynThreshDb;    ///< -60..0
    const char* dynRangeDb;     ///< 0..24, the most it turns the band down
    const char* dynAttackMs;    ///< 0.1..200
    const char* dynReleaseMs;   ///< 5..2000
};
inline constexpr int kNumEqBands = 6;
inline constexpr EqBandParamIds kEqBandParamIds[kNumEqBands] = {
    {1, "eq_on", "eq_freq", "eq_gain_db", "eq_q", "eq_type", "eq_kind", "eq_mode", "eq_dyn_on", "eq_dyn_thresh_db",
     "eq_dyn_range_db", "eq_dyn_attack_ms", "eq_dyn_release_ms"},
    {2, "eq2_on", "eq2_freq", "eq2_gain_db", "eq2_q", "eq2_type", "eq2_kind", "eq2_mode", "eq2_dyn_on",
     "eq2_dyn_thresh_db", "eq2_dyn_range_db", "eq2_dyn_attack_ms", "eq2_dyn_release_ms"},
    {3, "eq3_on", "eq3_freq", "eq3_gain_db", "eq3_q", "eq3_type", "eq3_kind", "eq3_mode", "eq3_dyn_on",
     "eq3_dyn_thresh_db", "eq3_dyn_range_db", "eq3_dyn_attack_ms", "eq3_dyn_release_ms"},
    {4, "eq4_on", "eq4_freq", "eq4_gain_db", "eq4_q", "eq4_type", "eq4_kind", "eq4_mode", "eq4_dyn_on",
     "eq4_dyn_thresh_db", "eq4_dyn_range_db", "eq4_dyn_attack_ms", "eq4_dyn_release_ms"},
    {5, "eq5_on", "eq5_freq", "eq5_gain_db", "eq5_q", "eq5_type", "eq5_kind", "eq5_mode", "eq5_dyn_on",
     "eq5_dyn_thresh_db", "eq5_dyn_range_db", "eq5_dyn_attack_ms", "eq5_dyn_release_ms"},
    {6, "eq6_on", "eq6_freq", "eq6_gain_db", "eq6_q", "eq6_type", "eq6_kind", "eq6_mode", "eq6_dyn_on",
     "eq6_dyn_thresh_db", "eq6_dyn_range_db", "eq6_dyn_attack_ms", "eq6_dyn_release_ms"},
};

/// The stages the plugin added in state v5 (STU-10), after the EQ bands.
struct DspParamIdsV5 {
    static constexpr const char* tsOn = "ts_on";                 ///< transient shaper
    static constexpr const char* tsAttackDb = "ts_attack_db";    ///< -24..24
    static constexpr const char* tsSustainDb = "ts_sustain_db";  ///< -24..24
    static constexpr const char* tsAttackMs = "ts_attack_ms";    ///< 1..50
    static constexpr const char* tsReleaseMs = "ts_release_ms";  ///< 20..1000
    static constexpr const char* satOn = "sat_on";               ///< tape saturation
    static constexpr const char* satDriveDb = "sat_drive_db";    ///< 0..24
    static constexpr const char* satWarmth = "sat_warmth";       ///< 0..1
    static constexpr const char* satMix = "sat_mix";             ///< 0..1
    static constexpr const char* satTrimDb = "sat_trim_db";      ///< -24..6
    static constexpr const char* bassMonoOn = "bassmono_on";     ///< mono below the crossover
    static constexpr const char* bassMonoFreq = "bassmono_freq"; ///< 40..300 Hz
    static constexpr const char* widthOn = "width_on";           ///< stereo width
    static constexpr const char* widthAmount = "width_amount";   ///< 0..2, 1 = unchanged
};

/// How far inside the genre's width window a width fix aims (width units).
inline constexpr float kWidthAimInside = 0.03f;
/// The crossover a sub_bass_too_wide fix monos the low end below.
inline constexpr float kSubBassMonoHz = 120.0f;

/// A width reading at or over this is the analyzer's clamp at 1 (the side as
/// loud as the mid or louder); a too-wide fix then solves from the finding's
/// "Mid/side balance" evidence b instead, as the unclamped width 1 - b (STU-16).
inline constexpr float kWidthClampedReading = 0.999f;

/// The side gain that moves a measured stereo width (2S^2/(M^2+S^2); the
/// unclamped value, 0..2) to `targetWidth` (0..1); 1 when either is outside.
float sideGainForWidth(float measuredWidth, float targetWidth);

struct MappedFix {
    /// Plain (not normalised) values to write, by parameter id.
    std::map<std::string, float> values;
    /// The recommendation keys that landed on a parameter.
    std::vector<std::string> mappedKeys;
    /// The keys with no built-in equivalent (to dial in by hand).
    std::vector<std::string> unmappedKeys;
    /// One line per stage touched, e.g. "EQ band 2: low shelf 120 Hz -2.5 dB".
    std::vector<std::string> notes;
    bool empty() const noexcept { return values.empty(); }
};

/// The recommendation with its parameters scaled by `amount` (0..1), the
/// same rules as the desktop's scaledRecommendation().
Recommendation scaledForAmount(const Recommendation& rec, float amount);

/// Maps `rec` at `amount` onto the plugin's parameters. `base` reads a
/// parameter's plain value BEFORE the fix (to pick a free EQ band and to
/// move the output gain relatively); an unknown id should return 0.
/// Values are clamped to the plugin's parameter ranges.
MappedFix mapRecommendationToDsp(const Recommendation& rec, float amount,
                                 const std::function<float(const std::string&)>& base);

/// The plugin's plain range of a mapped parameter id (false if unknown).
bool dspParamRange(const std::string& paramId, float& lo, float& hi);

} // namespace khensu::mixerchain
