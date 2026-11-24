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
//   eq_freq_hz + eq_gain_db (+eq_q)  -> a FREE EQ band (eqN_on off in `base`):
//                                       eq_type 2 = low shelf, 1 = high shelf,
//                                       else a bell (eqN_type)
//   eq_center_hz + eq_cut_db (+eq_q) -> a free band as a notch (bell cut)
//   comp_threshold_db / comp_ratio / comp_attack_ms / comp_release_ms
//                                    -> comp_on + the compressor
//   comp_makeup_gain_db, reduce_gain_db, increase_gain_db, limiter_gain_db
//                                    -> out_gain_db, RELATIVE to base
//   true_peak_ceiling_dbtp           -> lim_on, lim_thresh_db
//   deess_threshold_db / deess_freq_hz -> deess_on, deess_thresh_db, deess_freq,
//                                       deess_range_db (6 dB x amount)
// Everything else (denoise, transient, saturation, stereo width, reverb,
// unknown keys) is reported in `unmappedKeys`.
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
