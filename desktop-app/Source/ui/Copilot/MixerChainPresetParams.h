// desktop-app/Source/ui/Copilot/MixerChainPresetParams.h
// A Mixer Chain preset's exact DSP parameter states (its "parameter_states"
// object) and how a loaded preset hands them to the plugin chain (FN2-4).
//
// WF-9: one plugin chain. Loading a preset used to post its parameters
// straight to the plugin, beside the Signal Flow Rack that is meant to be the
// one editor that applies a chain. Now the preset's states are staged in the
// one model (lifecycle().stagePluginChain) and the rack applies or discards
// them, like every other surface that proposes a chain.
#pragma once

#include "../../services/IServiceLifecycle.h"
#include <JuceHeader.h>
#include <map>

namespace khensu::mixer_chain_preset {

/// The "parameter_states" object of a preset (empty when it has none).
inline std::map<juce::String, float> parameterStates(const juce::var& preset)
{
    std::map<juce::String, float> out;
    if (auto* obj = preset.getProperty("parameter_states", {}).getDynamicObject())
        for (const auto& p : obj->getProperties())
            if (p.value.isDouble() || p.value.isInt() || p.value.isInt64() || p.value.isBool())
                out[p.name.toString()] = static_cast<float>(static_cast<double>(p.value));
    return out;
}

/// Stages a loaded preset's states for the Signal Flow Rack. Returns false
/// when the preset carries none (nothing is staged).
inline bool stageLoadedPreset(IServiceLifecycle& lifecycle, const std::map<juce::String, float>& params,
                              const juce::String& presetName)
{
    if (params.empty()) return false;
    lifecycle.stagePluginChain(params, TRANS("Mixer Chain preset %s").replace("%s", presetName));
    return true;
}

} // namespace khensu::mixer_chain_preset
