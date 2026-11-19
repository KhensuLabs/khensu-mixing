# local-services/khensu_services/analysis/pitch_accuracy.py
"""Pitch-Accuracy Map: per-note cents-deviation tuning analysis for a vocal
(or any monophonic-ish) take, using librosa's pYIN pitch tracker.

Unlike a broad "harshness"/"resonance" style score, this answers a concrete
question a singer/engineer actually asks: "which notes are flat/sharp, and by
how much?" — the analysis that a graphical pitch-correction editor (see the
UI/UX backlog item) would eventually be built on top of.

Pipeline:
  1. pYIN frame-level f0 + voiced flag.
  2. Convert each voiced frame's Hz to (nearest MIDI note, cents deviation)
     on the measurement grid: A=440 by default, or the caller's
     ``reference_hz`` (e.g. the mix's tuning reference). Not the take's own
     estimated reference - a take that is uniformly 30 cents flat would then
     read as perfectly in tune. The take's own reference is still estimated
     (circular mean of voiced-frame offsets from 12-TET) and reported as
     ``detected_reference_hz`` / ``reference_offset_cents``, with
     ``reference_offset_warning`` when it sits > 10 cents off the grid.
  3. Segment contiguous voiced runs into "notes" (a run ends when unvoiced,
     or when the nearest MIDI note changes and stays changed for
     ``_SPLIT_HYSTERESIS_SEC`` (~90 ms) — avoids treating vibrato/portamento
     as many tiny notes: a +-80 cent vibrato at 5-6 Hz spends ~40-60 ms past
     the half-semitone line per cycle, which the old ~35 ms hysteresis split
     into a phantom neighbour note, THY-12). Deviations are measured against the note's own
     centre, so a vibrato excursion past the half-semitone reads as +-60
     cents of that note, not as a separate neighbour note.
  4. Per note: mean cents deviation, stability (std dev of cents — a
     wavering note vs. a confidently-off one read very differently).

Stateless, numpy/librosa only.
"""
from __future__ import annotations

from khensu_services.analysis.context import load_audio
from typing import Any

import numpy as np
import numpy.typing as npt

from khensu_services.analysis._note_names import (
    hz_to_midi,
    midi_name,
    tuning_reference_report,
)

# A new note starts only after the nearest semitone has changed for this long
# (in consecutive voiced frames). ~90 ms: longer than one vibrato excursion
# past the half-semitone line, shorter than a real sung note (THY-12).
_SPLIT_HYSTERESIS_SEC = 0.090
# Kept for callers that segment at an unknown frame rate (~35 ms legacy).
_SPLIT_HYSTERESIS_FRAMES = 3
_MIN_NOTE_FRAMES = 3
_HOP = 512


def hysteresis_frames(frame_rate: float) -> int:
    """Frames that span ``_SPLIT_HYSTERESIS_SEC`` at ``frame_rate`` (>= 3)."""
    return max(_SPLIT_HYSTERESIS_FRAMES, int(np.ceil(_SPLIT_HYSTERESIS_SEC * float(frame_rate))))


def _midi_to_name(midi: float) -> str:
    return midi_name(midi)


def _segment_with_hysteresis(midi_exact: npt.NDArray, voiced: npt.NDArray,
                             hysteresis: int = _SPLIT_HYSTERESIS_FRAMES
                             ) -> list[tuple[int, int, int]]:
    """(start, end, note_midi) runs; a note change must persist for
    ``hysteresis`` voiced frames to split the run."""
    runs: list[tuple[int, int, int]] = []
    n = len(voiced)
    i = 0
    while i < n:
        if not voiced[i]:
            i += 1
            continue
        start = i
        # Seed from the median of the first `hysteresis` voiced frames, not the
        # first frame alone: a wide vibrato starting near its +80 cent peak
        # otherwise seeds the note a semitone up and splits it (librosa 0.11's
        # pyin starts tracking at a different phase than 0.10 did).
        head_end = i
        while head_end < n and voiced[head_end] and head_end - i < hysteresis:
            head_end += 1
        note = int(round(float(np.median(midi_exact[i:head_end]))))
        j = i + 1
        while j < n and voiced[j]:
            cand = int(round(midi_exact[j]))
            if cand != note:
                k = j
                while (k < n and voiced[k] and int(round(midi_exact[k])) == cand
                       and k - j < hysteresis):
                    k += 1
                if k - j >= hysteresis:
                    runs.append((start, j, note))
                    start, note = j, cand
            j += 1
        runs.append((start, j, note))
        i = j
    # Label each run by its median pitch and merge touching runs that land on
    # the same note: a split made while the seed was still settling is not a
    # note change.
    merged: list[tuple[int, int, int]] = []
    for start, end, _note in runs:
        label = int(round(float(np.median(midi_exact[start:end]))))
        if merged and merged[-1][1] == start and merged[-1][2] == label:
            merged[-1] = (merged[-1][0], end, label)
        else:
            merged.append((start, end, label))
    # A fragment shorter than two hysteresis windows that touches a note a
    # semitone away is a vibrato excursion, not a note: a tracker voiced from
    # the first frame (CREPE, NEW-7) catches a wide vibrato's opening rise as
    # its own 70 ms note. Fold it into the touching neighbour.
    changed = True
    while changed and len(merged) > 1:
        changed = False
        for k, (start, end, label) in enumerate(merged):
            if end - start >= 2 * hysteresis:
                continue
            nbrs = [m for m in (k - 1, k + 1) if 0 <= m < len(merged)
                    and (merged[m][1] == start or merged[m][0] == end)
                    and abs(merged[m][2] - label) <= 1]
            if not nbrs:
                continue
            m = max(nbrs, key=lambda q: merged[q][1] - merged[q][0])
            lo, hi = min(start, merged[m][0]), max(end, merged[m][1])
            merged[m] = (lo, hi, int(round(float(np.median(midi_exact[lo:hi])))))
            del merged[k]
            changed = True
            break
    return merged


def _detrended_std(cents: npt.NDArray) -> float:
    """Std of ``cents`` after removing a least-squares line: the note's wobble
    (vibrato, unsteadiness) without its scoop / drift."""
    c = np.asarray(cents, dtype=float)
    if c.size < 3:
        return float(np.std(c)) if c.size else 0.0
    x = np.arange(c.size, dtype=float)
    slope, intercept = np.polyfit(x, c, 1)
    return float(np.std(c - (slope * x + intercept)))


def note_deviations(y: npt.NDArray, sr: int, fmin: float = 70.0, fmax: float = 1200.0,
                    reference_hz: float | None = None) -> dict[str, Any]:
    """Per-note intonation of a monophonic signal against a tuning grid.

    The shared measurement behind /analyze/pitch_accuracy and the comping
    pitch score (THY-02). Returns ``{"notes": [...], "tuning": {...},
    "voiced": bool}``; each note carries ``start_sec``/``end_sec``,
    ``note_midi``, ``mean_cents`` (deviation from the nearest semitone of the
    grid - ``reference_hz``, else A=440, never the signal's own reference),
    ``stability_cents`` (std) and ``wobble_cents`` (detrended std)."""

    mono = y.mean(axis=0) if y.ndim > 1 else y
    # CREPE in the main environment when its model is present (NEW-7), pyin otherwise.
    from khensu_services.analysis import vocal_pitch
    f0, voiced_flag, _voiced_prob = vocal_pitch.track(
        mono.astype(np.float32), sr, fmin=fmin, fmax=fmax, hop_length=_HOP, frame_length=2048)
    frame_rate = sr / _HOP
    hop_sec = 1.0 / frame_rate
    voiced = voiced_flag & np.isfinite(f0)
    tuning = tuning_reference_report(np.where(voiced, f0, np.nan), frame_rate, reference_hz)
    if not np.any(voiced):
        return {"notes": [], "tuning": tuning, "voiced": False}
    grid_hz = tuning["measurement_reference_hz"]
    midi_exact = np.asarray(hz_to_midi(np.where(voiced, f0, np.nan), grid_hz), dtype=float)

    notes: list[dict[str, Any]] = []
    for start, end, note_midi in _segment_with_hysteresis(midi_exact, voiced,
                                                          hysteresis_frames(frame_rate)):
        if end - start < _MIN_NOTE_FRAMES:
            continue
        run_cents = (midi_exact[start:end] - note_midi) * 100.0
        # A frame more than half a semitone off the note is a different note:
        # a tracker glitch at a transition (librosa >= 0.11's pyin keeps a few
        # of these voiced). One ~+100 cent glitch would drag a -30 cent note's
        # mean toward 0, so measure the note on its own semitone's frames.
        own = run_cents[np.abs(run_cents) <= 50.0]
        mean_cents = own if own.size >= _MIN_NOTE_FRAMES else run_cents
        # Wobble keeps a vibrato's excursions past the half-semitone line but
        # not the transition glitches into the neighbouring notes (C -> E is
        # +400 cents): those inflated a steady note's wobble past a real 60
        # cent vibrato's under librosa 0.11's pyin.
        near = run_cents[np.abs(run_cents) < 100.0]
        wobble_frames = near if near.size >= _MIN_NOTE_FRAMES else run_cents
        notes.append({
            "start_sec": round(start * hop_sec, 3),
            "end_sec": round(end * hop_sec, 3),
            "note_midi": int(note_midi),
            "mean_cents": float(np.mean(mean_cents)),
            "stability_cents": float(np.std(mean_cents)),
            # Within a semitone of the note: a vibrato's excursions past the
            # half-semitone line belong to the note's wobble, a jump to the
            # next note doesn't.
            "wobble_cents": _detrended_std(wobble_frames),
        })
    return {"notes": notes, "tuning": tuning, "voiced": True}


def analyze_pitch_accuracy(y: npt.NDArray, sr: int,
                          fmin: float = 70.0, fmax: float = 1200.0,
                          reference_hz: float | None = None) -> dict[str, Any]:
    mono = y.mean(axis=0) if y.ndim > 1 else y
    if mono.size < sr // 4:
        return {"notes": [], "mean_abs_cents": 0.0, "in_tune_pct": 100.0,
                "worst_note": None, "verdict": "insufficient audio"}

    measured = note_deviations(mono, sr, fmin, fmax, reference_hz)
    tuning = measured["tuning"]
    if not measured["voiced"]:
        return {"notes": [], "mean_abs_cents": 0.0, "in_tune_pct": 100.0,
                "worst_note": None, "verdict": "no pitched content detected", **tuning}

    notes: list[dict[str, Any]] = [{
        "start_sec": nt["start_sec"],
        "end_sec": nt["end_sec"],
        "note_name": _midi_to_name(float(nt["note_midi"])),
        "note_midi": nt["note_midi"],
        "mean_cents_deviation": round(nt["mean_cents"], 1),
        "cents_stability": round(nt["stability_cents"], 1),
    } for nt in measured["notes"]]

    if not notes:
        return {"notes": [], "mean_abs_cents": 0.0, "in_tune_pct": 100.0,
                "worst_note": None, "verdict": "no stable notes detected", **tuning}

    all_abs_cents = [abs(nt["mean_cents_deviation"]) for nt in notes]
    mean_abs_cents = float(np.mean(all_abs_cents))
    # "In tune" threshold: within 15 cents is imperceptible to most listeners
    # for sustained pitch (JND for trained ears is roughly 5-10 cents, but
    # 15 is a forgiving, practical mixing-context bar).
    in_tune_pct = round(100.0 * sum(1 for c in all_abs_cents if c <= 15.0) / len(all_abs_cents), 1)
    worst = max(notes, key=lambda nt: abs(nt["mean_cents_deviation"]))

    if mean_abs_cents <= 10.0:
        verdict = "excellent pitch accuracy"
    elif mean_abs_cents <= 20.0:
        verdict = "solid — a few notes drift"
    elif mean_abs_cents <= 35.0:
        verdict = "noticeable pitch drift — correction recommended"
    else:
        verdict = "significant pitch issues"

    return {
        "notes": notes,
        "mean_abs_cents": round(mean_abs_cents, 1),
        "in_tune_pct": in_tune_pct,
        "worst_note": worst,
        "verdict": verdict,
        # The take's own detected concert-A (unchanged meaning); cents above
        # are measured against measurement_reference_hz, not this.
        "reference_hz": tuning["detected_reference_hz"],
        **tuning,
    }


def analyze_pitch_accuracy_file(file_path: str) -> dict[str, Any]:
    y, sr = load_audio(file_path, sr=None, mono=False)
    return analyze_pitch_accuracy(y, int(sr))
