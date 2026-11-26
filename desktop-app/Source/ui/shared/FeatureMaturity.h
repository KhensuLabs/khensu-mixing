// desktop-app/Source/ui/shared/FeatureMaturity.h
// VER-8: which features are experimental - built and tested on synthetic
// input, but not yet checked on real audio, in a real DAW or against the tool
// they claim parity with. An experimental Dashboard card carries a badge and
// stays out of the Essentials view (UX-1).
//
// docs/manual.md section 9 holds the whole table (these cards plus the
// app-wide items) with each outstanding check; scripts/check_doc_drift.py
// fails when an id here is missing from it. Keep the two texts in step
// (CLN-2).
#pragma once

#include <array>
#include <string_view>

#include <JuceHeader.h>

namespace khensu::maturity {

struct ExperimentalFeature {
    std::string_view id;               ///< Dashboard panel id
    const char* outstandingCheck;      ///< what would make it proven (NEEDS_TRANS)
};

inline constexpr std::array<ExperimentalFeature, 9> kExperimentalPanels{{
    {"hooks", NEEDS_TRANS("Hook detection has not been compared with listeners' judgements.")},
    {"memorability", NEEDS_TRANS("The memorability score has not been compared with listeners' judgements.")},
    {"emotional_arc", NEEDS_TRANS("The emotional arc has not been compared with listener ratings over time.")},
    {"heaviness", NEEDS_TRANS("The heaviness model has not been checked against rated tracks.")},
    {"recreate_sound", NEEDS_TRANS("Sound recreation has not been checked against the source on real stems.")},
    {"ai_artifacts", NEEDS_TRANS("Partly checked: the learned probe has not been tested on generators unlike the seven it knows.")},
    {"spatial_audio", NEEDS_TRANS("Immersive checks have not been compared with a Dolby renderer.")},
    {"structural", NEEDS_TRANS("Detected section boundaries have not been compared with hand-marked songs.")},
    {"section_readiness", NEEDS_TRANS("Detected section boundaries have not been compared with hand-marked songs.")},
}};

/// App-wide experimental features that are not Dashboard cards; each surface
/// shows an ExperimentalBadge (ExperimentalBadge::forFeature) beside its title.
inline constexpr std::array<ExperimentalFeature, 5> kExperimentalFeatures{{
    {"listening_room", NEEDS_TRANS("The listening room has not been tested from a second device.")},
    {"similar_references", NEEDS_TRANS("Partly checked on released tracks; not yet on a user's own library.")},
    {"local_llm", NEEDS_TRANS("Partly checked with Ollama; the in-process GGUF path has not been checked.")},
    {"voice_commands", NEEDS_TRANS("Partly checked with synthetic voices; not yet with real voices in a control room.")},
    {"reference_profiles", NEEDS_TRANS("Shared reference profiles have not been tested with a hosted index.")},
}};

/// A Dashboard card or an app-wide feature by id; nullptr when proven.
[[nodiscard]] constexpr const ExperimentalFeature* findExperimentalFeature(std::string_view id) noexcept
{
    for (const auto& f : kExperimentalPanels)
        if (f.id == id) return &f;
    for (const auto& f : kExperimentalFeatures)
        if (f.id == id) return &f;
    return nullptr;
}

[[nodiscard]] constexpr const ExperimentalFeature* findExperimental(std::string_view id) noexcept
{
    for (const auto& f : kExperimentalPanels)
        if (f.id == id) return &f;
    return nullptr;
}

[[nodiscard]] constexpr bool isExperimental(std::string_view id) noexcept { return findExperimental(id) != nullptr; }

} // namespace khensu::maturity
