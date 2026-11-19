// desktop-app/Source/ui/shared/DspDomainClassifier.h
#pragma once
#include <JuceHeader.h>
#include <khensu/recommendations/Recommendation.h>

namespace khensu {

/// Which DSP domain a recommendation belongs to, for UI grouping purposes
/// only (Recommendation has no explicit domain field - ProcessingStage
/// captures *production workflow* stage (arrangement/mix/master), a
/// different axis from "which knob domain": EQ vs compression vs stereo).
enum class DspDomain { ClipInput, Spectral, Dynamics, Stereo, LoudnessOutput, None };

inline const char* to_string(DspDomain d) noexcept
{
    switch (d) {
    case DspDomain::ClipInput:
        return "Clip Guard / Input";
    case DspDomain::Spectral:
        return "Spectral / EQ";
    case DspDomain::Dynamics:
        return "Dynamics / Compression";
    case DspDomain::Stereo:
        return "Stereo Imaging";
    case DspDomain::LoudnessOutput:
        return "Loudness / Output";
    default:
        return "Unclassified";
    }
}

/// Single shared keyword classifier for "which DSP domain does this
/// recommendation belong to" - used by IssueHeatmapPanel (cross-domain
/// timeline lanes); the Mix/Master pipeline view that shared it went in
/// CUT-2. Previously
/// each hand-maintained its own, slightly different keyword table (different
/// keyword lists AND different match order), so the same recommendation
/// could land in "Spectral" on one screen and be silently dropped ("None")
/// on the other. One table, one order, checked here.
inline DspDomain classifyDspDomain(const Recommendation& rec)
{
    juce::String haystack =
        juce::String(rec.what) + " " + juce::String(rec.suggested_action) + " " + juce::String(rec.stem_or_bus);
    for (const auto& [k, v] : rec.suggested_parameters)
        haystack += " " + juce::String(k);
    haystack = haystack.toLowerCase();

    auto any = [&](std::initializer_list<const char*> keys) {
        for (auto* k : keys)
            if (haystack.contains(k)) return true;
        return false;
    };

    // Order matters: first match wins, so more specific/higher-priority
    // domains are checked first (the original pipeline view's order).
    if (any({"clip", "artifact", "true peak", "clipping"})) return DspDomain::ClipInput;
    if (any({"spectral", "frequency", "hz", "tonal", "sibilance", "eq", "brightness", "warmth", "balance", "resonance",
             "harsh", "muddy", "bright", "dull"}))
        return DspDomain::Spectral;
    if (any({"dynamic", "compress", "crest", "range", "transient", "density", "punch", "gain", "limiter", "peak"}))
        return DspDomain::Dynamics;
    if (any({"stereo", "width", "correlation", "mono", "phase", "imaging", "side", "pan", "goniometer"}))
        return DspDomain::Stereo;
    if (any({"loud", "lufs", "lra", "plr", "limit", "ceil", "gain staging", "output"}))
        return DspDomain::LoudnessOutput;
    return DspDomain::None;
}

} // namespace khensu
