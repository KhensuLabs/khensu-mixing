from __future__ import annotations
import numpy as np
import pytest


@pytest.fixture
def sine_tone_1k():
    """1 kHz sine at SR=22050, 3 seconds, stereo."""
    sr = 22050
    t = np.linspace(0, 3.0, 3 * sr)
    mono = 0.5 * np.sin(2 * np.pi * 1000 * t)
    return np.stack([mono, mono]), sr  # stereo, same channel


@pytest.fixture
def harsh_tone_3k():
    """Harsh 3.5 kHz sine with higher amplitude — should score as harsh."""
    sr = 22050
    t = np.linspace(0, 3.0, 3 * sr)
    mono = 0.8 * np.sin(2 * np.pi * 3500 * t)
    return np.stack([mono, mono]), sr


def test_analyze_harshness_mild_on_1k(sine_tone_1k):
    from khensu_services.analysis.extended_analyzer import analyze_harshness
    y, sr = sine_tone_1k
    result = analyze_harshness(y, sr)
    assert 0.0 <= result["score"] <= 1.0
    assert result["score"] < 0.5, "1 kHz tone should not be considered harsh"


def test_analyze_harshness_high_on_3k(harsh_tone_3k):
    from khensu_services.analysis.extended_analyzer import analyze_harshness
    y, sr = harsh_tone_3k
    result = analyze_harshness(y, sr)
    assert result["score"] > 0.3, "3.5 kHz dominant tone should score as harsh"


def test_analyze_mono_compat_returns_all_bands(sine_tone_1k):
    from khensu_services.analysis.extended_analyzer import analyze_mono_compat
    y, sr = sine_tone_1k
    result = analyze_mono_compat(y, sr)
    for band in ("sub", "bass", "midrange", "upper_mid", "air"):
        assert band in result["per_band_correlation"], f"Missing band: {band}"
        corr = result["per_band_correlation"][band]
        assert -1.0 <= corr <= 1.0, f"Correlation out of range for {band}"
    assert 0.0 <= result["low_end_stereo_pct"] <= 100.0


def test_analyze_translation_risk_returns_all_environments(sine_tone_1k):
    from khensu_services.analysis.extended_analyzer import (
        analyze_mono_compat, analyze_translation_risk
    )
    y, sr = sine_tone_1k
    mono_result = analyze_mono_compat(y, sr)
    spectral_stub = {"centroid_hz": 3000.0, "balance_low": 0.35}
    dynamics_stub = {"crest_factor_db": 10.0, "lra": 7.0}
    result = analyze_translation_risk(mono_result, spectral_stub, dynamics_stub)
    per_env = result["per_environment"]
    for env in ("studio", "earbuds", "phone", "car", "club"):
        assert env in per_env, f"Missing environment: {env}"
        assert 0.0 <= per_env[env] <= 1.0, f"Risk out of range for {env}"
    assert isinstance(result["score"], float)
    assert 0.0 <= result["score"] <= 1.0, "Aggregate score out of range"


def test_analyze_resonances_returns_list(sine_tone_1k):
    from khensu_services.analysis.extended_analyzer import analyze_resonances
    y, sr = sine_tone_1k
    result = analyze_resonances(y, sr)
    assert isinstance(result["hotspots"], list)
    assert len(result["hotspots"]) <= 5, "Should return at most 5 hotspots"
    for peak in result["hotspots"]:
        assert "hz" in peak
        assert "db" in peak
        assert "prominence" in peak
        assert peak["prominence"] >= 0


def test_analyze_resonances_detects_1k_peak(sine_tone_1k):
    from khensu_services.analysis.extended_analyzer import analyze_resonances
    y, sr = sine_tone_1k
    result = analyze_resonances(y, sr)
    hotspots = result["hotspots"]
    # A 1 kHz sine should produce a strong resonance around 1 kHz
    assert len(hotspots) >= 1
    peak_hzs = [r["hz"] for r in hotspots]
    assert any(800 < hz < 1200 for hz in peak_hzs), \
        f"Expected resonance near 1 kHz, got: {peak_hzs}"


# ---------------------------------------------------------------------------
# FINDING-01 regression: mono compat must use separate L/R channel paths
# ---------------------------------------------------------------------------


def test_mono_compat_finding01_regression():
    """FINDING-01: L/R must be filtered independently, not from a mono downmix.

    Setup: L = 100 Hz only, R = 500 Hz only.
    Bass band (80–250 Hz) filters L to ~0.5 amplitude, R to near-zero.
    If the old bug is present (downmix before filter), the filtered signal is
    (L+R)/2 on both sides → correlation = 1.0. Correct behaviour: near-zero
    correlation because L has bass energy and R does not.
    """
    from khensu_services.analysis.extended_analyzer import analyze_mono_compat
    sr  = 22050
    t   = np.linspace(0, 3.0, 3 * sr)
    left  = 0.5 * np.sin(2 * np.pi * 100 * t)
    right = 0.5 * np.sin(2 * np.pi * 500 * t)
    y = np.stack([left, right])

    result = analyze_mono_compat(y, sr)
    bass_corr = result["per_band_correlation"]["bass"]

    assert bass_corr < 0.7, (
        f"Bass band correlation should be low for L=100Hz / R=500Hz stereo, got {bass_corr:.3f}. "
        "If this is near 1.0, FINDING-01 has re-regressed: _bandpass is computing a mono "
        "downmix before filtering instead of treating L and R as separate channels."
    )


def test_mono_compat_identical_channels_are_correlated(sine_tone_1k):
    """Identical L and R should produce high correlation in all bands."""
    from khensu_services.analysis.extended_analyzer import analyze_mono_compat
    y, sr = sine_tone_1k  # L == R (same 1 kHz sine)
    result = analyze_mono_compat(y, sr)
    midrange_corr = result["per_band_correlation"]["midrange"]
    assert midrange_corr > 0.9, (
        f"Identical L/R should have near-1.0 midrange correlation, got {midrange_corr:.3f}"
    )


# ---------------------------------------------------------------------------
# Sibilance analysis
# ---------------------------------------------------------------------------

def test_analyze_sibilance_returns_score_and_peak(sine_tone_1k):
    from khensu_services.analysis.extended_analyzer import analyze_sibilance
    y, sr = sine_tone_1k
    result = analyze_sibilance(y, sr)
    assert 0.0 <= result["score"] <= 1.0
    assert "peak_hz" in result


def test_analyze_sibilance_low_on_1k(sine_tone_1k):
    from khensu_services.analysis.extended_analyzer import analyze_sibilance
    y, sr = sine_tone_1k
    result = analyze_sibilance(y, sr)
    assert result["score"] < 0.5, "1 kHz tone has no sibilant energy, score should be low"


def test_analyze_sibilance_high_on_7k():
    from khensu_services.analysis.extended_analyzer import analyze_sibilance
    sr = 22050
    t  = np.linspace(0, 3.0, 3 * sr)
    mono = 0.8 * np.sin(2 * np.pi * 7000 * t)
    y = np.stack([mono, mono])
    result = analyze_sibilance(y, sr)
    assert result["score"] > 0.5, "7 kHz dominant signal should score as sibilant"


# ---------------------------------------------------------------------------
# Vocal clarity analysis
# ---------------------------------------------------------------------------

def test_analyze_vocal_clarity_returns_all_fields(sine_tone_1k):
    from khensu_services.analysis.extended_analyzer import analyze_vocal_clarity
    y, sr = sine_tone_1k
    result = analyze_vocal_clarity(y, sr)
    for field in ("presence_score", "clarity_score", "presence_ratio", "mud_ratio"):
        assert field in result, f"Missing field: {field}"
        assert 0.0 <= result[field] <= 1.0, f"{field} out of range: {result[field]}"


def test_analyze_vocal_clarity_high_on_presence_band():
    from khensu_services.analysis.extended_analyzer import analyze_vocal_clarity
    sr = 22050
    t  = np.linspace(0, 3.0, 3 * sr)
    # 2 kHz sits in the presence zone (1–5 kHz)
    mono = 0.8 * np.sin(2 * np.pi * 2000 * t)
    y = np.stack([mono, mono])
    result = analyze_vocal_clarity(y, sr)
    assert result["presence_score"] > 0.5, (
        f"2 kHz signal should score high on presence, got {result['presence_score']:.3f}"
    )
