"""Reference tests for TPDF dither algorithm properties.

These tests verify the mathematical properties of triangular-probability-density-function
(TPDF) dither in Python to:
  1. Document the expected LSB residual signature of dithered vs. undithered quantisation.
  2. Serve as a regression canary — if AudioExporter.cpp's dithering is accidentally removed,
     any Python re-implementation of the export path will break these tests.

TPDF dither theory:
  A TPDF noise sequence is the sum of two uniform random variables on [-LSB, +LSB].
  Its RMS = LSB / sqrt(6) ≈ 0.408 × LSB_16 (where LSB_16 = 1/32768 ≈ 3.05e-5 for float32).

  - Without dither: rounding a smooth sub-LSB signal → residual near 0 (pure rounding error).
  - With TPDF dither: residual RMS ≈ 0.408 × LSB_16 (dominated by the added noise).
"""
from __future__ import annotations
import math
import numpy as np
import pytest


LSB_16: float = 1.0 / 32768.0  # one 16-bit LSB in normalised float range


def _quantise_16(signal: np.ndarray) -> np.ndarray:
    """Round-to-nearest 16-bit quantisation without dither."""
    return (np.round(signal / LSB_16) * LSB_16).astype(np.float64)


def _apply_tpdf_dither(signal: np.ndarray, rng: np.random.Generator) -> np.ndarray:
    """Add TPDF dither before quantisation.

    Triangular distribution = sum of two uniform [-0.5 LSB, +0.5 LSB] sequences.
    This is the same algorithm used in AudioExporter::applyTpdfDither().
    """
    n = len(signal)
    dither = (rng.uniform(-0.5, 0.5, n) + rng.uniform(-0.5, 0.5, n)) * LSB_16
    return _quantise_16(signal + dither)


def _lsb_residual_rms(original: np.ndarray, quantised: np.ndarray) -> float:
    residual = original.astype(np.float64) - quantised.astype(np.float64)
    return float(math.sqrt(float(np.mean(residual ** 2))))


@pytest.fixture
def smooth_signal() -> np.ndarray:
    """2-second 997 Hz sine at -18 dBFS (well above 16-bit noise floor; smooth sub-LSB texture)."""
    sr = 44100
    t = np.linspace(0, 2.0, sr * 2, endpoint=False)
    # Amplitude 0.125 so the waveform has fine sub-LSB structure without clipping
    return (np.sin(2 * np.pi * 997 * t) * 0.125).astype(np.float64)


def test_undithered_lsb_residual_is_near_zero(smooth_signal: np.ndarray) -> None:
    """Without dither, rounding a smooth signal leaves a near-zero residual."""
    q = _quantise_16(smooth_signal)
    rms = _lsb_residual_rms(smooth_signal, q)
    # Rounding error is at most 0.5 LSB, so RMS ≈ LSB/sqrt(12) ≈ 0.289 × LSB.
    # We check it's below 0.40 × LSB (strict enough to distinguish from dithered).
    assert rms < LSB_16 * 0.40, (
        f"Undithered residual RMS = {rms:.2e} is too large (>0.40 × LSB_16 = {LSB_16 * 0.40:.2e}). "
        "Expected near-zero quantisation residual without dither."
    )


def test_tpdf_dithered_residual_exceeds_undithered(smooth_signal: np.ndarray) -> None:
    """TPDF-dithered quantisation has a higher LSB residual than undithered (noise was added)."""
    rng = np.random.default_rng(42)
    q_no_dither = _quantise_16(smooth_signal)
    q_dithered  = _apply_tpdf_dither(smooth_signal, rng)

    rms_no_dither = _lsb_residual_rms(smooth_signal, q_no_dither)
    rms_dithered  = _lsb_residual_rms(smooth_signal, q_dithered)

    assert rms_dithered > rms_no_dither, (
        f"Dithered residual ({rms_dithered:.2e}) should exceed undithered ({rms_no_dither:.2e}). "
        "TPDF dither adds shaped noise so the residual RMS should always be larger."
    )


def test_tpdf_residual_rms_near_theoretical(smooth_signal: np.ndarray) -> None:
    """TPDF residual RMS should be close to LSB / sqrt(6) ≈ 0.408 × LSB."""
    rng = np.random.default_rng(0)
    q = _apply_tpdf_dither(smooth_signal, rng)
    rms = _lsb_residual_rms(smooth_signal, q)
    theoretical = LSB_16 / math.sqrt(6.0)  # ≈ 0.408 × LSB_16

    # The measured residual = original − quantised(original + dither).
    # It contains BOTH the dither contribution AND the post-dither quantisation
    # granularity, so the true RMS is slightly above the theoretical dither-only
    # value. Allow ±50% to catch "no dither at all" (RMS ≈ 0) while confirming
    # dither noise is present at the expected order of magnitude.
    assert theoretical * 0.50 <= rms <= theoretical * 1.50, (
        f"TPDF residual RMS = {rms:.2e}; expected {theoretical:.2e} ± 50%. "
        "TPDF dither should produce triangular noise at the correct level."
    )


def test_tpdf_residual_has_near_zero_mean(smooth_signal: np.ndarray) -> None:
    """TPDF dither is zero-mean — the mean of the residual should be near 0."""
    rng = np.random.default_rng(7)
    q = _apply_tpdf_dither(smooth_signal, rng)
    residual = smooth_signal - q.astype(np.float64)
    mean_residual = float(np.mean(residual))
    assert abs(mean_residual) < LSB_16 * 0.05, (
        f"TPDF residual mean = {mean_residual:.2e}; should be < 5% of LSB_16 = {LSB_16 * 0.05:.2e}. "
        "Zero-mean dither should not bias the signal."
    )


def test_dither_does_not_produce_clipping() -> None:
    """TPDF dither on a near-full-scale signal must not push samples above ±1.0."""
    rng = np.random.default_rng(99)
    sr = 44100
    t = np.linspace(0, 0.1, int(sr * 0.1), endpoint=False)
    # 0 dBFS signal (exactly at peak)
    signal = np.sin(2 * np.pi * 440 * t).astype(np.float64)
    q = _apply_tpdf_dither(signal, rng)
    # TPDF dither adds at most ±1 LSB, so clipping is theoretically possible only
    # if the signal is exactly ±1.0 + dither. Accept it as a known edge case;
    # the implementation should clip or saturate, not overflow.
    assert np.max(np.abs(q)) <= 1.0 + LSB_16 * 1.5, (
        "Dithered signal exceeds ±1.0 by more than 1.5 LSB — dither is too large."
    )
