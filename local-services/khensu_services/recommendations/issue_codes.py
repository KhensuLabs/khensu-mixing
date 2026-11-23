"""Finding codes, generated from shared-core/.../recommendations/IssueCodes.h (ENG-7).

Do not edit: change the C++ table and run ``python scripts/gen_issue_codes.py``.
The C++ recommenders own the codes and their severity; the service words them.
"""

# code -> (taste decision, origin: engine | service | legacy)
ISSUE_CODES: dict[str, tuple[bool, str]] = {
    "lufs_too_loud": (False, "engine"),
    "lufs_too_quiet": (False, "engine"),
    "lra_too_compressed": (False, "engine"),
    "lra_too_dynamic": (False, "engine"),
    "true_peak_over": (False, "engine"),
    "over_compressed": (False, "engine"),
    "under_compressed": (False, "engine"),
    "crest_above_genre_target": (True, "engine"),
    "crest_below_genre_target": (True, "legacy"),
    "flat_loudness_arc": (True, "legacy"),
    "clipping_detected": (False, "engine"),
    "noise_floor_high": (False, "engine"),
    "dc_offset": (False, "engine"),
    "ai_artifacts": (False, "legacy"),
    "silent_track": (False, "service"),
    "centroid_too_bright": (False, "engine"),
    "centroid_too_dark": (False, "engine"),
    "low_end_heavy": (False, "engine"),
    "low_end_light": (False, "engine"),
    "tilt_too_bright": (True, "engine"),
    "tilt_too_dark": (True, "engine"),
    "harshness_detected": (False, "engine"),
    "resonance_hotspot": (False, "engine"),
    "ms_brightness_imbalance": (False, "engine"),
    "stereo_too_wide": (False, "engine"),
    "stereo_too_narrow": (False, "engine"),
    "phase_issue": (False, "engine"),
    "mono_compat_warning": (False, "engine"),
    "sub_bass_too_wide": (False, "engine"),
    "translation_risk_high": (False, "engine"),
    "masking_collision": (True, "engine"),
    "kick_bass_collision": (False, "engine"),
    "midrange_vocal_masking": (False, "service"),
    "hook_strength_low": (True, "engine"),
    "hook_density_low": (True, "engine"),
    "no_hooks_detected": (True, "engine"),
    "earworm_likelihood_low": (True, "engine"),
    "memorability_low": (True, "engine"),
    "arc_quality_low": (False, "legacy"),
    "structural_flow_poor": (False, "engine"),
    "heaviness_extremity_mismatch": (True, "engine"),
    "heaviness_weight_low": (True, "engine"),
    "heaviness_distortion_mismatch": (True, "engine"),
    "section_arc_too_flat": (False, "engine"),
    "section_arc_too_wide": (False, "engine"),
    "section_peak_too_quiet": (False, "engine"),
}

TASTE_CODES: frozenset[str] = frozenset(c for c, (taste, _) in ISSUE_CODES.items() if taste)

# Engine constants the service repeats (FIX-9), from SpectralRecommender.h:
# the centroid window is the genre centroid times these.
CENTROID_CEILING_RATIO = 1.25
CENTROID_FLOOR_RATIO = 0.75
