"""Tests for the TransientShaper effect (dual-envelope attack/sustain split)."""
from __future__ import annotations

import numpy as np
import pytest

from khensu_services.copilot.dsp import _apply_transient_shaper


@pytest.fixture
def hit_then_tail():
    """A short sharp transient (click) followed by a sustained tone."""
    sr = 44100
    n = sr  # 1 second
    y = np.zeros(n, dtype=np.float64)
    click_len = int(0.003 * sr)  # 3 ms sharp hit
    y[:click_len] = 0.9
    t = np.arange(n - click_len) / sr
    y[click_len:] = 0.2 * np.sin(2 * np.pi * 200.0 * t)
    return y[np.newaxis, :], sr, click_len


def test_transient_shaper_boosts_attack_relative_to_sustain(hit_then_tail):
    audio, sr, click_len = hit_then_tail
    boosted = _apply_transient_shaper(audio, sr, attack_gain_db=12.0, sustain_gain_db=0.0)

    attack_peak_before = np.max(np.abs(audio[0, :click_len]))
    attack_peak_after = np.max(np.abs(boosted[0, :click_len]))
    tail_rms_before = np.sqrt(np.mean(audio[0, sr // 2:] ** 2))
    tail_rms_after = np.sqrt(np.mean(boosted[0, sr // 2:] ** 2))

    assert attack_peak_after > attack_peak_before
    # Sustain (steady tail, well after the transient decays) should be
    # essentially unaffected by an attack-only boost.
    assert tail_rms_after == pytest.approx(tail_rms_before, rel=0.05)


def test_transient_shaper_cuts_sustain_relative_to_attack(hit_then_tail):
    audio, sr, click_len = hit_then_tail
    out = _apply_transient_shaper(audio, sr, attack_gain_db=0.0, sustain_gain_db=-12.0)

    attack_peak_before = np.max(np.abs(audio[0, :click_len]))
    attack_peak_after = np.max(np.abs(out[0, :click_len]))
    tail_rms_before = np.sqrt(np.mean(audio[0, sr // 2:] ** 2))
    tail_rms_after = np.sqrt(np.mean(out[0, sr // 2:] ** 2))

    assert tail_rms_after < tail_rms_before
    assert attack_peak_after == pytest.approx(attack_peak_before, rel=0.05)


def test_transient_shaper_unity_is_transparent(hit_then_tail):
    audio, sr, _ = hit_then_tail
    out = _apply_transient_shaper(audio, sr, attack_gain_db=0.0, sustain_gain_db=0.0)
    np.testing.assert_allclose(out, audio, atol=1e-6)


def test_transient_shaper_finite_output(hit_then_tail):
    audio, sr, _ = hit_then_tail
    out = _apply_transient_shaper(audio, sr, attack_gain_db=18.0, sustain_gain_db=-18.0)
    assert np.all(np.isfinite(out))
