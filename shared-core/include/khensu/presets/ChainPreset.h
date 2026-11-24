// shared-core/include/khensu/presets/ChainPreset.h
// FN2-5: the one preset format for a processing chain, shared by the Copilot
// plugin and the desktop Signal Flow Rack. One JSON file per preset in a
// per-user folder, <data dir>/presets/ (AppPaths::dataDirectory()).
//
// API (namespace khensu::presets, KhensuCommon - no database, no network):
//   ChainPreset p;                       // name, author, source, created, version
//   p.name = "Club master";
//   p.params["comp_on"] = 1.0f;          // plugin DSP parameter id -> PLAIN value
//   p.stages.push_back({"reverb", {{"mix", 0.2f}}}); // optional desktop-only stage
//   juce::File written; juce::String error;
//   save(p, presetsDirectory(), written, error);     // "<name>.chain.json"
//   for (auto& entry : listPresets(presetsDirectory())) entry.preset.name ...;
//   auto loaded = loadPreset(file, &error);          // std::optional<ChainPreset>
//
// The file:
//   { "format": "khensu.chain_preset", "version": 1,
//     "name": "Club master", "author": "", "source": "Khensu Copilot",
//     "created": "2026-11-23T10:00:00.000Z",
//     "params": { "comp_on": 1, "comp_thresh_db": -18.5, ... },
//     "stages": [ { "effect": "reverb", "parameters": { "mix": 0.2 } } ] }
//
// "params" uses the plugin's DSP parameter ids - the same strings as
// mixerchain::DspParamIds / DspParamIdsV3 and KhensuCopilotProcessor::ParamIDs
// - with plain (not normalised) values. A reader applies the ids it knows and
// ignores the rest; the plugin returns every DSP parameter a preset does not
// name to its default. "stages" are EffectSpec-style processors only the
// desktop rack can run (effect name + parameter map); the plugin ignores
// them. Readers ignore unknown keys, load any version (an older file without
// "version", "format" or "stages" loads with defaults; a newer one loads what
// this reader knows), skip non-finite values, and refuse a file that is not
// a JSON object or holds neither params nor stages. Factory presets stay
// built into the plugin; this folder holds the user's own.
// THREAD_CONTEXT(AnyThread) - pure data + file I/O (call file I/O off the
// audio thread).
#pragma once
#include <juce_core/juce_core.h>

#include <map>
#include <optional>
#include <vector>

namespace khensu::presets {

/// Written into every file; readers accept any version.
inline constexpr int kChainPresetVersion = 1;
/// The "format" tag.
inline constexpr const char* kChainPresetFormat = "khensu.chain_preset";
/// File name suffix of a preset in the folder.
inline constexpr const char* kChainPresetExtension = ".chain.json";

/// A desktop-only processor in the chain (EffectSpec-style).
struct ChainStage {
    juce::String effect;
    std::map<juce::String, float> parameters;
    bool operator==(const ChainStage& o) const { return effect == o.effect && parameters == o.parameters; }
};

struct ChainPreset {
    juce::String name;
    juce::String author;  ///< free text, "" = unknown
    juce::String source;  ///< what wrote it: "Khensu Copilot", "Khensu Mixing", "Migrated", ...
    juce::String created; ///< ISO 8601
    int version = kChainPresetVersion;
    /// Plugin DSP parameter id -> plain value.
    std::map<juce::String, float> params;
    std::vector<ChainStage> stages;
};

/// <data dir>/presets, created if absent.
juce::File presetsDirectory();

/// The JSON text of a preset (always written at kChainPresetVersion).
juce::String toJson(const ChainPreset& preset);
/// Parses a preset; nullopt (with `error`) for something that is not one.
std::optional<ChainPreset> fromJson(const juce::String& text, juce::String* error = nullptr);

/// "<legal file name>.chain.json" in `dir`.
juce::File fileFor(const juce::File& dir, const juce::String& name);
/// Writes `preset` (its name must not be blank) into `dir`, replacing a
/// preset of the same name; `created` is filled in when empty.
bool save(const ChainPreset& preset, const juce::File& dir, juce::File& written, juce::String& error);
/// Reads one file; nullopt (with `error`) for a missing / foreign / malformed file.
std::optional<ChainPreset> loadPreset(const juce::File& file, juce::String* error = nullptr);

struct ListedPreset {
    juce::File file;
    ChainPreset preset;
};
/// Every readable preset in `dir`, sorted by name (case-insensitive); files
/// that do not parse are skipped.
std::vector<ListedPreset> listPresets(const juce::File& dir);

} // namespace khensu::presets
