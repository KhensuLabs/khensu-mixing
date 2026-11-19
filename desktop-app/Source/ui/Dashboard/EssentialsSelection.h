// desktop-app/Source/ui/Dashboard/EssentialsSelection.h
// UX-1: the Dashboard's default "Essentials" view - about eight Zone 3 cards
// instead of all 46. Mix Notes (the findings) always leads; then the card that
// shows each finding, worst finding first; then a fixed core set fills the
// rest. Experimental cards (VER-8) never appear here; "All" and the category
// tabs still show them.
#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <khensu/recommendations/Recommendation.h>

#include "../shared/FeatureMaturity.h"

namespace khensu::essentials {

inline constexpr std::size_t kEssentialsCount = 8;

/// The Zone 3 card that shows a finding, by its code (the id's prefix), or
/// empty when the finding's numbers live above Zone 3 (loudness, dynamics and
/// stereo have their own always-visible panels) or nowhere on the Dashboard.
[[nodiscard]] inline std::string_view panelForFinding(std::string_view findingId) noexcept
{
    struct Entry {
        std::string_view codePrefix;
        std::string_view panel;
    };
    // Longest prefixes first where one code starts another.
    static constexpr Entry kMap[] = {
        {"true_peak_over", "translation"},
        {"translation_risk_high", "translation"},
        {"lufs_too_", "streaming_norm"},
        {"lra_too_", "loudness_curve"},
        {"over_compressed", "loudness_curve"},
        {"under_compressed", "loudness_curve"},
        {"crest_above_genre_target", "loudness_curve"},
        {"centroid_too_", "tonal_balance"},
        {"tilt_too_", "tonal_balance"},
        {"low_end_", "tonal_balance"},
        {"harshness_detected", "eq_curve"},
        {"resonance_hotspot", "eq_curve"},
        {"phase_issue", "phase"},
        {"mono_compat_warning", "mono_compat"},
        {"sub_bass_too_wide", "sub_bass_phase"},
        {"ms_brightness_imbalance", "mid_side_spec"},
        {"stereo_too_", "phase"},
        {"clipping_detected", "artifact"},
        {"noise_floor_high", "artifact"},
        {"dc_offset", "artifact"},
        {"ai_artifacts", "ai_artifacts"},
        {"masking_collision", "taxonomy_masking"},
        {"kick_bass_collision", "taxonomy_masking"},
        {"midrange_vocal_masking", "taxonomy_masking"},
        {"flat_loudness_arc", "loudness_arc"},
        {"section_arc_", "loudness_arc"},
        {"section_peak_too_quiet", "loudness_arc"},
        {"structural_flow_poor", "structural"},
        {"hook_", "hooks"},
        {"no_hooks_detected", "hooks"},
        {"earworm_likelihood_low", "memorability"},
        {"memorability_low", "memorability"},
        {"heaviness_", "heaviness"},
    };
    for (const auto& e : kMap)
        if (findingId.substr(0, e.codePrefix.size()) == e.codePrefix) return e.panel;
    return {};
}

/// The cards that fill Essentials when the findings name fewer than eight.
inline constexpr std::string_view kCorePanels[] = {"reference",     "tonal_balance", "translation", "artifact",
                                                    "loudness_curve", "phase",        "eq_curve",    "song_info"};

/// The Essentials card ids for a result's findings, at most `count`, in
/// priority order (Mix Notes, then by finding severity, then the core set).
[[nodiscard]] inline std::vector<std::string> select(const std::vector<Recommendation>& findings,
                                                     std::size_t count = kEssentialsCount)
{
    std::vector<std::string> out;
    auto add = [&](std::string_view id) {
        if (out.size() >= count || id.empty() || maturity::isExperimental(id)) return;
        if (std::find(out.begin(), out.end(), id) == out.end()) out.emplace_back(id);
    };

    add("mix_notes");

    std::vector<const Recommendation*> bySeverity;
    bySeverity.reserve(findings.size());
    for (const auto& r : findings)
        bySeverity.push_back(&r);
    std::stable_sort(bySeverity.begin(), bySeverity.end(),
                     [](const Recommendation* a, const Recommendation* b) { return a->severity > b->severity; });
    for (const auto* r : bySeverity)
        add(panelForFinding(r->id));

    for (auto id : kCorePanels)
        add(id);
    return out;
}

} // namespace khensu::essentials
