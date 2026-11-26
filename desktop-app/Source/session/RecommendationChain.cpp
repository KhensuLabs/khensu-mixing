// desktop-app/Source/session/RecommendationChain.cpp
// Moved out of RenderService::applyRecommendationChain (P0-2) so the Dashboard
// card can tell a renderable finding from an advice-only one.
#include "RecommendationChain.h"
#include <khensu/recommendations/MaskingChain.h>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace khensu {

Recommendation scaledRecommendation(const Recommendation& rec, float amount)
{
    Recommendation out = rec;
    for (auto& [key, value] : out.suggested_parameters) {
        const bool isDb = key.size() > 3 && key.compare(key.size() - 3, 3, "_db") == 0;
        const bool isLevel = key.find("threshold") != std::string::npos;
        if (isDb && !isLevel)
            value *= amount;
        else if (key == "comp_ratio" || key == "stereo_width")
            value = 1.0f + (value - 1.0f) * amount;
    }
    return out;
}

std::vector<ipc::HttpServiceBridge::EffectSpec> buildRecommendationChain(const Recommendation& rec)
{
    const auto& p = rec.suggested_parameters;
    auto has = [&](const char* k) { return p.count(k) != 0; };
    auto get = [&](const char* k, float def = 0.0f) {
        auto it = p.find(k);
        return it != p.end() ? it->second : def;
    };

    std::vector<ipc::HttpServiceBridge::EffectSpec> chain;
    auto add = [&chain](const char* effect, std::initializer_list<std::pair<const char*, float>> params) {
        ipc::HttpServiceBridge::EffectSpec e;
        e.effect = effect;
        for (const auto& [name, value] : params)
            e.parameters[name] = value;
        chain.push_back(std::move(e));
    };

    // Parameter keys the Feedback Decoder emits besides the EQ/comp/gain set
    // (copilot/feedback_decoder.py), mapped onto the render's effect vocabulary
    // (dsp.py PARAM_RANGES) - unmapped keys were silently dropped.
    if (has("highpass_hz")) add("HighpassFilter", {{"cutoff_frequency_hz", get("highpass_hz")}});

    // ENH-6: a stem-masking finding renders as a dynamic EQ (keyed on the
    // leading stem when the render is given it) instead of a static cut.
    const auto dynamicEq = maskingDynamicEq(rec);
    if (dynamicEq) {
        ipc::HttpServiceBridge::EffectSpec dyn;
        dyn.effect = dynamicEq->stage.effect;
        for (const auto& [name, value] : dynamicEq->stage.parameters) dyn.parameters[name] = value;
        chain.push_back(std::move(dyn));
    }
    if (!dynamicEq && has("eq_freq_hz") && has("eq_gain_db")) {
        ipc::HttpServiceBridge::EffectSpec eq;
        // eq_type (EqCurveComponent's codes): 1 = high shelf, 2 = low shelf,
        // else a bell. The tonal findings advise a shelf; rendering a bell at
        // the shelf's corner moved the band the finding measures by a
        // fraction of the gain (MEAS-4 fix check).
        const int eqType = has("eq_type") ? static_cast<int>(std::lround(get("eq_type"))) : 0;
        eq.effect = eqType == 1 ? "HighShelfFilter" : eqType == 2 ? "LowShelfFilter" : "PeakFilter";
        eq.parameters["cutoff_frequency_hz"] = get("eq_freq_hz");
        eq.parameters["gain_db"] = get("eq_gain_db");
        eq.parameters["q"] = has("eq_q") ? get("eq_q") : 0.707f;
        chain.push_back(std::move(eq));
    }
    // A resonance notch (ResonanceRecommender): a cut at the peak.
    if (has("eq_center_hz") && has("eq_cut_db") && !has("eq_gain_db")) {
        ipc::HttpServiceBridge::EffectSpec notch;
        notch.effect = "PeakFilter";
        notch.parameters["cutoff_frequency_hz"] = get("eq_center_hz");
        notch.parameters["gain_db"] = -std::abs(get("eq_cut_db"));
        notch.parameters["q"] = has("eq_q") ? get("eq_q") : 6.0f;
        chain.push_back(std::move(notch));
    }
    if (has("comp_ratio") || has("comp_threshold_db")) {
        ipc::HttpServiceBridge::EffectSpec comp;
        comp.effect = "Compressor";
        comp.parameters["threshold_db"] = has("comp_threshold_db") ? get("comp_threshold_db") : -18.0f;
        comp.parameters["ratio"] = has("comp_ratio") ? get("comp_ratio") : 2.0f;
        comp.parameters["attack_ms"] = has("comp_attack_ms") ? get("comp_attack_ms") : 5.0f;
        comp.parameters["release_ms"] = has("comp_release_ms") ? get("comp_release_ms") : 100.0f;
        chain.push_back(std::move(comp));
    }
    if (has("deess_threshold_db") || has("deess_freq_hz"))
        add("DeEsser", {{"threshold_db", has("deess_threshold_db") ? get("deess_threshold_db") : -22.0f},
                        {"frequency_hz", has("deess_freq_hz") ? get("deess_freq_hz") : 7000.0f}});
    // FIX-B: steady hiss - the service's spectral denoiser.
    if (has("denoise_reduction_db")) add("SpectralDenoise", {{"reduction_db", get("denoise_reduction_db")}});
    if (has("transient_attack_db")) add("TransientShaper", {{"attack_gain_db", get("transient_attack_db")}});
    if (has("saturation_drive_db")) add("TapeSaturation", {{"drive_db", get("saturation_drive_db")}});
    if (has("stereo_width")) add("StereoWidener", {{"width", get("stereo_width")}});
    // STU-18: a sub too wide - mono below the crossover.
    if (has("bass_mono_hz")) add("BassMonoMaker", {{"crossover_hz", get("bass_mono_hz")}});
    // reverb_wet / reverb_size / reverb_predelay_ms describe a reverb SEND
    // setting ("less reverb" lowers it); adding a Reverb to the bounced file
    // would do the opposite, so those stay advice-only.
    {
        float gainDb = 0.0f;
        bool hasGain = false;
        if (has("limiter_gain_db")) { // "push the limiter": drive into the ceiling below
            gainDb += std::abs(get("limiter_gain_db"));
            hasGain = true;
        }
        if (has("comp_makeup_gain_db")) {
            gainDb += get("comp_makeup_gain_db");
            hasGain = true;
        }
        if (has("reduce_gain_db")) {
            gainDb -= std::abs(get("reduce_gain_db"));
            hasGain = true;
        }
        if (has("increase_gain_db")) {
            gainDb += std::abs(get("increase_gain_db"));
            hasGain = true;
        }
        if (hasGain) {
            ipc::HttpServiceBridge::EffectSpec gain;
            gain.effect = "Gain";
            gain.parameters["gain_db"] = gainDb;
            chain.push_back(std::move(gain));
        }
    }
    if (has("true_peak_ceiling_dbtp")) {
        ipc::HttpServiceBridge::EffectSpec lim;
        lim.effect = "Limiter";
        lim.parameters["threshold_db"] = get("true_peak_ceiling_dbtp");
        chain.push_back(std::move(lim));
    }

    return chain;
}

} // namespace khensu
