"""Tests for the Doubler effect (ADT-style double-tracker — a true delayed,
drifting second take, not Chorus's short-cycle modulation)."""
from __future__ import annotations

import numpy as np
import pytest

from khensu_services.copilot.dsp import _apply_doubler, _run_serial_chain

SR = 44100


@pytest.fixture
def vocal_like():
    """A 220 Hz tone with an amplitude envelope, mono + dual-mono stereo."""
    t = np.arange(SR * 2) / SR
    env = 0.5 * (1.0 + np.sin(2 * np.pi * 0.7 * t))
    mono = (0.4 * env * np.sin(2 * np.pi * 220 * t)).astype(np.float64)
    return mono, np.stack([mono, mono])


def test_doubler_preserves_shape_and_stays_finite(vocal_like):
    mono, stereo = vocal_like
    out_m = _apply_doubler(mono, SR)
    out_s = _apply_doubler(stereo, SR)
    assert out_m.shape == mono.shape and out_m.ndim == 1
    assert out_s.shape == stereo.shape
    assert np.isfinite(out_m).all() and np.isfinite(out_s).all()


def test_doubler_mix_zero_is_identity(vocal_like):
    mono, _ = vocal_like
    out = _apply_doubler(mono, SR, mix=0.0)
    np.testing.assert_allclose(out, mono, atol=1e-9)


def test_doubler_adds_copy_at_requested_delay(vocal_like):
    mono, _ = vocal_like
    delay_ms = 30.0
    # drift 0 → the double is exactly the dry signal shifted by delay_ms and
    # scaled by mix (a periodic-tone correlation search can't discriminate
    # cycle-aliased lags, so assert the shift directly).
    out = _apply_doubler(mono, SR, delay_ms=delay_ms, drift_ms=0.0, mix=0.7)
    # Undo the equal-power level compensation (audit CPL-22) to recover x + mix*double.
    out = out * np.sqrt(1.0 + 0.7 ** 2)
    double = out - mono
    shift = round(SR * delay_ms / 1000.0)
    np.testing.assert_allclose(double[shift:], 0.7 * mono[:-shift], atol=1e-6)
    # Nothing arrives before the delayed take starts.
    np.testing.assert_allclose(double[:shift], 0.0, atol=1e-6)


def test_doubler_width_decorrelates_stereo(vocal_like):
    _, stereo = vocal_like
    # Dual-mono in, width=1 → the two doubles drift differently, so the
    # channels must stop being identical.
    wide = _apply_doubler(stereo, SR, drift_ms=4.0, width=1.0, mix=0.8)
    assert not np.allclose(wide[0], wide[1], atol=1e-4)
    # width=0 keeps the double identical on both channels (mono-safe).
    narrow = _apply_doubler(stereo, SR, drift_ms=4.0, width=0.0, mix=0.8)
    np.testing.assert_allclose(narrow[0], narrow[1], atol=1e-9)


def test_doubler_drift_modulates_the_delay(vocal_like):
    mono, _ = vocal_like
    # With drift the double is NOT a pure static delay of the dry signal:
    # subtracting the best static delay leaves substantial residual.
    out = _apply_doubler(mono, SR, delay_ms=25.0, drift_ms=6.0,
                         drift_rate_hz=1.5, mix=1.0)
    double = out - mono
    static = _apply_doubler(mono, SR, delay_ms=25.0, drift_ms=0.0, mix=1.0) - mono
    residual = np.sqrt(np.mean((double - static) ** 2))
    assert residual > 0.05 * np.sqrt(np.mean(static ** 2))


def test_doubler_degenerate_delay_is_noop():
    # Delay longer than the clip: nothing sensible to double — pass through.
    y = np.sin(2 * np.pi * 440 * np.arange(256) / SR)
    out = _apply_doubler(y, SR, delay_ms=60.0)
    np.testing.assert_array_equal(out, y)


def test_chain_dispatches_doubler(vocal_like):
    _, stereo = vocal_like
    processed, applied, skipped, _hr, _sl = _run_serial_chain(stereo, SR, [
        {"effect": "Doubler", "parameters": {
            # Out-of-range values must clamp, not crash.
            "delay_ms": 500.0, "drift_ms": 99.0, "width": 3.0, "mix": 2.0}},
    ])
    assert applied == ["Doubler"]
    assert skipped == []
    assert processed.shape == stereo.shape
    assert np.isfinite(processed).all()
