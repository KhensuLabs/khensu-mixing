// desktop-app/Source/ui/MixMasterEngine/PresetStore.h
// UI-9 / FN2-5: the chain presets the Preset Library card and the Mix /
// Master preset bar share - the one shared preset format
// (khensu/presets/ChainPreset.h) in the shared folder <data dir>/presets, so
// a preset saved here is in the Copilot plugin's preset menu and one saved in
// the plugin is here. A preset holds the plugin rack's DSP parameters
// ("params", applied through the one plugin chain, WF-9) and / or the
// desktop-only processors it renders ("stages", EffectSpec-style, rendered
// as a new version, WF-1). Plus a few factory chains that ship with the app
// (never written to disk, never deleted).
//
// The old desktop store (<data dir>/effect_chain_presets, {schema_version,
// name, saved_at, effects: [{effect, parameters}]}) is migrated once into the
// shared folder: every old file becomes a preset with its effects as stages
// (one that already exists there is left alone), the old files are kept, and
// a marker file in the old folder records that it ran.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <khensu/common/AppPaths.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/presets/ChainPreset.h>
#include <algorithm>
#include <map>
#include <optional>
#include <vector>

namespace khensu::preset_store {

using Chain = std::vector<ipc::HttpServiceBridge::EffectSpec>;
using Params = std::map<juce::String, float>;

struct Entry {
    juce::String name;
    juce::String fileName; ///< the file's name in the folder ("" for a factory preset)
    juce::String savedAt;
    juce::String source; ///< what wrote it ("Khensu Copilot", "Khensu Mixing", "Migrated")
    int effectCount = 0; ///< desktop stages
    int paramCount = 0;  ///< plugin rack parameters
    bool factory = false;
};

/// The marker the one-time migration leaves in the old folder.
inline constexpr const char* kMigratedMarker = ".migrated_to_presets";

inline juce::File legacyDirectory()
{
    return juce::File(juce::String(AppPaths::dataDirectory())).getChildFile("effect_chain_presets");
}

inline ipc::HttpServiceBridge::EffectSpec effect(const char* name,
                                                 std::initializer_list<std::pair<const char*, float>> params)
{
    ipc::HttpServiceBridge::EffectSpec fx;
    fx.effect = name;
    for (const auto& [k, v] : params)
        fx.parameters[k] = v;
    return fx;
}

/// Factory chains: gentle, conventional starting points.
inline const std::vector<std::pair<juce::String, Chain>>& factory()
{
    static const std::vector<std::pair<juce::String, Chain>> kFactory{
        {"Gentle glue", {effect("GlueCompressor", {{"threshold_db", -18.0f}, {"ratio", 2.0f}, {"attack_ms", 10.0f}})}},
        {"Air lift", {effect("HighShelfFilter", {{"cutoff_frequency_hz", 10000.0f}, {"gain_db", 1.5f}, {"q", 0.707f}})}},
        {"Low-end tidy", {effect("HighpassFilter", {{"cutoff_frequency_hz", 30.0f}})}},
        {"True-peak safety", {effect("Limiter", {{"threshold_db", -1.0f}})}},
    };
    return kFactory;
}

inline std::vector<presets::ChainStage> stagesOf(const Chain& chain)
{
    std::vector<presets::ChainStage> stages;
    for (const auto& fx : chain) {
        presets::ChainStage st;
        st.effect = fx.effect;
        for (const auto& [k, v] : fx.parameters)
            st.parameters[juce::String(k)] = v;
        stages.push_back(std::move(st));
    }
    return stages;
}

inline Chain chainOf(const presets::ChainPreset& preset)
{
    Chain chain;
    for (const auto& st : preset.stages) {
        if (st.effect.isEmpty()) continue;
        ipc::HttpServiceBridge::EffectSpec fx;
        fx.effect = st.effect;
        for (const auto& [k, v] : st.parameters)
            fx.parameters[k] = v;
        chain.push_back(std::move(fx));
    }
    return chain;
}

/// The old desktop file's effects (schema_version 1).
inline Chain legacyChainOf(const juce::var& json)
{
    Chain chain;
    if (auto* obj = json.getDynamicObject())
        if (auto* arr = obj->getProperty("effects").getArray())
            for (const auto& fxVar : *arr) {
                ipc::HttpServiceBridge::EffectSpec fx;
                fx.effect = fxVar["effect"].toString();
                if (auto* params = fxVar["parameters"].getDynamicObject())
                    for (const auto& prop : params->getProperties())
                        fx.parameters[prop.name.toString()] = static_cast<float>(static_cast<double>(prop.value));
                if (fx.effect.isNotEmpty()) chain.push_back(std::move(fx));
            }
    return chain;
}

/// Copies every old-format preset in `legacyDir` into `dir` once; returns how
/// many it wrote. The old files stay; a marker in `legacyDir` stops a rerun.
inline int migrateLegacy(const juce::File& legacyDir, const juce::File& dir)
{
    if (!legacyDir.isDirectory() || legacyDir.getChildFile(kMigratedMarker).existsAsFile()) return 0;
    int written = 0;
    for (const auto& file : legacyDir.findChildFiles(juce::File::findFiles, false, "*.json")) {
        const auto json = juce::JSON::parse(file.loadFileAsString());
        auto* obj = json.getDynamicObject();
        if (obj == nullptr) continue;
        presets::ChainPreset p;
        p.name = obj->getProperty("name").toString().trim();
        if (p.name.isEmpty()) p.name = file.getFileNameWithoutExtension();
        p.source = "Migrated";
        p.created = obj->getProperty("saved_at").toString();
        p.stages = stagesOf(legacyChainOf(json));
        if (p.stages.empty()) continue;
        if (presets::fileFor(dir, p.name).existsAsFile()) continue; // never overwrite a shared preset
        juce::File out;
        juce::String error;
        if (presets::save(p, dir, out, error)) ++written;
    }
    legacyDir.getChildFile(kMigratedMarker)
        .replaceWithText("Migrated to " + dir.getFullPathName() + " on " +
                         juce::Time::getCurrentTime().toISO8601(true) + ". The files here are kept as a backup.\n");
    return written;
}

/// The shared folder, after the one-time migration of the old desktop store.
inline juce::File directory()
{
    auto dir = presets::presetsDirectory();
    migrateLegacy(legacyDirectory(), dir);
    return dir;
}

/// `dir`, or the shared folder when none is given (the default arguments
/// below; a factory preset never touches the disk).
inline juce::File resolve(const juce::File& dir) { return dir == juce::File() ? directory() : dir; }

/// Saved presets in `dir`, newest first.
inline std::vector<Entry> list(const juce::File& dir = {})
{
    std::vector<Entry> out;
    for (const auto& listed : presets::listPresets(resolve(dir))) {
        Entry e;
        e.name = listed.preset.name.isNotEmpty() ? listed.preset.name : listed.file.getFileName();
        e.fileName = listed.file.getFileName();
        e.savedAt = listed.preset.created;
        e.source = listed.preset.source;
        e.effectCount = static_cast<int>(listed.preset.stages.size());
        e.paramCount = static_cast<int>(listed.preset.params.size());
        out.push_back(std::move(e));
    }
    std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.savedAt > b.savedAt; });
    return out;
}

/// Writes `chain` (desktop stages) and `params` (the rack) as `name`,
/// replacing a preset of the same name; the entry written, or nothing when
/// the file could not be written or there is nothing to save.
inline std::optional<Entry> save(const juce::String& name, const Chain& chain, const Params& params = {},
                                 const juce::File& dir = {})
{
    if ((chain.empty() && params.empty()) || name.trim().isEmpty()) return std::nullopt;
    presets::ChainPreset p;
    p.name = name.trim();
    p.source = "Khensu Mixing";
    p.params = params;
    p.stages = stagesOf(chain);
    juce::File written;
    juce::String error;
    if (!presets::save(p, resolve(dir), written, error)) return std::nullopt;
    Entry e;
    e.name = p.name;
    e.fileName = written.getFileName();
    e.source = p.source;
    e.effectCount = static_cast<int>(p.stages.size());
    e.paramCount = static_cast<int>(p.params.size());
    return e;
}

inline std::optional<presets::ChainPreset> read(const Entry& e, const juce::File& dir = {})
{
    if (e.factory) {
        for (const auto& [name, chain] : factory())
            if (name == e.name) {
                presets::ChainPreset p;
                p.name = name;
                p.stages = stagesOf(chain);
                return p;
            }
        return std::nullopt;
    }
    return presets::loadPreset(resolve(dir).getChildFile(e.fileName));
}

/// The preset's desktop stages (empty for a rack-only preset).
inline Chain load(const Entry& e, const juce::File& dir = {})
{
    const auto p = read(e, dir);
    return p ? chainOf(*p) : Chain{};
}

/// The preset's plugin rack parameters (empty for a stages-only preset).
inline Params loadParams(const Entry& e, const juce::File& dir = {})
{
    const auto p = read(e, dir);
    return p ? p->params : Params{};
}

inline bool remove(const Entry& e, const juce::File& dir = {})
{
    return !e.factory && e.fileName.isNotEmpty() && resolve(dir).getChildFile(e.fileName).deleteFile();
}

/// "3 stages, rack" - what a preset holds, for a row or a menu.
inline juce::String describe(const Entry& e)
{
    juce::StringArray parts;
    if (e.effectCount > 0) parts.add(juce::String(e.effectCount) + " " + (e.effectCount == 1 ? TRANS("stage") : TRANS("stages")));
    if (e.paramCount > 0) parts.add(TRANS("rack settings"));
    if (e.source.isNotEmpty() && !e.factory) parts.add(e.source);
    return parts.joinIntoString(", ");
}

/// The index after `current` in a list of `count`, wrapping, `step` = +1 / -1.
inline int stepIndex(int current, int count, int step) noexcept
{
    if (count <= 0) return -1;
    if (current < 0) return step > 0 ? 0 : count - 1;
    return ((current + step) % count + count) % count;
}

} // namespace khensu::preset_store
