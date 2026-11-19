#pragma once

#include <JuceHeader.h>
#include <optional>

namespace khensu {

/// Durable, per-session workspace state. File existence is deliberately
/// checked by AppController at restore time because removable drives and
/// render cleanup can make a previously valid path unavailable.
struct WorkspaceRecoveryState {
    juce::String renderPath;
    juce::String referencePath;
    juce::StringArray takes; ///< the vocal Takes bin (WF-5); absent in older saves
    juce::String vocalPath;  ///< the edited current vocal (WF-4); empty = the separated stem
};

inline juce::String workspaceRecoveryStateToJson(const WorkspaceRecoveryState& state)
{
    juce::DynamicObject::Ptr object = new juce::DynamicObject();
    object->setProperty("version", 1);
    object->setProperty("render_path", state.renderPath);
    object->setProperty("reference_path", state.referencePath);
    juce::Array<juce::var> takes;
    for (const auto& t : state.takes)
        takes.add(t);
    object->setProperty("takes", takes);
    object->setProperty("vocal_path", state.vocalPath);
    return juce::JSON::toString(juce::var(object.get()));
}

inline std::optional<WorkspaceRecoveryState> workspaceRecoveryStateFromJson(const juce::String& jsonText)
{
    const auto json = juce::JSON::parse(jsonText);
    const auto* object = json.getDynamicObject();
    if (object == nullptr || static_cast<int>(object->getProperty("version")) != 1) return std::nullopt;

    WorkspaceRecoveryState state{object->getProperty("render_path").toString(),
                                 object->getProperty("reference_path").toString(), {},
                                 object->getProperty("vocal_path").toString()};
    if (const auto* takes = object->getProperty("takes").getArray())
        for (const auto& t : *takes)
            state.takes.add(t.toString());
    return state;
}

} // namespace khensu
