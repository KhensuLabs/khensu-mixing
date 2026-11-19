from khensu_services.recommendations.text_factory import enrich, _fallback

REQUIRED_KEYS = [
    "what", "why_helps", "suggested_action", "engineer_note",
    "severity_context", "confidence_reason", "most_likely_cause",
    "trade_off", "bpm_mood_genre_relevance", "comparison_to_similar_songs",
    "alternative_strategies",
]

ALL_26_CODES = [
    "lufs_too_loud", "lufs_too_quiet", "lra_too_compressed", "lra_too_dynamic",
    "true_peak_over", "centroid_too_bright", "centroid_too_dark", "low_end_heavy",
    "low_end_light", "over_compressed", "under_compressed", "stereo_too_narrow",
    "stereo_too_wide", "phase_issue", "clipping_detected", "noise_floor_high",
    "ai_artifacts", "no_hooks_detected", "hook_density_low", "hook_strength_low",
    "memorability_low", "earworm_likelihood_low", "arc_quality_low",
    "heaviness_extremity_mismatch", "heaviness_weight_low", "heaviness_distortion_mismatch",
]


def _finding(issue_code: str, **kwargs) -> dict:
    return {
        "id": f"{issue_code}_550e8400-e29b-41d4-a716-446655440000",
        "issue_code": issue_code,
        "domain": "loudness",
        "severity": kwargs.get("severity", 42.0),
        "confidence": kwargs.get("confidence", 0.80),
        "measured_value": kwargs.get("measured_value", -7.0),
        "norm_value": kwargs.get("norm_value", -9.0),
        "unit": kwargs.get("unit", "LUFS"),
        "is_target_relative": False,
        "stem_or_bus": "Master",
        "section": "",
        "suggested_parameters": {},
    }


def test_dc_offset_has_its_own_text_not_the_fallback():
    # MEAS-4: the C++ engine raises dc_offset; the service words it.
    result = enrich(_finding("dc_offset", measured_value=0.02, norm_value=0.002, unit=""), "Trap", 140.0, "aggressive")
    for key in REQUIRED_KEYS:
        assert result.get(key), key
    assert "+0.0200" in result["what"]


def test_all_26_codes_return_all_required_keys():
    for code in ALL_26_CODES:
        result = enrich(_finding(code, measured_value=0.5, norm_value=0.3),
                        "Trap", 140.0, "aggressive")
        for key in REQUIRED_KEYS:
            assert key in result, f"Missing key '{key}' for issue_code '{code}'"
        assert result["what"], f"Empty 'what' for '{code}'"
        assert result["why_helps"], f"Empty 'why_helps' for '{code}'"
        assert result["suggested_action"], f"Empty 'suggested_action' for '{code}'"
        assert result["engineer_note"], f"Empty 'engineer_note' for '{code}'"
        assert isinstance(result["alternative_strategies"], list), f"alternative_strategies must be list for '{code}'"


def test_lufs_too_loud_contains_measured_value_in_what():
    result = enrich(_finding("lufs_too_loud", measured_value=-6.5, norm_value=-9.0),
                    "Trap", 140.0, "aggressive")
    assert "-6.5" in result["what"] or "-6" in result["what"]


def test_fallback_returns_non_empty_strings():
    result = _fallback({"id": "unknown_test", "issue_code": "unknown_code"})
    for key in REQUIRED_KEYS:
        assert key in result
    assert result["what"]
    assert result["why_helps"]
    assert result["suggested_action"]
    assert result["engineer_note"]
    assert isinstance(result["alternative_strategies"], list)
    assert len(result["alternative_strategies"]) > 0


def test_enrich_uses_fallback_for_unknown_code():
    result = enrich(_finding("totally_unknown_code"), "Pop", 120.0, "")
    assert result["what"]
    assert "totally_unknown_code" in result["what"] or "unknown" in result["what"].lower()


def test_every_template_has_how_to_verify():
    from khensu_services.recommendations.text_factory import TEMPLATES
    for code, tmpl in TEMPLATES.items():
        assert "how_to_verify" in tmpl, f"Missing how_to_verify in {code!r}"
        result = tmpl["how_to_verify"]({"genre_name": "pop", "bpm": 120.0,
                                         "mood": "energetic", "severity": 50.0,
                                         "measured_value": -10.0, "norm_value": -14.0})
        assert isinstance(result, str) and len(result) > 10, \
            f"how_to_verify for {code!r} is too short"


def test_taste_codes_are_arrangement_codes():
    from khensu_services.recommendations.text_factory import TASTE_CODES
    assert "hook_strength_low" in TASTE_CODES
    assert "hook_density_low" in TASTE_CODES
    assert "no_hooks_detected" in TASTE_CODES
    assert "memorability_low" in TASTE_CODES
    assert "earworm_likelihood_low" in TASTE_CODES
    assert "heaviness_extremity_mismatch" in TASTE_CODES
    assert "heaviness_weight_low" in TASTE_CODES
    assert "heaviness_distortion_mismatch" in TASTE_CODES
    # arc_quality_low is NOT in TASTE_CODES (C++ doesn't emit this code)
    assert "arc_quality_low" not in TASTE_CODES
    # Technical codes must NOT be in TASTE_CODES
    assert "lufs_too_loud" not in TASTE_CODES
    assert "true_peak_over" not in TASTE_CODES


def test_templates_have_no_is_taste_decision_key():
    from khensu_services.recommendations.text_factory import TEMPLATES
    for code, tmpl in TEMPLATES.items():
        assert "is_taste_decision" not in tmpl, f"{code} still has is_taste_decision in TEMPLATES"


def test_active_recommendation_codes_have_templates():
    """Catches renames where C++ codes drift from Python TEMPLATES keys."""
    from khensu_services.recommendations.text_factory import TEMPLATES
    # Fixed codes emitted by C++ recommenders (verified against .h files)
    fixed_codes = {
        "lufs_too_loud", "lufs_too_quiet",
        "true_peak_over", "clipping_detected",
        "over_compressed", "under_compressed",
        "lra_too_compressed", "lra_too_dynamic",
        "centroid_too_bright", "centroid_too_dark",
        "low_end_heavy", "low_end_light",
        "noise_floor_high",
        "stereo_too_narrow", "stereo_too_wide", "phase_issue",
        "ai_artifacts",
        "hook_strength_low", "hook_density_low", "no_hooks_detected",
        "memorability_low", "earworm_likelihood_low",
        "heaviness_extremity_mismatch", "heaviness_weight_low", "heaviness_distortion_mismatch",
    }
    # Prefix keys for dynamic-ID recommenders (e.g. "harshness_detected_<file_id>")
    prefix_keys = {
        "harshness_detected", "mono_compat_warning",
        "translation_risk_high", "resonance_hotspot", "structural_flow_poor",
    }
    missing = (fixed_codes | prefix_keys) - set(TEMPLATES.keys())
    assert not missing, f"C++ codes missing from TEMPLATES: {sorted(missing)}"
