// plugin-copilot/Source/CopilotPresets.cpp
#include "CopilotPresets.h"

#include <cmath>

namespace khensu::copilot::presets {

const std::vector<FactoryPreset>& factoryPresets()
{
    // Program 0 matches the parameter defaults, so a fresh instance reports
    // the program it is actually in.
    static const std::vector<FactoryPreset> presets{
        {"Bypass (flat)", {}},
        {"Streaming master",
         {{"hpf_on", 1.0f},
          {"hpf_freq", 25.0f},
          {"comp_on", 1.0f},
          {"comp_thresh_db", -20.0f},
          {"comp_ratio", 2.0f},
          {"comp_attack_ms", 30.0f},
          {"comp_release_ms", 200.0f},
          {"lim_on", 1.0f},
          {"lim_thresh_db", -1.0f}}},
        {"Club master",
         {{"hpf_on", 1.0f},
          {"hpf_freq", 30.0f},
          {"comp_on", 1.0f},
          {"comp_multiband", 1.0f},
          {"comp_low_thresh_db", -22.0f},
          {"comp_mid_thresh_db", -20.0f},
          {"comp_high_thresh_db", -22.0f},
          {"comp_ratio", 2.5f},
          {"comp_attack_ms", 10.0f},
          {"comp_release_ms", 120.0f},
          {"out_gain_db", 2.0f},
          {"lim_on", 1.0f},
          {"lim_thresh_db", -0.5f}}},
        {"Podcast voice",
         {{"hpf_on", 1.0f},
          {"hpf_freq", 80.0f},
          {"eq_on", 1.0f},
          {"eq_type", 1.0f},
          {"eq_freq", 200.0f},
          {"eq_gain_db", -2.0f},
          {"eq_q", 0.71f},
          {"eq2_on", 1.0f},
          {"eq2_freq", 3000.0f},
          {"eq2_gain_db", 2.0f},
          {"eq2_q", 1.0f},
          {"deess_on", 1.0f},
          {"deess_freq", 6500.0f},
          {"deess_thresh_db", -30.0f},
          {"deess_range_db", 6.0f},
          {"comp_on", 1.0f},
          {"comp_thresh_db", -24.0f},
          {"comp_ratio", 3.0f},
          {"comp_attack_ms", 5.0f},
          {"comp_release_ms", 80.0f},
          {"out_gain_db", 3.0f},
          {"lim_on", 1.0f},
          {"lim_thresh_db", -1.0f}}},
        {"Vocal clean-up",
         {{"hpf_on", 1.0f},
          {"hpf_freq", 100.0f},
          {"eq_on", 1.0f},
          {"eq_freq", 300.0f},
          {"eq_gain_db", -2.5f},
          {"eq_q", 1.2f},
          {"eq2_on", 1.0f},
          {"eq2_type", 2.0f},
          {"eq2_freq", 10000.0f},
          {"eq2_gain_db", 1.5f},
          {"eq2_q", 0.71f},
          {"deess_on", 1.0f},
          {"deess_freq", 7000.0f},
          {"deess_thresh_db", -32.0f},
          {"deess_range_db", 8.0f},
          {"comp_on", 1.0f},
          {"comp_thresh_db", -20.0f},
          {"comp_ratio", 2.5f},
          {"comp_attack_ms", 8.0f},
          {"comp_release_ms", 100.0f}}},
        {"Gentle glue",
         {{"comp_on", 1.0f},
          {"comp_thresh_db", -18.0f},
          {"comp_ratio", 1.5f},
          {"comp_attack_ms", 30.0f},
          {"comp_release_ms", 250.0f}}},
    };
    return presets;
}

int numFactoryPresets() noexcept
{
    return static_cast<int>(factoryPresets().size());
}

namespace {
const juce::Identifier kRootType{"KhensuCopilotPreset"};
} // namespace

bool readLegacyUserPreset(const juce::File& file, juce::String& name, std::map<juce::String, float>& values,
                          juce::String& error)
{
    values.clear();
    auto xml = juce::parseXML(file);
    if (xml == nullptr || !xml->hasTagName(kRootType.toString())) {
        error = TRANS("Not a Khensu Copilot preset.");
        return false;
    }
    name = xml->getStringAttribute("name", file.getFileNameWithoutExtension());
    for (auto* param : xml->getChildWithTagNameIterator("PARAM")) {
        const auto id = param->getStringAttribute("id");
        const auto value = static_cast<float>(param->getDoubleAttribute("value", std::nan("")));
        if (id.isNotEmpty() && std::isfinite(value)) values[id] = value;
    }
    if (values.empty()) {
        error = TRANS("The preset holds no parameters.");
        return false;
    }
    return true;
}

int migrateLegacyUserPresets(const juce::File& legacyDir, const juce::File& sharedDir)
{
    if (!legacyDir.isDirectory()) return 0;
    int migrated = 0;
    for (const auto& file :
         legacyDir.findChildFiles(juce::File::findFiles, false, juce::String("*") + kLegacyUserPresetExtension)) {
        juce::String name, error;
        std::map<juce::String, float> values;
        if (!readLegacyUserPreset(file, name, values, error)) continue; // a foreign file stays where it is
        const auto target = khensu::presets::fileFor(sharedDir, name);
        if (!target.existsAsFile()) {
            khensu::presets::ChainPreset preset;
            preset.name = name;
            preset.source = "Khensu Copilot (migrated)";
            preset.created = file.getLastModificationTime().toISO8601(true);
            preset.params = std::move(values);
            juce::File written;
            if (!khensu::presets::save(preset, sharedDir, written, error)) continue; // try again next time
            ++migrated;
        }
        // Retired, not deleted: "<name>.khensupreset.migrated" (backup-first).
        file.moveFileTo(file.getSiblingFile(file.getFileName() + ".migrated"));
    }
    return migrated;
}

} // namespace khensu::copilot::presets
