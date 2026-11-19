// desktop-app/Source/ui/shared/FeatureMaturity.h
// VER-8: which features are experimental - built and tested on synthetic
// input, but not yet checked on real audio, in a real DAW or against the tool
// they claim parity with. An experimental Dashboard card carries a badge and
// stays out of the Essentials view (UX-1).
//
// docs/experimental-features.md holds the whole table (these cards plus the
// app-wide items) with each outstanding check and who can do it;
// scripts/check_doc_drift.py fails when an id here is missing from it.
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
    {"emotional_arc", NEEDS_TRANS("The emotional arc has not been compared with listeners' judgements.")},
    {"heaviness", NEEDS_TRANS("The heaviness model has not been checked against rated tracks.")},
    {"recreate_sound", NEEDS_TRANS("Sound recreation has not been checked against the source on real stems.")},
    {"ai_artifacts", NEEDS_TRANS("The AI-detection fusion has not had its false-alarm rate measured on real releases.")},
    {"spatial_audio", NEEDS_TRANS("Immersive checks have not been compared with a Dolby renderer.")},
    {"structural", NEEDS_TRANS("Sections are fixed 8-bar blocks, not detected verses and choruses.")},
    {"section_readiness", NEEDS_TRANS("Sections are fixed 8-bar blocks, not detected verses and choruses.")},
}};

/// App-wide experimental features that are not Dashboard cards; each surface
/// shows an ExperimentalBadge (ExperimentalBadge::forFeature) beside its title.
inline constexpr std::array<ExperimentalFeature, 5> kExperimentalFeatures{{
    {"listening_room", NEEDS_TRANS("The listening room has not been tested from a second device.")},
    {"similar_references", NEEDS_TRANS("CLAP similarity has not been checked on a real reference library.")},
    {"local_llm", NEEDS_TRANS("Local models have not been checked with a real GGUF model or local server.")},
    {"voice_commands", NEEDS_TRANS("Whisper has not been checked on real spoken instructions.")},
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
