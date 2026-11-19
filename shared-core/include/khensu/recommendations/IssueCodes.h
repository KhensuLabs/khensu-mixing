// shared-core/include/khensu/recommendations/IssueCodes.h
// ENG-7: the one list of finding codes. The C++ recommenders own the codes
// and their severity; the Python service only words them
// (recommendations/text_factory templates, keyed by code). The Python copy,
// local-services/khensu_services/recommendations/issue_codes.py, is generated
// from this table by scripts/gen_issue_codes.py (check_doc_drift.py fails when
// it is stale), and tests/test_cpp_issue_code_sync.py checks that every code a
// recommender emits is listed here and every listed code has a template.
#pragma once
#include <string_view>

namespace khensu::recommendations {

enum class IssueOrigin {
    Engine,  ///< emitted by a C++ recommender
    Service, ///< emitted by a service analyzer (multitrack advice), worded the same way
    Legacy,  ///< no longer emitted; kept so sessions saved while it was still read back
};

struct IssueCodeInfo {
    std::string_view code;
    bool taste;         ///< a style decision, not a technical error (is_taste_decision)
    IssueOrigin origin; ///< who emits it
};

// clang-format off
inline constexpr IssueCodeInfo kIssueCodes[] = {
    // Loudness and dynamics
    {"lufs_too_loud",                false, IssueOrigin::Engine},
    {"lufs_too_quiet",               false, IssueOrigin::Engine},
    {"lra_too_compressed",           false, IssueOrigin::Engine},
    {"lra_too_dynamic",              false, IssueOrigin::Engine},
    {"true_peak_over",               false, IssueOrigin::Engine},
    {"over_compressed",              false, IssueOrigin::Engine},
    {"under_compressed",             false, IssueOrigin::Engine},
    {"crest_above_genre_target",     true,  IssueOrigin::Engine},
    {"crest_below_genre_target",     true,  IssueOrigin::Legacy},  // replaced by over_compressed
    {"flat_loudness_arc",            true,  IssueOrigin::Engine},
    // Signal integrity
    {"clipping_detected",            false, IssueOrigin::Engine},
    {"noise_floor_high",             false, IssueOrigin::Engine},
    {"dc_offset",                    false, IssueOrigin::Engine},
    {"ai_artifacts",                 false, IssueOrigin::Legacy},  // FIX-13: AI detection is the service's
    {"silent_track",                 false, IssueOrigin::Service},
    // Tone
    {"centroid_too_bright",          false, IssueOrigin::Engine},
    {"centroid_too_dark",            false, IssueOrigin::Engine},
    {"low_end_heavy",                false, IssueOrigin::Engine},
    {"low_end_light",                false, IssueOrigin::Engine},
    {"tilt_too_bright",              true,  IssueOrigin::Engine},
    {"tilt_too_dark",                true,  IssueOrigin::Engine},
    {"harshness_detected",           false, IssueOrigin::Engine},
    {"resonance_hotspot",            false, IssueOrigin::Engine},
    {"ms_brightness_imbalance",      false, IssueOrigin::Engine},
    // Stereo and translation
    {"stereo_too_wide",              false, IssueOrigin::Engine},
    {"stereo_too_narrow",            false, IssueOrigin::Engine},
    {"phase_issue",                  false, IssueOrigin::Engine},
    {"mono_compat_warning",          false, IssueOrigin::Engine},
    {"sub_bass_too_wide",            false, IssueOrigin::Engine},
    {"translation_risk_high",        false, IssueOrigin::Engine},
    // Masking
    {"masking_collision",            true,  IssueOrigin::Engine},
    {"kick_bass_collision",          false, IssueOrigin::Engine},
    {"midrange_vocal_masking",       false, IssueOrigin::Engine},
    // Song level
    {"hook_strength_low",            true,  IssueOrigin::Engine},
    {"hook_density_low",             true,  IssueOrigin::Engine},
    {"no_hooks_detected",            true,  IssueOrigin::Engine},
    {"earworm_likelihood_low",       true,  IssueOrigin::Engine},
    {"memorability_low",             true,  IssueOrigin::Engine},
    {"arc_quality_low",              false, IssueOrigin::Legacy},  // the arc finding is section_arc_*
    {"structural_flow_poor",         false, IssueOrigin::Engine},
    {"heaviness_extremity_mismatch", true,  IssueOrigin::Engine},
    {"heaviness_weight_low",         true,  IssueOrigin::Engine},
    {"heaviness_distortion_mismatch",true,  IssueOrigin::Engine},
    {"section_arc_too_flat",         false, IssueOrigin::Engine},
    {"section_arc_too_wide",         false, IssueOrigin::Engine},
    {"section_peak_too_quiet",       false, IssueOrigin::Engine},
};
// clang-format on

/// The code a recommendation id starts with (`<code>_<file_id>`), longest
/// match first so "lufs_too_loud" never matches a shorter prefix; the id
/// itself when no code matches.
inline std::string_view issueCodePrefix(std::string_view id) noexcept
{
    std::string_view best;
    for (const auto& c : kIssueCodes)
        if (id.size() >= c.code.size() && id.substr(0, c.code.size()) == c.code && c.code.size() > best.size())
            best = c.code;
    return best.empty() ? id : best;
}

inline const IssueCodeInfo* findIssueCode(std::string_view code) noexcept
{
    for (const auto& c : kIssueCodes)
        if (c.code == code) return &c;
    return nullptr;
}

} // namespace khensu::recommendations
