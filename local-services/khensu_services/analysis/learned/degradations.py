"""Programmatic mix-defect simulators for the self-supervised mix-quality model
(pipeline #9).

A calibrated "how professionally mixed does this sound" model needs labelled
good/bad audio, but human mix-quality ratings are exactly the blocked corpus.
Self-supervision sidesteps that: professionally-produced tracks (MUSDB18 /
Jamendo) are the CLEAN class, and these functions apply a known, common mix
defect to the same audio to make the DEFECTIVE class. The model then learns to
detect the *presence of known defects* (an explainable proxy for quality), not a
subjective score — and each defect maps to a concrete, fixable problem.

All are numpy-only, deterministic given a seeded RNG, and peak-normalize their
output so the model can't shortcut on loudness.
"""
from __future__ import annotations

import numpy as np

DEFECT_KINDS = ("clipping", "harshness", "mud", "overcompression")


def _peak_norm(y: np.ndarray, target: float = 0.9) -> np.ndarray:
    peak = float(np.max(np.abs(y))) + 1e-12
    return (y * (target / peak)).astype(np.float32)


def _band_boost(y: np.ndarray, sr: int, lo: float, hi: float, gain_db: float) -> np.ndarray:
    """Boost a frequency band by gain_db via rFFT (a crude but deterministic
    shelf/bell) — used to simulate harshness (2.5-5 kHz) and mud (150-400 Hz)."""
    spec = np.fft.rfft(y)
    freqs = np.fft.rfftfreq(len(y), 1.0 / sr)
    gain = 10.0 ** (gain_db / 20.0)
    spec[(freqs >= lo) & (freqs <= hi)] *= gain
    return np.fft.irfft(spec, n=len(y)).astype(np.float32)


def _overcompress(y: np.ndarray, sr: int, strength: float) -> np.ndarray:
    """Squash macro-dynamics: pull each ~50 ms block's RMS toward the track
    mean, `strength` in [0,1] (1 = fully flattened). Reduces crest factor, the
    over-limiting/over-compression tell."""
    block = max(1, int(0.05 * sr))
    n = len(y) // block
    if n < 2:
        return y.astype(np.float32)
    out = y.astype(np.float32).copy()
    trimmed = out[: n * block].reshape(n, block)
    rms = np.sqrt(np.mean(trimmed ** 2, axis=1)) + 1e-9
    target = float(np.mean(rms))
    gain = (1.0 - strength) + strength * (target / rms)
    trimmed *= gain[:, None]
    out[: n * block] = trimmed.reshape(-1)
    return out


def apply_defect(y: np.ndarray, sr: int, kind: str, rng: np.random.Generator) -> np.ndarray:
    """Apply one defect `kind` (see DEFECT_KINDS) with a randomized severity,
    peak-normalized. Raises on an unknown kind."""
    y = np.asarray(y, dtype=np.float32)
    if kind == "clipping":
        thr = float(rng.uniform(0.15, 0.4))          # hard clip well below peak
        out = np.clip(y, -thr, thr)
    elif kind == "harshness":
        out = _band_boost(y, sr, 2500.0, 5000.0, float(rng.uniform(8.0, 16.0)))
    elif kind == "mud":
        out = _band_boost(y, sr, 150.0, 400.0, float(rng.uniform(8.0, 16.0)))
    elif kind == "overcompression":
        out = _overcompress(y, sr, float(rng.uniform(0.7, 1.0)))
    else:
        raise ValueError(f"unknown defect kind: {kind}")
    return _peak_norm(out)


def random_defect(y: np.ndarray, sr: int, rng: np.random.Generator) -> tuple[np.ndarray, str]:
    """Apply a uniformly-chosen defect; returns (degraded_audio, kind)."""
    kind = str(rng.choice(DEFECT_KINDS))
    return apply_defect(y, sr, kind, rng), kind
