"""Tests for the Exciter effect (Aphex-style aural enhancer — synthesized
high-frequency harmonics mixed under the dry signal, not an EQ boost)."""
from __future__ import annotations

import numpy as np
import pytest

from khensu_services.copilot.dsp import _apply_exciter, _run_serial_chain

SR = 44100


def _band_rms(y: np.ndarray, sr: int, lo: float, hi: float) -> float:
    spec = np.fft.rfft(y)
    freqs = np.fft.rfftfreq(y.shape[0], 1.0 / sr)
    mask = (freqs >= lo) & (freqs <= hi)
    banded = np.fft.irfft(spec * mask, n=y.shape[0])
    return float(np.sqrt(np.mean(banded ** 2)))


@pytest.fixture
def dull_tone():
    """A 500 Hz fundamental plus a 5 kHz component — nothing above 6 kHz."""
    t = np.arange(SR) / SR
    mono = (0.4 * np.sin(2 * np.pi * 500 * t)
            + 0.25 * np.sin(2 * np.pi * 5000 * t))
    return mono, np.stack([mono, mono])


def test_exciter_preserves_shape_and_stays_finite(dull_tone):
    mono, stereo = dull_tone
    out_m = _apply_exciter(mono, SR)
    out_s = _apply_exciter(stereo, SR)
    assert out_m.shape == mono.shape and out_m.ndim == 1
    assert out_s.shape == stereo.shape
    assert np.isfinite(out_m).all() and np.isfinite(out_s).all()


def test_exciter_mix_zero_is_identity(dull_tone):
    mono, _ = dull_tone
    out = _apply_exciter(mono, SR, mix=0.0)
    np.testing.assert_allclose(out, mono, atol=1e-9)


def test_exciter_synthesizes_new_high_harmonics(dull_tone):
    mono, _ = dull_tone
    # Excite everything above 4 kHz: the 5 kHz partial should grow harmonics
    # at 10 kHz / 15 kHz that simply do not exist in the input.
    out = _apply_exciter(mono, SR, frequency_hz=4000.0, drive=6.0, mix=0.8)
    edge = SR // 20
    before = _band_rms(mono[edge:-edge], SR, 8000, 18000)
    after = _band_rms(out[edge:-edge], SR, 8000, 18000)
    assert before < 1e-6            # the fixture really is dull up there
    assert after > 50.0 * max(before, 1e-9)
    assert after > 0.001


def test_exciter_leaves_lows_untouched(dull_tone):
    mono, _ = dull_tone
    out = _apply_exciter(mono, SR, frequency_hz=4000.0, drive=6.0, mix=0.8)
    edge = SR // 20
    # The 500 Hz fundamental sits far below the crossover — same level out.
    assert _band_rms(out[edge:-edge], SR, 400, 600) == pytest.approx(
        _band_rms(mono[edge:-edge], SR, 400, 600), rel=0.05)


def test_exciter_drive_scales_the_effect(dull_tone):
    mono, _ = dull_tone
    edge = SR // 20
    gentle = _apply_exciter(mono, SR, frequency_hz=4000.0, drive=1.0, mix=0.8)
    hard = _apply_exciter(mono, SR, frequency_hz=4000.0, drive=8.0, mix=0.8)
    assert (_band_rms(hard[edge:-edge], SR, 8000, 18000)
            > _band_rms(gentle[edge:-edge], SR, 8000, 18000))


def test_exciter_degenerate_crossover_is_noop():
    # Crossover clamped/near Nyquist at a low sample rate → pass through.
    y = np.sin(2 * np.pi * 440 * np.arange(SR // 4) / SR)
    out = _apply_exciter(y, 8000, frequency_hz=12000.0)
    np.testing.assert_array_equal(out, y)


def test_chain_dispatches_exciter(dull_tone):
    _, stereo = dull_tone
    processed, applied, skipped, _hr, _sl = _run_serial_chain(stereo, SR, [
        {"effect": "Exciter", "parameters": {
            # Out-of-range values must clamp, not crash.
            "frequency_hz": 99999.0, "drive": 100.0, "mix": 5.0}},
    ])
    assert applied == ["Exciter"]
    assert skipped == []
    assert processed.shape == stereo.shape
    assert np.isfinite(processed).all()
