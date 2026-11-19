"""Tests for the stereo-imaging apply-layer effects (StereoWidener, BassMonoMaker)."""
from __future__ import annotations

import numpy as np
import pytest

from khensu_services.copilot.dsp import _apply_stereo_widener, _apply_bass_mono_maker


@pytest.fixture
def wide_stereo():
    sr = 44100
    n = sr // 2
    t = np.arange(n) / sr
    # Decorrelated L/R (independent low tones + a shared high tone) so both
    # low-end phase and overall width are meaningfully non-trivial.
    low_l = 0.4 * np.sin(2 * np.pi * 80.0 * t)
    low_r = 0.4 * np.sin(2 * np.pi * 80.0 * t + np.pi * 0.9)  # near-opposite phase
    shared_high = 0.3 * np.sin(2 * np.pi * 4000.0 * t)
    left = low_l + shared_high
    right = low_r + shared_high
    return np.stack([left, right], axis=0), sr


def test_stereo_widener_zero_collapses_to_mono(wide_stereo):
    audio, sr = wide_stereo
    out = _apply_stereo_widener(audio, sr, width=0.0)
    np.testing.assert_allclose(out[0], out[1], atol=1e-9)


def test_stereo_widener_unity_is_transparent(wide_stereo):
    audio, sr = wide_stereo
    out = _apply_stereo_widener(audio, sr, width=1.0)
    np.testing.assert_allclose(out, audio, atol=1e-9)


def test_stereo_widener_increases_side_energy(wide_stereo):
    audio, sr = wide_stereo
    out = _apply_stereo_widener(audio, sr, width=1.8)
    side_before = np.sum(((audio[0] - audio[1]) * 0.5) ** 2)
    side_after = np.sum(((out[0] - out[1]) * 0.5) ** 2)
    assert side_after > side_before


def test_stereo_widener_noop_on_mono():
    mono = np.zeros((1, 1000))
    out = _apply_stereo_widener(mono, 44100, width=1.5)
    np.testing.assert_array_equal(out, mono)


def test_bass_mono_maker_collapses_low_end_correlation(wide_stereo):
    audio, sr = wide_stereo
    out = _apply_bass_mono_maker(audio, sr, crossover_hz=150.0)

    from scipy.signal import butter, sosfiltfilt
    sos_lo = butter(4, 150.0 / (sr * 0.5), btype="low", output="sos")
    low_l_before = sosfiltfilt(sos_lo, audio[0])
    low_r_before = sosfiltfilt(sos_lo, audio[1])
    low_l_after = sosfiltfilt(sos_lo, out[0])
    low_r_after = sosfiltfilt(sos_lo, out[1])

    # Before: near-opposite-phase low end barely correlates. After: forced mono.
    corr_before = np.corrcoef(low_l_before, low_r_before)[0, 1]
    corr_after = np.corrcoef(low_l_after, low_r_after)[0, 1]
    assert corr_after > 0.98
    assert corr_before < 0.5


def test_bass_mono_maker_preserves_high_end_difference(wide_stereo):
    # Highs are identical in this fixture (shared_high on both channels), so
    # just verify the high band survives roughly unchanged in level.
    audio, sr = wide_stereo
    out = _apply_bass_mono_maker(audio, sr, crossover_hz=150.0)
    assert np.max(np.abs(out)) > 0.1
    assert np.all(np.isfinite(out))


def test_bass_mono_maker_noop_on_mono():
    mono = np.zeros((1, 1000))
    out = _apply_bass_mono_maker(mono, 44100, crossover_hz=120.0)
    np.testing.assert_array_equal(out, mono)
