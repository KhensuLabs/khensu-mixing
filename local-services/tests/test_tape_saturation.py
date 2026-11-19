"""Tests for the TapeSaturation effect (oversampled tube/tape waveshaper)."""
from __future__ import annotations

import numpy as np
import pytest

from khensu_services.copilot.dsp import _apply_tape_saturation


@pytest.fixture
def tone():
    sr = 44100
    n = sr // 2
    t = np.arange(n) / sr
    y = (0.5 * np.sin(2 * np.pi * 220.0 * t)).astype(np.float64)
    return y[np.newaxis, :], sr


def test_tape_saturation_shape_and_finiteness(tone):
    audio, sr = tone
    out = _apply_tape_saturation(audio, sr, drive_db=6.0, warmth=0.5, mix=1.0)
    assert out.shape == audio.shape
    assert np.all(np.isfinite(out))


def test_tape_saturation_zero_mix_is_transparent(tone):
    audio, sr = tone
    out = _apply_tape_saturation(audio, sr, drive_db=12.0, warmth=0.8, mix=0.0)
    np.testing.assert_allclose(out, audio, atol=1e-9)


def test_tape_saturation_adds_harmonics(tone):
    audio, sr = tone
    out = _apply_tape_saturation(audio, sr, drive_db=18.0, warmth=0.5, mix=1.0)
    # A driven waveshaper on a pure sine must add energy at the 2nd/3rd
    # harmonic bins that isn't present in the (linear) dry signal.
    n = audio.shape[1]
    freqs = np.fft.rfftfreq(n, d=1.0 / sr)
    dry_spec = np.abs(np.fft.rfft(audio[0] * np.hanning(n)))
    wet_spec = np.abs(np.fft.rfft(out[0] * np.hanning(n)))

    def bin_near(hz):
        return int(np.argmin(np.abs(freqs - hz)))

    h2 = bin_near(440.0)
    h3 = bin_near(660.0)
    assert wet_spec[h2] > dry_spec[h2] * 5.0
    assert wet_spec[h3] > dry_spec[h3] * 5.0


def test_tape_saturation_warmth_changes_output(tone):
    audio, sr = tone
    tube = _apply_tape_saturation(audio, sr, drive_db=12.0, warmth=0.0, mix=1.0)
    tape = _apply_tape_saturation(audio, sr, drive_db=12.0, warmth=1.0, mix=1.0)
    assert not np.allclose(tube, tape, atol=1e-4)


def test_tape_saturation_does_not_blow_up_headroom(tone):
    audio, sr = tone
    out = _apply_tape_saturation(audio, sr, drive_db=24.0, warmth=0.5, mix=1.0)
    # Saturation should compress peaks toward +/-1, not amplify them wildly.
    assert np.max(np.abs(out)) < 2.0
