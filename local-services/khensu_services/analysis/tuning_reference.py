# local-services/khensu_services/analysis/tuning_reference.py
"""Tuning-reference detection: concert-pitch A and per-section drift.

Is the track tuned to A=440, A=432 (or another reference), or drifting? Uses
librosa's tuning estimate (deviation from the 12-TET grid) globally and per
window, converts it to the implied reference frequency in Hz and cents, and
flags material drift across the song (a red flag for stitched-together or
varispeed sources).
"""
from __future__ import annotations

from typing import Any

import numpy as np
from khensu_services.analysis import _lean_spectral as _lean

from khensu_services.analysis._note_names import (
    cents_to_ref_hz,
    circular_cents_diff,
    circular_mean_cents,
)


_SEMITONE_CAVEAT = ("Tuning is measured modulo one semitone (+-50 cents): a reference a "
                    "whole semitone away reads the same - A={lo:.1f} Hz (e.g. A=415 "
                    "baroque pitch) or A={hi:.1f} Hz cannot be told apart from this "
                    "reading by the audio alone.")


def _cents_to_ref_hz(cents: float) -> float:
    return cents_to_ref_hz(cents)


def _classify(cents: float) -> str:
    ref = _cents_to_ref_hz(cents)
    if abs(cents) < 8.0:
        return "A=440 (standard)"
    # A=432 is ~-31.8 cents from 440.
    if abs(ref - 432.0) < 1.5:
        return "A=432"
    sign = "sharp" if cents > 0 else "flat"
    return f"~{abs(round(cents))} cents {sign} of A=440 (A={ref:.1f} Hz)"


def analyze_tuning_reference(file_path: str, window_sec: float = 10.0) -> dict[str, Any]:
    import librosa

    from khensu_services.analysis.context import load_audio
    y, sr = load_audio(file_path, sr=None, mono=True)
    # librosa.estimate_tuning returns deviation in fractions of a semitone.
    global_semitones = float(_lean.estimate_tuning(y, sr))
    global_cents = round(global_semitones * 100.0, 1)

    # Per-window drift.
    win = int(window_sec * sr)
    drift: list[dict[str, float]] = []
    if win > sr and len(y) >= win:  # need at least ~1 s windows
        # Every full window, including the final one (the old range() stop
        # of len(y) - win dropped it whenever the length was an exact multiple).
        for start in range(0, len(y) - win + 1, win):
            seg = y[start:start + win]
            try:
                st = float(librosa.estimate_tuning(y=seg, sr=sr))
            except Exception:
                continue
            drift.append({"time_sec": round(start / sr, 2), "cents": round(st * 100.0, 1)})

    # Tuning offsets live on a 100-cent circle (+49 and -49 cents are 2 cents
    # apart, not 98): measure the spread as circular differences around the
    # windows' circular mean.
    drift_cents = [d["cents"] for d in drift]
    if drift_cents:
        centre = circular_mean_cents(drift_cents)
        rel = np.asarray(circular_cents_diff(drift_cents, centre), dtype=float)
        drift_range = round(float(rel.max() - rel.min()), 1)
    else:
        drift_range = 0.0

    ref_hz = _cents_to_ref_hz(global_cents)
    return {
        "reference_hz": round(ref_hz, 1),
        "cents_from_440": global_cents,
        "verdict": _classify(global_cents),
        "drift_range_cents": drift_range,
        "drifting": bool(drift_range > 20.0),
        "drift": drift,
        # THY-12: tuning is measured modulo one semitone (+-50 cents), so a
        # reference a whole semitone away reads exactly like this one.
        "semitone_alternatives_hz": [round(ref_hz * 2.0 ** (-1.0 / 12.0), 1),
                                     round(ref_hz * 2.0 ** (1.0 / 12.0), 1)],
        "caveat": _SEMITONE_CAVEAT.format(lo=ref_hz * 2.0 ** (-1.0 / 12.0),
                                          hi=ref_hz * 2.0 ** (1.0 / 12.0)),
    }
