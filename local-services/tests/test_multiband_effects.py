"""Tests for MultibandCompressor, MidSideProcessor, and Clipper effects."""
from __future__ import annotations

import numpy as np
import pytest

from khensu_services.copilot.dsp import (
    _apply_multiband_compressor, _apply_mid_side_processor, _apply_clipper, _run_serial_chain,
)

SR = 44100


@pytest.fixture
def three_band_tone():
    t = np.arange(SR * 2) / SR
    y = (0.8 * np.sin(2 * np.pi * 100 * t)      # low band
         + 0.3 * np.sin(2 * np.pi * 1000 * t)    # mid band
         + 0.2 * np.sin(2 * np.pi * 8000 * t))   # high band
    return np.stack([y, y]), SR


def test_multiband_compressor_reduces_targeted_band_only(three_band_tone):
    stereo, sr = three_band_tone
    # Heavy compression on the low band only; mid/high left near-transparent.
    out = _apply_multiband_compressor(
        stereo, sr,
        low_threshold_db=-40.0, low_ratio=10.0,
        mid_threshold_db=0.0, mid_ratio=1.0,
        high_threshold_db=0.0, high_ratio=1.0,
        attack_ms=1.0, release_ms=50.0,
    )
    assert out.shape == stereo.shape
    # Heavy low-band gain reduction should measurably lower overall peak
    # vs. the uncompressed original (steady-state region, past attack).
    steady_orig = stereo[0, sr:]
    steady_out = out[0, sr:]
    assert np.sqrt(np.mean(steady_out ** 2)) < np.sqrt(np.mean(steady_orig ** 2))


def test_multiband_compressor_transparent_when_all_bands_below_threshold(three_band_tone):
    stereo, sr = three_band_tone
    out = _apply_multiband_compressor(
        stereo, sr,
        low_threshold_db=0.0, low_ratio=1.0,
        mid_threshold_db=0.0, mid_ratio=1.0,
        high_threshold_db=0.0, high_ratio=1.0,
    )
    # Summed bands reconstruct the original (zero-phase splits are comb-free)
    # when no band is actually compressing. Trim the signal edges: sosfiltfilt
    # padding artifacts at each independent band-split's boundary don't
    # perfectly cancel on summation there, which is inherent to cascaded
    # zero-phase filtering (not a compressor-correctness issue) — the interior
    # is what actually demonstrates band-sum reconstruction.
    edge = SR // 20   # 50 ms
    np.testing.assert_allclose(out[:, edge:-edge], stereo[:, edge:-edge], atol=0.02)


def test_mid_side_gain_scales_mid_only_for_correlated_signal():
    t = np.arange(SR) / SR
    y = 0.5 * np.sin(2 * np.pi * 440 * t)
    mid_only = np.stack([y, y])   # L==R -> pure mid, zero side
    out = _apply_mid_side_processor(mid_only, SR, mid_gain_db=-6.0, side_gain_db=12.0)
    # Side gain must not create energy from a zero side channel.
    expected = mid_only * (10.0 ** (-6.0 / 20.0))
    np.testing.assert_allclose(out, expected, atol=1e-6)


def test_mid_side_gain_scales_side_only_for_antiphase_signal():
    t = np.arange(SR) / SR
    y = 0.5 * np.sin(2 * np.pi * 440 * t)
    side_only = np.stack([y, -y])   # L==-R -> pure side, zero mid
    out = _apply_mid_side_processor(side_only, SR, mid_gain_db=12.0, side_gain_db=-6.0)
    expected = side_only * (10.0 ** (-6.0 / 20.0))
    np.testing.assert_allclose(out, expected, atol=1e-6)


def test_mid_side_mono_input_is_noop():
    y = np.sin(2 * np.pi * 440 * np.arange(SR) / SR)
    out = _apply_mid_side_processor(y, SR, mid_gain_db=-6.0, side_gain_db=6.0)
    np.testing.assert_array_equal(out, y)


def test_clipper_never_exceeds_ceiling_regardless_of_hardness():
    t = np.arange(SR // 2) / SR
    loud = 1.8 * np.sin(2 * np.pi * 300 * t)
    stereo = np.stack([loud, loud])
    ceiling = 10.0 ** (-1.0 / 20.0)
    for hardness in (0.0, 0.5, 1.0):
        out = _apply_clipper(stereo, SR, ceiling_db=-1.0, hardness=hardness, mix=1.0)
        assert np.max(np.abs(out)) <= ceiling * 1.001, f"hardness={hardness} exceeded ceiling"


def test_clipper_mix_zero_is_transparent():
    t = np.arange(SR // 2) / SR
    loud = 1.8 * np.sin(2 * np.pi * 300 * t)
    stereo = np.stack([loud, loud])
    out = _apply_clipper(stereo, SR, ceiling_db=-6.0, hardness=1.0, mix=0.0)
    np.testing.assert_allclose(out, stereo, atol=1e-6)


def test_chain_dispatches_all_three_new_effects(three_band_tone):
    stereo, sr = three_band_tone
    processed, applied, skipped, _hr, _sl = _run_serial_chain(stereo, sr, [
        {"effect": "MultibandCompressor", "parameters": {"low_threshold_db": -30.0}},
        {"effect": "MidSideProcessor", "parameters": {"side_gain_db": -3.0}},
        {"effect": "Clipper", "parameters": {"ceiling_db": -1.0}},
    ])
    assert applied == ["MultibandCompressor", "MidSideProcessor", "Clipper"]
    assert skipped == []
    assert processed.shape == stereo.shape
    assert np.isfinite(processed).all()
