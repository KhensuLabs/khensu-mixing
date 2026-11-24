// plugin-copilot/Source/CopilotPresets.h
// UI-9: the Copilot's factory presets (exposed to hosts as programs) and its
// user presets - since FN2-5 the shared chain-preset JSON files in
// <data dir>/presets (khensu/presets/ChainPreset.h).
// A preset sets EVERY built-in DSP parameter: what it does not name returns
// to the parameter's default, so loading one never inherits stale stages.
#pragma once
#include <JuceHeader.h>
#include <khensu/presets/ChainPreset.h>

#include <map>
#include <string>
#include <vector>

namespace khensu::copilot::presets {

struct FactoryPreset {
    const char* name;
    /// Plain values by parameter id; unnamed parameters use their defaults.
    std::vector<std::pair<const char*, float>> values;
};

/// The factory presets, in program order (program 0 is the flat default).
const std::vector<FactoryPreset>& factoryPresets();
int numFactoryPresets() noexcept;

/// User presets are shared chain presets (FN2-5, khensu/presets/ChainPreset.h)
/// in <data dir>/presets, one JSON file each, readable by the desktop rack.
/// Before that they were XML files in <data dir>/Copilot Presets; these
/// read and migrate those.
inline constexpr const char* kLegacyUserPresetExtension = ".khensupreset";

/// Reads a legacy XML preset; false (with `error`) for a missing / foreign / malformed file.
bool readLegacyUserPreset(const juce::File& file, juce::String& name, std::map<juce::String, float>& values,
                          juce::String& error);
/// Copies every legacy preset in `legacyDir` into the shared folder (a preset
/// already there by that name is kept) and renames each migrated original to
/// "<file>.migrated". Returns how many were written; cheap when the legacy
/// folder does not exist.
int migrateLegacyUserPresets(const juce::File& legacyDir, const juce::File& sharedDir);

} // namespace khensu::copilot::presets
