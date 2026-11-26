#pragma once
#include <khensu/ipc/HttpServiceBridge.h>
#include <map>
#include <string>
#include <vector>

namespace khensu::mixerchain {

// Parameter IDs for plugin-copilot's built-in corrective DSP chain (HPF / LPF
// / peak EQ / compressor / limiter / output gain / chain mix; the v3 / v5
// additions are in RecommendationDspMapper.h). Kept here
// rather than in plugin-copilot so both the plugin and the desktop app can
// map a generated or preset-loaded mixer-chain recipe onto the same param
// set - the desktop pushes the resulting values over
// HttpServiceBridge::postPluginParams, the plugin applies them locally via
// KhensuCopilotProcessor::setParamFromUi. The string values must stay in
// sync with KhensuCopilotProcessor::ParamIDs in plugin-copilot/Source/PluginProcessor.h.
struct DspParamIds {
    static constexpr const char* hpfOn = "hpf_on";
    static constexpr const char* hpfFreq = "hpf_freq";
    static constexpr const char* lpfOn = "lpf_on";
    static constexpr const char* lpfFreq = "lpf_freq";
    static constexpr const char* eqOn = "eq_on";
    static constexpr const char* eqFreq = "eq_freq";
    static constexpr const char* eqGainDb = "eq_gain_db";
    static constexpr const char* eqQ = "eq_q";
    // Bands 2/3 of the corrective EQ stage (see PluginProcessor::ParamIDs) -
    // let a recipe naming multiple EQ moves (e.g. Pultec low+high boost, or a
    // 3-band match-EQ) land on 3 real parametric bands instead of collapsing
    // onto one, which previously discarded all but the largest-gain move.
    static constexpr const char* eq2On = "eq2_on";
    static constexpr const char* eq2Freq = "eq2_freq";
    static constexpr const char* eq2GainDb = "eq2_gain_db";
    static constexpr const char* eq2Q = "eq2_q";
    static constexpr const char* eq3On = "eq3_on";
    static constexpr const char* eq3Freq = "eq3_freq";
    static constexpr const char* eq3GainDb = "eq3_gain_db";
    static constexpr const char* eq3Q = "eq3_q";
    static constexpr const char* compOn = "comp_on";
    static constexpr const char* compThreshDb = "comp_thresh_db";
    static constexpr const char* compRatio = "comp_ratio";
    static constexpr const char* compAttackMs = "comp_attack_ms";
    static constexpr const char* compReleaseMs = "comp_release_ms";
    static constexpr const char* compMultiband = "comp_multiband";
    static constexpr const char* compLowThreshDb = "comp_low_thresh_db";
    static constexpr const char* compMidThreshDb = "comp_mid_thresh_db";
    static constexpr const char* compHighThreshDb = "comp_high_thresh_db";
    static constexpr const char* crossoverLowFreq = "crossover_low_freq";
    static constexpr const char* crossoverHighFreq = "crossover_high_freq";
    static constexpr const char* outGainDb = "out_gain_db";
    static constexpr const char* limOn = "lim_on";
    static constexpr const char* limThreshDb = "lim_thresh_db";
    static constexpr const char* chainMix = "chain_mix";
};

/// STU-17: why a recipe plugin, or one of its EQ moves, did not land on the
/// built-in chain.
enum class RecipeSkipReason {
    NoBuiltInStage,   ///< reverb, delay, pitch, denoise, multiband dynamics... (detail: its category)
    NoUsableSettings, ///< the stage exists, but no value could be read from its settings (detail: category)
    NoFreeEqBand,     ///< an EQ move ranked below six more important ones (detail: the move)
    StageTaken,       ///< an earlier plugin in the recipe already set that stage (detail: that plugin)
};

struct RecipeSkip {
    std::string plugin; ///< the recipe plugin's name (UTF-8)
    RecipeSkipReason reason = RecipeSkipReason::NoBuiltInStage;
    std::string detail; ///< see RecipeSkipReason (UTF-8)
};

struct RecipeDspMapping {
    /// Plain values to write, by parameter id, clamped to the plugin's ranges.
    std::map<std::string, float> values;
    /// Stage names mapped ("HPF", "EQ", "Compressor", "Width", ...), in order.
    std::vector<std::string> mappedStages;
    /// The recipe plugins that set at least one value, in recipe order.
    std::vector<std::string> mappedPlugins;
    /// One line per stage or band, e.g. "EQ band 1: high shelf 10000 Hz +3.0 dB".
    std::vector<std::string> notes;
    /// What could not be mapped, and why.
    std::vector<RecipeSkip> skipped;
};

/// STU-17: maps a third-party plugin recipe (HttpServiceBridge::generateMixerChain,
/// or a saved chain preset) onto the plugin's built-in chain, state v5:
///  - EQ moves from every EQ-category plugin are pooled and ranked by
///    importance (|gain|; a dynamic move's range; a filter or notch counts as
///    6 dB) - the six most important take bands 1-6 in that order, the rest
///    are reported as NoFreeEqBand. Shelves ("Low/High Shelf", "LF/HF/Air"
///    at the band edges) set eqN_type; "notch" and frequency-only cuts
///    ("Sub Cut: 30 Hz", "HF Cut: 16 kHz") set eqN_kind (a cut goes to the
///    HPF / LPF stage first while that is free). A move with a threshold /
///    range, or from a Dynamic EQ, is a dynamic band (eqN_dyn_*); "side" /
///    "(mid)" moves, or Mid / Side moves of an M/S EQ, set eqN_mode.
///  - Compressor, limiter (Ceiling / Output / Threshold), de-esser, HPF / LPF
///    as before; saturation / tape / exciter / harmonic -> sat_* (Drive %
///    is 18 dB at 100 %), transient -> ts_* (Attack / Sustain, % is 12 dB
///    at 100 %), Width % / imager -> width_*, bass mono / mono maker ->
///    bassmono_*. One plugin per single stage: a later one is StageTaken.
///  - A plugin that sets nothing is listed in `skipped`.
RecipeDspMapping mapRecipeToDsp(const std::vector<khensu::ipc::HttpServiceBridge::MixerChainPlugin>& plugins);

/// Approximates a third-party plugin recipe (as returned by
/// HttpServiceBridge::generateMixerChain, or restored from a saved chain
/// preset) onto the built-in corrective DSP chain. Returns a param-id -> value
/// map ready for HttpServiceBridge::postPluginParams / setParamFromUi.
/// `mappedStagesOut`, if non-null, receives the human-readable stage names
/// (e.g. "HPF", "Compressor") that were actually mapped, for status display.
/// The values of mapRecipeToDsp().
std::map<std::string, float>
approximateDspParams(const std::vector<khensu::ipc::HttpServiceBridge::MixerChainPlugin>& plugins,
                     std::vector<std::string>* mappedStagesOut = nullptr);

} // namespace khensu::mixerchain
