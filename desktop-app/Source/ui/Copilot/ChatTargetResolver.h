#pragma once

#include <JuceHeader.h>
#include <khensu/session/Session.h>
#include <functional>

namespace khensu {

/// Pure target-selection policy for Copilot chain renders (audit AI-02).
/// "mix"/empty resolves to the session's primary file; a named stem must
/// resolve to an existing separated file and never silently fall back to mix.
struct ChatTargetResolution {
    bool ok = false;
    juce::String path;
    juce::String targetStem;
    juce::String error;
};

inline ChatTargetResolution resolveChatTargetStem(
    const Session* session, const juce::String& requestedStem,
    const std::function<juce::File(const juce::String&)>& getStemFile)
{
    ChatTargetResolution r;
    if (session == nullptr || session->files.empty()) {
        r.error = "No session audio to process.";
        return r;
    }

    const juce::String stem = requestedStem.trim().toLowerCase();
    if (stem.isEmpty() || stem == "mix") {
        r.ok = true;
        r.path = juce::String(session->files[0].path);
        r.targetStem = "mix";
        return r;
    }

    const auto stemFile = getStemFile(stem);
    if (!stemFile.existsAsFile()) {
        r.error = "Copilot suggested a \"" + stem +
                  "\" chain, but no separated " + stem + " stem is available for this session - "
                  "run stem separation first, or ask for a mix-level suggestion instead.";
        return r;
    }
    r.ok = true;
    r.path = stemFile.getFullPathName();
    r.targetStem = stem;
    return r;
}

} // namespace khensu
