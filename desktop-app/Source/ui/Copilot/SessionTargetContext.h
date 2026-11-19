#pragma once
#include <JuceHeader.h>
#include <khensu/targets/CreativeTarget.h>

namespace khensu::chat_context {

/// WF-11: what the user said they want - Intake's genre, sub-genre, mood,
/// playback intent and vision notes - for the copilot's analysis context.
/// Only the fields the user filled in; a void var when there are none.
inline juce::var sessionTarget(const CreativeTarget& t)
{
    juce::DynamicObject::Ptr target = new juce::DynamicObject();
    auto put = [&target](const char* key, const std::string& value) {
        if (!value.empty()) target->setProperty(key, juce::String::fromUTF8(value.c_str()));
    };
    put("genre", t.primary_genre);
    put("sub_genre", t.sub_genre);
    put("mood", t.mood);
    put("playback_intent", t.playback_intent);
    put("vision", t.vision_statement);
    if (target->getProperties().isEmpty()) return {};
    return juce::var(target.get());
}

} // namespace khensu::chat_context
