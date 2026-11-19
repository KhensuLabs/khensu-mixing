"""Deterministic, music-like stereo test signals (no audio files, no models).

Shared by ``test_metamorphic_invariants.py`` and ``test_golden_regression.py``.

Each song is a four-bar loop - a chord progression of harmonic tones with an
amplitude envelope, a bass line, a kick on every beat, a snare on the
backbeat, off-beat hats and a pink-ish bed - tiled into an arrangement whose
first part is quieter than the rest, so loudness range is not trivially zero.
Left and right differ: chord tones and hats are panned, the bed has
independent phases per channel.

Everything is written as a function of time in seconds and is band-limited
(no partial above 14 kHz, raised-cosine envelopes), so the same song can be
*synthesised* at 44.1 kHz and at 48 kHz - not resampled - and compared. The
only randomness is a seeded ``numpy`` generator used for partial frequencies
and phases.
"""
from __future__ import annotations

from functools import lru_cache

import numpy as np

# name -> tempo, chord roots/intervals (one chord per 4/4 bar), panning, seed.
SONGS: dict[str, dict] = {
    # I - IV - V - I in C major, 120 BPM: an 8 s loop.
    "c_major_120": {
        "bpm": 120.0,
        "chords": [(48, (0, 4, 7)), (53, (0, 4, 7)), (55, (0, 4, 7)), (48, (0, 4, 7))],
        "pad_pan": (1.0, 0.35), "hat_pan": (0.2, 1.0), "seed": 7,
        "key": "C major",
    },
    # i - VI - i - V in A minor, 96 BPM: a 10 s loop.
    "a_minor_96": {
        "bpm": 96.0,
        "chords": [(45, (0, 3, 7)), (41, (0, 4, 7)), (45, (0, 3, 7)), (52, (0, 4, 7))],
        "pad_pan": (0.3, 1.0), "hat_pan": (1.0, 0.25), "seed": 11,
        "key": "A minor",
    },
}

_BEATS_PER_BAR = 4
PAD_HARMONICS = (1, 2, 3, 4)
CLICK = 0.06
SNARE = 0.03
HAT = 0.02
_QUIET_LEVEL = 0.45          # the quiet part sits 20*log10(0.45) = -6.9 dB down


def _hz(midi: float) -> float:
    return 440.0 * 2.0 ** ((midi - 69.0) / 12.0)


def _raised_cosine(x: np.ndarray) -> np.ndarray:
    return 0.5 - 0.5 * np.cos(np.pi * np.clip(x, 0.0, 1.0))


def _pluck(x: np.ndarray, decay: float, attack: float = 0.003) -> np.ndarray:
    """Smooth attack, exponential decay; ``x`` is seconds since the hit (>= 0)."""
    return _raised_cosine(x / attack) * np.exp(-x / decay)


def _partials(x: np.ndarray, freqs: np.ndarray, amps: np.ndarray, phases: np.ndarray) -> np.ndarray:
    out = np.zeros_like(x)
    for f, a, p in zip(freqs, amps, phases):
        out += a * np.sin(2.0 * np.pi * f * x + p)
    return out


def loop_seconds(name: str) -> float:
    return len(SONGS[name]["chords"]) * _BEATS_PER_BAR * 60.0 / SONGS[name]["bpm"]


@lru_cache(maxsize=None)
def _loop(name: str, sr: int) -> np.ndarray:
    """One exactly periodic four-bar loop, float64 ``(2, n)``."""
    cfg = SONGS[name]
    rng = np.random.default_rng(cfg["seed"])
    beat = 60.0 / cfg["bpm"]
    bar = beat * _BEATS_PER_BAR
    seconds = loop_seconds(name)
    n = int(round(seconds * sr))
    t = np.arange(n) / float(sr)
    left = np.zeros(n)
    right = np.zeros(n)

    # Chords and bass: one chord per bar, phases counted from the bar line.
    for b, (root, intervals) in enumerate(cfg["chords"]):
        lo, hi = int(round(b * bar * sr)), int(round((b + 1) * bar * sr))
        x = t[lo:hi] - b * bar
        gate = _raised_cosine(x / 0.02) * _raised_cosine((bar - x) / 0.08)
        pad_l = np.zeros_like(x)
        pad_r = np.zeros_like(x)
        for k, iv in enumerate(intervals):
            f0 = _hz(root + 12 + iv)
            tone = np.zeros_like(x)
            for h in PAD_HARMONICS:
                tone += np.sin(2.0 * np.pi * f0 * h * x + 0.7 * k + 0.3 * h) / h
            # Chord tones lean alternately left and right.
            pad_l += cfg["pad_pan"][k % 2] * tone
            pad_r += cfg["pad_pan"][(k + 1) % 2] * tone
        bass = np.zeros_like(x)
        for h in range(1, 5):
            bass += np.sin(2.0 * np.pi * _hz(root - 12) * h * x) / (h * h)
        left[lo:hi] += gate * (0.07 * pad_l + 0.10 * bass)
        right[lo:hi] += gate * (0.07 * pad_r + 0.10 * bass)

    # Drums: kick on every beat (accented downbeat), snare on 2 and 4, hats
    # on the off-beat eighths.
    click_f = rng.uniform(700.0, 9000.0, 32)
    click_p = rng.uniform(0.0, 2.0 * np.pi, 32)
    snare_f = rng.uniform(1400.0, 6000.0, 24)
    snare_p = rng.uniform(0.0, 2.0 * np.pi, 24)
    hat_f = rng.uniform(6500.0, 11500.0, 14)
    hat_p = rng.uniform(0.0, 2.0 * np.pi, 14)
    for i in range(len(cfg["chords"]) * _BEATS_PER_BAR):
        start = i * beat
        lo = int(round(start * sr))
        hi = min(n, lo + int(0.45 * sr))
        x = t[lo:hi] - start
        x = np.maximum(x, 0.0)
        accent = 1.0 if i % _BEATS_PER_BAR == 0 else 0.7
        # 110 -> 45 Hz sweep.
        phase = 2.0 * np.pi * (45.0 * x + 65.0 * 0.05 * (1.0 - np.exp(-x / 0.05)))
        kick = accent * 0.24 * _pluck(x, 0.12) * np.sin(phase)
        kick = kick + accent * CLICK * _pluck(x, 0.012, attack=0.001) * _partials(
            x, click_f, np.full(32, 0.25), click_p)
        left[lo:hi] += kick
        right[lo:hi] += kick
        if i % _BEATS_PER_BAR in (1, 3):
            snare = SNARE * _pluck(x, 0.07) * _partials(x, snare_f, np.full(24, 0.25), snare_p)
            left[lo:hi] += 0.85 * snare
            right[lo:hi] += snare
        h_start = start + beat / 2.0
        h_lo = int(round(h_start * sr))
        h_hi = min(n, h_lo + int(0.15 * sr))
        hx = np.maximum(t[h_lo:h_hi] - h_start, 0.0)
        hat = HAT * _pluck(hx, 0.03, attack=0.002) * _partials(hx, hat_f, np.full(14, 1.0 / 3.0), hat_p)
        left[h_lo:h_hi] += cfg["hat_pan"][0] * hat
        right[h_lo:h_hi] += cfg["hat_pan"][1] * hat

    # Pink-ish bed: 1/f power, frequencies snapped to the loop's harmonics so
    # the loop tiles without a click, independent phases left and right.
    bed_f = np.round(np.geomspace(60.0, 14000.0, 64) * rng.uniform(0.985, 1.015, 64) * seconds) / seconds
    bed_a = 0.02 / np.sqrt(bed_f / 60.0)
    left += _partials(t, bed_f, bed_a, rng.uniform(0.0, 2.0 * np.pi, 64))
    right += _partials(t, bed_f, bed_a, rng.uniform(0.0, 2.0 * np.pi, 64))

    out = np.stack([left, right])
    out.setflags(write=False)
    return out


@lru_cache(maxsize=None)
def song(name: str, sr: int, loops: int = 2, quiet_loops: int = 1) -> np.ndarray:
    """``loops`` repeats of the loop; the first ``quiet_loops`` are quieter.

    Returns a read-only float64 array shaped ``(2, samples)`` - copy before
    changing it in place.
    """
    one = _loop(name, sr)
    y = np.tile(one, (1, loops))
    t = np.arange(y.shape[1]) / float(sr)
    edge = quiet_loops * loop_seconds(name)
    # 50 ms raised-cosine step between the two levels (band-limited).
    level = _QUIET_LEVEL + (1.0 - _QUIET_LEVEL) * _raised_cosine((t - edge) / 0.05 + 0.5)
    y = y * level
    y.setflags(write=False)
    return y
