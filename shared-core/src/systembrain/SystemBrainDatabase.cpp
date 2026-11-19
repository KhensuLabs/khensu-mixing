// THREAD_CONTEXT(WorkerThread) for load*; THREAD_CONTEXT(AnyThread) for queries.
#include <khensu/systembrain/SystemBrainDatabase.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace khensu {

namespace {
std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string getString(const juce::var& v, const char* key) { return v[key].toString().toStdString(); }

std::vector<std::string> getStringArray(const juce::var& v, const char* key)
{
    std::vector<std::string> out;
    if (const auto* arr = v[key].getArray())
        for (const auto& item : *arr)
            out.push_back(item.toString().toStdString());
    return out;
}

FeatureVisualization getVisualization(const juce::var& v)
{
    FeatureVisualization viz;
    const juce::var vizVar = v["visualization"];
    if (!vizVar.isObject()) return viz;

    viz.type = vizVar["type"].toString().toStdString();
    if (viz.type.empty()) viz.type = "none";
    viz.min = (float)vizVar["min"];
    viz.max = (float)vizVar["max"];
    viz.icon = vizVar["icon"].toString().toStdString();
    if (const auto* arr = vizVar["labels"].getArray())
        for (const auto& item : *arr)
            viz.labels.push_back(item.toString().toStdString());
    return viz;
}
} // namespace

Result<void> SystemBrainDatabase::loadFromString(const std::string& jsonContent) { return parseJson(jsonContent); }

Result<void> SystemBrainDatabase::loadFromFile(const std::string& filePath)
{
    // UTF-8 path via juce::File (std::ifstream(std::string) uses the ANSI code
    // page on MSVC and fails for non-ASCII user folders).
    const juce::File file(juce::String::fromUTF8(filePath.c_str()));
    if (!file.existsAsFile()) return Result<void>::err("System Brain file not found: " + filePath);
    return parseJson(file.loadFileAsString().toStdString());
}

std::optional<FeatureEntry> SystemBrainDatabase::findById(const std::string& id) const
{
    auto it = m_byId.find(id);
    if (it != m_byId.end()) return it->second;
    return std::nullopt;
}

std::vector<FeatureEntry> SystemBrainDatabase::search(const std::string& query) const
{
    if (query.empty()) return m_ordered;
    const std::string q = toLower(query);
    std::vector<FeatureEntry> out;
    for (const auto& e : m_ordered) {
        bool match = toLower(e.name).find(q) != std::string::npos ||
                     toLower(e.short_description).find(q) != std::string::npos ||
                     toLower(e.category).find(q) != std::string::npos;
        if (!match)
            for (const auto& tag : e.tags)
                if (toLower(tag).find(q) != std::string::npos) {
                    match = true;
                    break;
                }
        if (match) out.push_back(e);
    }
    return out;
}

std::vector<FeatureEntry> SystemBrainDatabase::filterByCategory(const std::string& category) const
{
    const std::string c = toLower(category);
    std::vector<FeatureEntry> out;
    for (const auto& e : m_ordered)
        if (toLower(e.category) == c) out.push_back(e);
    return out;
}

std::vector<std::string> SystemBrainDatabase::categories() const
{
    std::vector<std::string> out;
    for (const auto& e : m_ordered)
        if (std::find(out.begin(), out.end(), e.category) == out.end()) out.push_back(e.category);
    return out;
}

Result<void> SystemBrainDatabase::parseJson(const std::string& jsonContent)
{
    juce::var root = juce::JSON::parse(juce::String(jsonContent));
    if (!root.isObject()) return Result<void>::err("Feature encyclopedia root is not an object.");

    const juce::var featuresVar = root["features"];
    if (!featuresVar.isArray()) return Result<void>::err("Feature encyclopedia missing 'features' array.");

    m_byId.clear();
    m_ordered.clear();

    for (int i = 0; i < featuresVar.size(); ++i) {
        const juce::var& e = featuresVar[i];

        FeatureEntry f;
        f.id = getString(e, "id");
        f.name = getString(e, "name");
        f.category = getString(e, "category");
        f.short_description = getString(e, "short_description");
        f.detailed_description = getString(e, "detailed_description");
        f.logic_and_math = getString(e, "logic_and_math");
        f.why_it_exists = getString(e, "why_it_exists");
        f.when_to_use = getString(e, "when_to_use");
        f.desktop_location = getString(e, "desktop_location");
        f.plugin_available = (bool)e["plugin_available"];
        f.inputs = getStringArray(e, "inputs");
        f.tags = getStringArray(e, "tags");
        f.related_features = getStringArray(e, "related_features");
        f.visualization = getVisualization(e);

        if (!f.id.empty()) {
            m_byId[f.id] = f;
            m_ordered.push_back(std::move(f));
        }
    }

    m_loaded = true;
    return Result<void>::ok();
}

} // namespace khensu
