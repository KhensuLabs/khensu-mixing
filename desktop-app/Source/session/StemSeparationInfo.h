// desktop-app/Source/session/StemSeparationInfo.h
// What actually produced a session's stems (audit 2026-11-06 AID-07, desktop
// half). The service writes <stems dir>/separation.json next to the four stem
// files: {"method": "demucs"|"hpss", "reason": "<why it fell back>",
// "model": "<demucs model name>"}. The HPSS fallback writes the SAME file
// names, but its "vocals" is the whole harmonic part of the mix - so a stem
// consumer that only checks the files exist (taxonomy shares, masking, the AI
// deep tier, deep humanize, arrangement/depth) would treat a guess as real
// separated stems. Consumers read this instead.
// THREAD_CONTEXT(AnyThread) - pure file read + parse.
#pragma once
#include <JuceHeader.h>

namespace khensu {

struct StemSeparationInfo {
    juce::String method; ///< "demucs", "hpss", or "" when unknown (no / unreadable separation.json)
    juce::String reason; ///< why the requested method was not used ("" when it was)
    juce::String model;  ///< the Demucs model name, when known

    static constexpr const char* kFileName = "separation.json";

    bool known() const noexcept { return method.isNotEmpty(); }
    /// HPSS (harmonic/percussive split), not a real source separation.
    bool isApproximate() const noexcept { return method.equalsIgnoreCase("hpss"); }

    /// Parses the separation.json text. Missing keys stay empty.
    static StemSeparationInfo fromJson(const juce::String& text)
    {
        StemSeparationInfo info;
        const juce::var v = juce::JSON::parse(text);
        if (auto* o = v.getDynamicObject()) {
            info.method = o->getProperty("method").toString().trim();
            info.reason = o->getProperty("reason").toString().trim();
            info.model = o->getProperty("model").toString().trim();
        }
        return info;
    }

    /// Reads <stemsDir>/separation.json; an empty (unknown) info when absent.
    static StemSeparationInfo readFrom(const juce::File& stemsDir)
    {
        const auto f = stemsDir.getChildFile(kFileName);
        if (!f.existsAsFile()) return {};
        return fromJson(f.loadFileAsString());
    }

    /// One user-facing line describing the stems' provenance. ASCII.
    juce::String describe() const
    {
        if (!known()) return {};
        if (isApproximate())
            return TRANS("Approximate stems (harmonic/percussive split, not AI separation)") +
                   (reason.isNotEmpty() ? juce::String(": ") + reason : juce::String("."));
        return TRANS("AI-separated stems") + (model.isNotEmpty() ? " (" + model + ")" : juce::String()) + ".";
    }
};

} // namespace khensu
