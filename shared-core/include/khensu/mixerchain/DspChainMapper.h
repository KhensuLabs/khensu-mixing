#pragma once
#include <khensu/ipc/HttpServiceBridge.h>
#include <map>
#include <string>
#include <vector>

namespace khensu::mixerchain {

// Parameter IDs for plugin-copilot's built-in corrective DSP chain (HPF / LPF
// / peak EQ / compressor / limiter / output gain / chain mix). Kept here
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

/// Approximates a third-party plugin recipe (as returned by
/// HttpServiceBridge::generateMixerChain, or restored from a saved chain
/// preset) onto the built-in corrective DSP chain. Returns a param-id -> value
/// map ready for HttpServiceBridge::postPluginParams / setParamFromUi.
/// `mappedStagesOut`, if non-null, receives the human-readable stage names
/// (e.g. "HPF", "Compressor") that were actually mapped, for status display.
std::map<std::string, float>
approximateDspParams(const std::vector<khensu::ipc::HttpServiceBridge::MixerChainPlugin>& plugins,
                     std::vector<std::string>* mappedStagesOut = nullptr);

} // namespace khensu::mixerchain
