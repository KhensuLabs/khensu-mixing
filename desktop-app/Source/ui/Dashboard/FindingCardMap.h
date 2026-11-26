// desktop-app/Source/ui/Dashboard/FindingCardMap.h
// ENH-7: the Analysis screen card that shows a finding - where "Start here",
// a Top findings chip or a spectrum marker takes the user (and what
// IShell::revealCardForFinding reveals from another screen). One row per
// finding code the engine owns (IssueCodes.h, ENG-7); the test checks that
// every code has a card. The card is the one whose finding row (Explain /
// Hear it / Fix, CardFindingRow.h) shows that code, so the reveal lands on
// the finding's own Fix. "loudness", "dynamics", "spectral" and "stereo" are
// the always-visible core measurement panels; every other id is a Zone 3
// card (a member of a CardGroup selects its tab).
// THREAD_CONTEXT(any) - pure.
#pragma once
#include <khensu/recommendations/IssueCodes.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
#include "DashboardCategory.h"
#include "EssentialsSelection.h"

namespace khensu::finding_card {

/// The core measurement panels above Zone 3 (always shown with a session).
inline bool isCorePanel(std::string_view id) noexcept
{
    return id == "loudness" || id == "dynamics" || id == "spectral" || id == "stereo";
}

/// The card for a finding code, or a recommendation id ("<code>_<file id>").
/// Empty when no card on the Analysis screen shows it.
[[nodiscard]] inline std::string forFinding(std::string_view idOrCode)
{
    const auto code = recommendations::issueCodePrefix(idOrCode);
    struct Entry {
        std::string_view code;
        std::string_view card;
    };
    static constexpr Entry kMap[] = {
        // Loudness and dynamics
        {"lufs_too_loud", "loudness"},
        {"lufs_too_quiet", "loudness"},
        {"lra_too_compressed", "loudness"},
        {"lra_too_dynamic", "loudness"},
        {"true_peak_over", "loudness"},
        {"silent_track", "loudness"},
        {"over_compressed", "dynamics"},
        {"under_compressed", "dynamics"},
        {"crest_above_genre_target", "dynamics"},
        {"crest_below_genre_target", "dynamics"},
        {"flat_loudness_arc", "loudness_arc"},
        {"section_arc_too_flat", "loudness_arc"},
        {"section_arc_too_wide", "loudness_arc"},
        {"section_peak_too_quiet", "loudness_arc"},
        // Source and artifacts
        {"clipping_detected", "artifact"},
        {"noise_floor_high", "artifact"},
        {"dc_offset", "artifact"},
        {"ai_artifacts", "ai_artifacts"},
        // Tone
        {"centroid_too_bright", "tonal_balance"},
        {"centroid_too_dark", "tonal_balance"},
        {"low_end_heavy", "tonal_balance"},
        {"low_end_light", "tonal_balance"},
        {"tilt_too_bright", "tonal_balance"},
        {"tilt_too_dark", "tonal_balance"},
        {"harshness_detected", "eq_curve"},
        {"resonance_hotspot", "eq_curve"},
        {"ms_brightness_imbalance", "mid_side_spec"},
        // Stereo
        {"stereo_too_wide", "stereo"},
        {"stereo_too_narrow", "stereo"},
        {"phase_issue", "phase"},
        {"mono_compat_warning", "mono_compat"},
        {"sub_bass_too_wide", "sub_bass_phase"},
        {"translation_risk_high", "translation"},
        // Stems
        {"masking_collision", "taxonomy_masking"},
        {"kick_bass_collision", "taxonomy_masking"},
        {"midrange_vocal_masking", "taxonomy_masking"},
        // Song
        {"hook_strength_low", "hooks"},
        {"hook_density_low", "hooks"},
        {"no_hooks_detected", "hooks"},
        {"earworm_likelihood_low", "memorability"},
        {"memorability_low", "memorability"},
        {"arc_quality_low", "emotional_arc"},
        {"structural_flow_poor", "structural"},
        {"heaviness_extremity_mismatch", "heaviness"},
        {"heaviness_weight_low", "heaviness"},
        {"heaviness_distortion_mismatch", "heaviness"},
    };
    for (const auto& e : kMap)
        if (e.code == code) return std::string(e.card);
    // A code outside the engine's list (a service finding): the Essentials
    // card for its prefix, if any.
    return std::string(essentials::panelForFinding(idOrCode));
}

/// The view a reveal shows the card (a layout unit: a group id or a card id)
/// in: the current one when it already holds the card, else "All" when All
/// shows it, else the card's own category (a song insight, an advanced card
/// while advanced cards are hidden from All). `essentialIds` are the units
/// Essentials shows now.
[[nodiscard]] inline juce::String viewForReveal(const juce::String& currentView, const std::string& unit,
                                                const std::vector<std::string>& essentialIds, bool showAdvanced)
{
    const juce::String essentials = dashboard_category::kTabs[0], all = dashboard_category::kTabs[1];
    const bool inView = currentView == essentials
                            ? std::find(essentialIds.begin(), essentialIds.end(), unit) != essentialIds.end()
                            : dashboard_category::shownInTab(currentView, unit, showAdvanced);
    if (inView) return currentView;
    return dashboard_category::shownInTab(all, unit, showAdvanced) ? all : dashboard_category::forPanel(unit);
}

} // namespace khensu::finding_card
