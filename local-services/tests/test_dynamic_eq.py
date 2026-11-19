"""Tests for the DynamicEQ effect (threshold-triggered per-band cut)."""
from __future__ import annotations

import numpy as np
import pytest

from khensu_services.copilot.dsp import _apply_dynamic_eq, _run_serial_chain

SR = 44100


def _band_rms(y: np.ndarray, sr: int, lo: float, hi: float) -> float:
    """RMS of the [lo, hi] Hz band via rFFT masking (mono input)."""
    spec = np.fft.rfft(y)
    freqs = np.fft.rfftfreq(y.shape[0], 1.0 / sr)
    mask = (freqs >= lo) & (freqs <= hi)
    banded = np.fft.irfft(spec * mask, n=y.shape[0])
    return float(np.sqrt(np.mean(banded ** 2)))


@pytest.fixture
def resonant_tone():
    """Loud 3 kHz resonance over a quiet 200 Hz fundamental, stereo."""
    t = np.arange(SR * 2) / SR
    y = (0.15 * np.sin(2 * np.pi * 200 * t)
         + 0.7 * np.sin(2 * np.pi * 3000 * t))
    return np.stack([y, y]), SR


def test_dynamic_eq_cuts_target_band_leaves_rest(resonant_tone):
    stereo, sr = resonant_tone
    out = _apply_dynamic_eq(stereo, sr, frequency_hz=3000.0, q=2.0,
                            threshold_db=-30.0, ratio=8.0,
                            attack_ms=1.0, release_ms=50.0)
    assert out.shape == stereo.shape
    # Steady-state region past the attack.
    steady_in, steady_out = stereo[0, sr:], out[0, sr:]
    assert _band_rms(steady_out, sr, 2500, 3500) < 0.5 * _band_rms(steady_in, sr, 2500, 3500)
    # The 200 Hz fundamental is far outside the band and must be untouched.
    assert _band_rms(steady_out, sr, 150, 250) == pytest.approx(
        _band_rms(steady_in, sr, 150, 250), rel=0.05)


def test_dynamic_eq_transparent_below_threshold(resonant_tone):
    stereo, sr = resonant_tone
    out = _apply_dynamic_eq(stereo, sr, frequency_hz=3000.0, q=2.0,
                            threshold_db=0.0, ratio=8.0)
    # Nothing crosses a 0 dBFS threshold, so the band gain never moves.
    edge = SR // 20
    np.testing.assert_allclose(out[:, edge:-edge], stereo[:, edge:-edge], atol=0.02)


def test_dynamic_eq_respects_max_reduction_floor(resonant_tone):
    stereo, sr = resonant_tone
    out = _apply_dynamic_eq(stereo, sr, frequency_hz=3000.0, q=2.0,
                            threshold_db=-60.0, ratio=20.0,
                            attack_ms=1.0, release_ms=50.0,
                            max_reduction_db=3.0)
    steady_in, steady_out = stereo[0, sr:], out[0, sr:]
    ratio_db = 20.0 * np.log10(_band_rms(steady_out, sr, 2500, 3500)
                               / _band_rms(steady_in, sr, 2500, 3500))
    # Extreme settings, but the cut may never exceed the 3 dB cap
    # (small tolerance for filter/envelope edges).
    assert ratio_db >= -3.5


def test_dynamic_eq_mono_input_roundtrip(resonant_tone):
    stereo, sr = resonant_tone
    mono = stereo[0]
    out = _apply_dynamic_eq(mono, sr, frequency_hz=3000.0,
                            threshold_db=-30.0, ratio=4.0)
    assert out.ndim == 1
    assert out.shape == mono.shape
    assert np.isfinite(out).all()


def test_dynamic_eq_degenerate_band_is_noop():
    # A narrow band centred so close to Nyquist that its lower edge lands
    # above the clamped upper edge → effect must pass audio through rather
    # than raise.
    y = np.sin(2 * np.pi * 440 * np.arange(SR) / SR)
    out = _apply_dynamic_eq(y, 8000, frequency_hz=3990.0, q=18.0)
    np.testing.assert_array_equal(out, y)


def test_chain_dispatches_dynamic_eq(resonant_tone):
    stereo, sr = resonant_tone
    processed, applied, skipped, _hr, _sl = _run_serial_chain(stereo, sr, [
        {"effect": "DynamicEQ", "parameters": {
            "frequency_hz": 3000.0, "threshold_db": -30.0, "ratio": 8.0}},
    ])
    assert applied == ["DynamicEQ"]
    assert skipped == []
    assert processed.shape == stereo.shape
    assert np.isfinite(processed).all()
