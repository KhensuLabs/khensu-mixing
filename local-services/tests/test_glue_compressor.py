"""Tests for the GlueCompressor effect (soft-knee, detector choice, auto-release)."""
from __future__ import annotations

import numpy as np
import pytest

from khensu_services.copilot.dsp import _apply_glue_compressor


@pytest.fixture
def loud_tone():
    sr = 44100
    n = sr  # 1 second
    t = np.arange(n) / sr
    y = (0.9 * np.sin(2 * np.pi * 200.0 * t)).astype(np.float64)
    return y[np.newaxis, :], sr  # shape (1, n) like the internal stereo convention


def test_glue_compressor_reduces_gain_above_threshold(loud_tone):
    audio, sr = loud_tone
    out = _apply_glue_compressor(audio, sr, threshold_db=-20.0, ratio=4.0,
                                 attack_ms=1.0, release_ms=50.0, knee_db=0.0)
    # A loud tone well above threshold with a hard knee and 4:1 ratio must be
    # attenuated once the envelope has settled (steady-state region).
    steady = out[0, sr // 2:]
    orig_steady = audio[0, sr // 2:]
    assert np.max(np.abs(steady)) < np.max(np.abs(orig_steady))


def test_glue_compressor_below_threshold_is_transparent(loud_tone):
    audio, sr = loud_tone
    out = _apply_glue_compressor(audio, sr, threshold_db=0.0, ratio=4.0,
                                 attack_ms=1.0, release_ms=50.0, knee_db=0.0)
    # Signal never reaches 0 dBFS threshold -> ~unity gain throughout.
    np.testing.assert_allclose(out, audio, atol=1e-3)


def test_glue_compressor_soft_knee_engages_below_nominal_threshold():
    # The defining behaviour of a soft knee (Giannoulis et al.'s standard
    # quadratic formulation) is that it starts compressing *before* the
    # nominal threshold is reached, unlike a hard knee which is exactly
    # transparent below it. A tone sitting 3 dB *below* threshold, inside a
    # 12 dB knee, must therefore see gain reduction with the soft knee but
    # none with a hard knee.
    sr = 44100
    n = sr
    t = np.arange(n) / sr
    peak_db = -10.0
    amp = 10.0 ** (peak_db / 20.0)
    y = (amp * np.sin(2 * np.pi * 200.0 * t)).astype(np.float64)[np.newaxis, :]
    threshold_db = -7.0  # peak sits ~3 dB below threshold

    hard = _apply_glue_compressor(y, sr, threshold_db=threshold_db, ratio=4.0,
                                  attack_ms=1.0, release_ms=50.0, knee_db=0.0)
    soft = _apply_glue_compressor(y, sr, threshold_db=threshold_db, ratio=4.0,
                                  attack_ms=1.0, release_ms=50.0, knee_db=12.0)

    steady = slice(sr // 2, None)
    hard_peak = np.max(np.abs(hard[0, steady]))
    soft_peak = np.max(np.abs(soft[0, steady]))
    np.testing.assert_allclose(hard_peak, amp, atol=1e-3)   # hard knee: untouched below threshold
    assert soft_peak < hard_peak - 1e-4                      # soft knee: already engaged


def test_glue_compressor_rms_detector_runs_without_error(loud_tone):
    audio, sr = loud_tone
    out = _apply_glue_compressor(audio, sr, threshold_db=-20.0, ratio=3.0,
                                 attack_ms=5.0, release_ms=100.0, detector=1.0)
    assert out.shape == audio.shape
    assert np.all(np.isfinite(out))


def test_glue_compressor_auto_release_runs_without_error(loud_tone):
    audio, sr = loud_tone
    out = _apply_glue_compressor(audio, sr, threshold_db=-20.0, ratio=3.0,
                                 attack_ms=5.0, release_ms=100.0, auto_release=1.0)
    assert out.shape == audio.shape
    assert np.all(np.isfinite(out))


def test_glue_compressor_makeup_gain_boosts_output(loud_tone):
    audio, sr = loud_tone
    baseline = _apply_glue_compressor(audio, sr, threshold_db=-20.0, ratio=4.0, makeup_db=0.0)
    boosted = _apply_glue_compressor(audio, sr, threshold_db=-20.0, ratio=4.0, makeup_db=6.0)
    steady = slice(sr // 2, None)
    assert np.max(np.abs(boosted[0, steady])) > np.max(np.abs(baseline[0, steady]))
