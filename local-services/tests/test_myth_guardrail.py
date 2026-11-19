from khensu_services.recommendations.myth_guardrail import apply

MYTH_CODES = ["lra_too_compressed", "lufs_too_loud", "stereo_too_wide", "over_compressed"]
NON_MYTH_CODES = ["clipping_detected", "true_peak_over", "centroid_too_bright",
                  "arc_quality_low", "heaviness_distortion_mismatch"]


def test_myth_codes_set_is_myth_adjacent():
    for code in MYTH_CODES:
        result = apply({}, code)
        assert result["is_myth_adjacent"] is True, f"Expected myth for {code}"
        assert result["myth_clarification"], f"Empty clarification for {code}"


def test_non_myth_codes_clear_is_myth_adjacent():
    for code in NON_MYTH_CODES:
        result = apply({}, code)
        assert result["is_myth_adjacent"] is False, f"Unexpected myth for {code}"


def test_unknown_code_returns_false():
    result = apply({}, "completely_unknown_code")
    assert result["is_myth_adjacent"] is False
