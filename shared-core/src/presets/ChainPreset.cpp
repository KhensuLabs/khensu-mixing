#include <khensu/presets/ChainPreset.h>
#include <khensu/common/AppPaths.h>

#include <algorithm>
#include <cmath>

namespace khensu::presets {

namespace {
void setError(juce::String* error, const juce::String& text)
{
    if (error != nullptr) *error = text;
}

/// Finite numbers (and bools) of a JSON object, by key.
std::map<juce::String, float> readValueMap(const juce::var& object)
{
    std::map<juce::String, float> values;
    const auto* dynamic = object.getDynamicObject();
    if (dynamic == nullptr) return values;
    for (const auto& property : dynamic->getProperties()) {
        const auto& v = property.value;
        if (!(v.isDouble() || v.isInt() || v.isInt64() || v.isBool())) continue;
        const double d = static_cast<double>(v);
        if (!std::isfinite(d)) continue;
        const auto key = property.name.toString();
        if (key.isNotEmpty()) values[key] = static_cast<float>(d);
    }
    return values;
}

juce::var writeValueMap(const std::map<juce::String, float>& values)
{
    juce::DynamicObject::Ptr object = new juce::DynamicObject();
    for (const auto& [key, value] : values)
        if (key.isNotEmpty() && std::isfinite(value)) object->setProperty(key, static_cast<double>(value));
    return juce::var(object.get());
}
} // namespace

juce::File presetsDirectory()
{
    auto dir = juce::File(juce::String(juce::CharPointer_UTF8(AppPaths::dataDirectory().c_str()))).getChildFile("presets");
    if (!dir.isDirectory()) dir.createDirectory();
    return dir;
}

juce::String toJson(const ChainPreset& preset)
{
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("format", kChainPresetFormat);
    root->setProperty("version", kChainPresetVersion);
    root->setProperty("name", preset.name);
    root->setProperty("author", preset.author);
    root->setProperty("source", preset.source);
    root->setProperty("created", preset.created);
    root->setProperty("params", writeValueMap(preset.params));
    juce::Array<juce::var> stages;
    for (const auto& stage : preset.stages) {
        if (stage.effect.isEmpty()) continue;
        juce::DynamicObject::Ptr s = new juce::DynamicObject();
        s->setProperty("effect", stage.effect);
        s->setProperty("parameters", writeValueMap(stage.parameters));
        stages.add(juce::var(s.get()));
    }
    root->setProperty("stages", stages);
    return juce::JSON::toString(juce::var(root.get()));
}

std::optional<ChainPreset> fromJson(const juce::String& text, juce::String* error)
{
    juce::var root;
    const auto parsed = juce::JSON::parse(text, root);
    if (parsed.failed() || !root.isObject()) {
        setError(error, "Not a chain preset (no JSON object).");
        return std::nullopt;
    }
    // A foreign JSON document that says what it is.
    const auto format = root.getProperty("format", juce::var()).toString();
    if (format.isNotEmpty() && format != kChainPresetFormat) {
        setError(error, "Not a chain preset (format \"" + format + "\").");
        return std::nullopt;
    }
    ChainPreset preset;
    const auto version = root.getProperty("version", juce::var());
    preset.version = version.isVoid() ? 0 : static_cast<int>(version); // 0 = before the field existed
    preset.name = root.getProperty("name", juce::var()).toString().trim();
    preset.author = root.getProperty("author", juce::var()).toString();
    preset.source = root.getProperty("source", juce::var()).toString();
    preset.created = root.getProperty("created", juce::var()).toString();
    preset.params = readValueMap(root.getProperty("params", juce::var()));
    if (const auto* stages = root.getProperty("stages", juce::var()).getArray())
        for (const auto& s : *stages) {
            if (!s.isObject()) continue;
            ChainStage stage;
            stage.effect = s.getProperty("effect", juce::var()).toString().trim();
            stage.parameters = readValueMap(s.getProperty("parameters", juce::var()));
            if (stage.effect.isNotEmpty()) preset.stages.push_back(std::move(stage));
        }
    if (preset.params.empty() && preset.stages.empty()) {
        setError(error, "The preset holds no parameters.");
        return std::nullopt;
    }
    return preset;
}

juce::File fileFor(const juce::File& dir, const juce::String& name)
{
    auto stem = juce::File::createLegalFileName(name.trim());
    if (stem.isEmpty()) stem = "Preset";
    return dir.getChildFile(stem + kChainPresetExtension);
}

bool save(const ChainPreset& preset, const juce::File& dir, juce::File& written, juce::String& error)
{
    if (preset.name.trim().isEmpty()) {
        error = "A preset needs a name.";
        return false;
    }
    if (!dir.isDirectory() && !dir.createDirectory()) {
        error = "Could not create the preset folder.";
        return false;
    }
    ChainPreset copy = preset;
    copy.name = preset.name.trim();
    if (copy.created.isEmpty()) copy.created = juce::Time::getCurrentTime().toISO8601(true);
    written = fileFor(dir, copy.name);
    // Write beside it and swap, so a crash never leaves half a preset.
    juce::TemporaryFile temp(written);
    if (!temp.getFile().replaceWithText(toJson(copy)) || !temp.overwriteTargetFileWithTemporary()) {
        error = "Could not write the preset file.";
        return false;
    }
    return true;
}

std::optional<ChainPreset> loadPreset(const juce::File& file, juce::String* error)
{
    if (!file.existsAsFile()) {
        setError(error, "The preset file is missing.");
        return std::nullopt;
    }
    // Presets are small; anything huge is not one.
    if (file.getSize() > 4 * 1024 * 1024) {
        setError(error, "Not a chain preset (too large).");
        return std::nullopt;
    }
    auto preset = fromJson(file.loadFileAsString(), error);
    if (preset.has_value() && preset->name.isEmpty())
        preset->name = file.getFileName().upToFirstOccurrenceOf(kChainPresetExtension, false, true);
    return preset;
}

std::vector<ListedPreset> listPresets(const juce::File& dir)
{
    std::vector<ListedPreset> list;
    if (!dir.isDirectory()) return list;
    for (const auto& file :
         dir.findChildFiles(juce::File::findFiles, false, juce::String("*") + kChainPresetExtension)) {
        if (auto preset = loadPreset(file)) list.push_back({file, std::move(*preset)});
    }
    std::sort(list.begin(), list.end(), [](const ListedPreset& a, const ListedPreset& b) {
        const int byName = a.preset.name.compareIgnoreCase(b.preset.name);
        return byName != 0 ? byName < 0 : a.file.getFileName() < b.file.getFileName();
    });
    return list;
}

} // namespace khensu::presets
