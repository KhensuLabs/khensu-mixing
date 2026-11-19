MYTHS: dict[str, tuple[bool, str]] = {
    "lufs_too_loud": (
        True,
        "The idea that tracks must be as loud as possible to 'compete' is a streaming-era myth. "
        "Spotify, Apple Music, and YouTube all apply loudness normalization, so overloud masters "
        "are turned down automatically — and often sound harsh or fatiguing compared to dynamic mixes.",
    ),
    "lra_too_compressed": (
        True,
        "High LRA is not inherently better. Genre context matters enormously: trap and pop "
        "intentionally target LRA 4–9 LU. Treating 'low LRA = bad' as universal conflates "
        "loudness-war criticism with genre production norms.",
    ),
    "stereo_too_wide": (
        True,
        "Extreme stereo width can seem impressive on headphones but collapses in mono and causes "
        "phase issues on club systems. Wider is not always better — genre norms exist for a reason.",
    ),
    "over_compressed": (
        True,
        "While heavy compression can reduce transient detail, it is a deliberate aesthetic choice "
        "in many modern genres (trap, EDM, pop). Low crest factor is not automatically a defect "
        "— it depends entirely on the production intent.",
    ),
    "crest_below_genre_target": (
        True,
        "A low crest factor is not 'more professional'. Loudness normalization removes the "
        "level advantage of squashing a master, so below the genre's typical crest factor the "
        "only lasting effect is lost transient impact - but dense genres legitimately sit low, "
        "which is why this is measured against the genre target, not a universal number.",
    ),
    "tilt_too_bright": (
        True,
        "'Brighter sounds better' only holds for a few seconds of A/B: a louder or brighter "
        "version wins a quick comparison, then fatigues. Compare against references at matched "
        "loudness before judging tonal balance.",
    ),
    # Every other code is not myth-adjacent. Keep one entry per issue code the
    # C++ recommenders emit (tests/test_cpp_issue_code_sync.py enforces it).
    "lufs_too_quiet": (False, ""),
    "lra_too_dynamic": (False, ""),
    "true_peak_over": (False, ""),
    "centroid_too_bright": (False, ""),
    "centroid_too_dark": (False, ""),
    "low_end_heavy": (False, ""),
    "low_end_light": (False, ""),
    "under_compressed": (False, ""),
    "stereo_too_narrow": (False, ""),
    "phase_issue": (False, ""),
    "clipping_detected": (False, ""),
    "noise_floor_high": (False, ""),
    "dc_offset": (False, ""),
    "ai_artifacts": (False, ""),
    "no_hooks_detected": (False, ""),
    "hook_density_low": (False, ""),
    "hook_strength_low": (False, ""),
    "memorability_low": (False, ""),
    "earworm_likelihood_low": (False, ""),
    "arc_quality_low": (False, ""),
    "heaviness_extremity_mismatch": (False, ""),
    "heaviness_weight_low": (False, ""),
    "heaviness_distortion_mismatch": (False, ""),
    "structural_flow_poor": (False, ""),
    "mono_compat_warning": (False, ""),
    "translation_risk_high": (False, ""),
    "harshness_detected": (False, ""),
    "resonance_hotspot": (False, ""),
    "sub_bass_too_wide": (False, ""),
    "ms_brightness_imbalance": (False, ""),
    "flat_loudness_arc": (False, ""),
    "crest_above_genre_target": (False, ""),
    "tilt_too_dark": (False, ""),
    "masking_collision": (False, ""),
    "kick_bass_collision": (False, ""),
    "midrange_vocal_masking": (False, ""),
    "section_arc_too_flat": (False, ""),
    "section_arc_too_wide": (False, ""),
    "section_peak_too_quiet": (False, ""),
}


def apply(result: dict, issue_code: str) -> dict:
    is_myth, clarification = MYTHS.get(issue_code, (False, ""))
    result["is_myth_adjacent"] = is_myth
    result["myth_clarification"] = clarification
    return result
