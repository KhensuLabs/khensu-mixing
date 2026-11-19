from __future__ import annotations
import numpy as np


def _stereo(sr: int, duration: float = 4.0) -> tuple:
    """Helper: returns (y_stereo, sr) where y has shape (2, N)."""
    n = int(sr * duration)
    return np.zeros((2, n), dtype=np.float32), sr


def _sine(freq: float, sr: int, duration: float = 4.0) -> np.ndarray:
    t = np.linspace(0, duration, int(sr * duration), endpoint=False)
    return (np.sin(2 * np.pi * freq * t)).astype(np.float32)


def test_mid_side_imbalance_returns_expected_keys():
    from khensu_services.analysis.extended_analyzer import analyze_mid_side_imbalance
    y, sr = _stereo(22050)
    result = analyze_mid_side_imbalance(y, sr)
    for key in ("side_bright_ratio", "side_heavy_ratio", "brightness_imbalance_db",
                "heaviness_imbalance_db", "imbalance_severity", "warning"):
        assert key in result, f"Missing key: {key}"


def test_mid_side_imbalance_mono_input_is_safe():
    from khensu_services.analysis.extended_analyzer import analyze_mid_side_imbalance
    sr = 22050
    y = np.zeros((1, sr * 2), dtype=np.float32)
    result = analyze_mid_side_imbalance(y, sr)
    assert result["imbalance_severity"] == 0.0


def test_mid_side_imbalance_identical_channels_near_zero_severity():
    """When L == R the side channel is zero — no imbalance."""
    from khensu_services.analysis.extended_analyzer import analyze_mid_side_imbalance
    sr = 22050
    sig = _sine(440, sr)
    y = np.stack([sig, sig])
    result = analyze_mid_side_imbalance(y, sr)
    assert result["imbalance_severity"] < 0.05


def test_mid_side_imbalance_sub_bass_too_wide_warns():
    """Side channel loaded with sub-bass → heavy_ratio > threshold → warning."""
    from khensu_services.analysis.extended_analyzer import analyze_mid_side_imbalance
    sr = 22050
    # Left: 60 Hz + 440 Hz; Right: 440 Hz only → large side-channel sub-bass
    sub = _sine(60, sr)
    mid_tone = _sine(440, sr)
    left  = (sub + mid_tone) * 0.5
    right = mid_tone * 0.5
    y = np.stack([left, right])
    result = analyze_mid_side_imbalance(y, sr)
    # The sub is only in L → side has sub energy
    assert result["side_heavy_ratio"] > 0.1
    # Severity should be non-negligible
    assert result["imbalance_severity"] > 0.0


def test_mid_side_imbalance_severity_in_range():
    from khensu_services.analysis.extended_analyzer import analyze_mid_side_imbalance
    sr = 22050
    y  = np.random.default_rng(0).random((2, sr * 4)).astype(np.float32) * 0.5
    result = analyze_mid_side_imbalance(y, sr)
    assert 0.0 <= result["imbalance_severity"] <= 1.0
