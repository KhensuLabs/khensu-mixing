# local-services/khensu_services/analysis/comping.py
"""Comping Assistant: scores multiple vocal takes per phrase and proposes
a comp — the modern-standard best-of assembly, computed instead of
eyeballed.

Assumes takes were recorded against the same backing track (the normal
punch-in workflow), so they share a timeline and phrase boundaries can be
segmented once from the union of all takes' energy. Per phrase, each take
is scored on:

  pitch           — intonation: each note's deviation from the nearest
                    semitone of the tuning grid (the mix's reference when
                    given, else A=440) plus its detrended wobble beyond a
                    natural-vibrato allowance (THY-02; the shared
                    pitch_accuracy measurement). A phrase sung 35 cents sour
                    or a steady note 45 cents flat now scores low; the old
                    spread-around-the-phrase-median score rated both perfect
  level           — distance from the takes' median phrase level; the
                    take that matches the performance's energy wins over
                    an outlier (too timid / too hot)
  tone            — spectral-centroid distance from the takes' median;
                    consistent timbre comps together without seams

The comp render concatenates each phrase's winning take with equal-power
crossfades at the boundaries.

Stateless, numpy/librosa-only analysis; soundfile for the render.
"""
from __future__ import annotations

import os
from typing import Any

import numpy as np
import numpy.typing as npt
from khensu_services.analysis.context import load_audio

_WIN_SEC = 0.050
_GATE_OPEN_DB = 12.0
_MIN_PHRASE_SEC = 0.25
_MERGE_GAP_SEC = 0.30
_XFADE_SEC = 0.030

_W_PITCH, _W_LEVEL, _W_TONE = 0.40, 0.35, 0.25

# Pitch score: 1 - error / _PITCH_FULL_SCALE_CENTS, where error is the
# duration-weighted mean |deviation| plus half the wobble above the vibrato
# allowance (a +-60 cent vibrato has a ~42 cent std; controlled vibrato is
# musical, a wavering note is not).
_PITCH_FULL_SCALE_CENTS = 50.0
_VIBRATO_ALLOWANCE_CENTS = 25.0
_WOBBLE_WEIGHT = 0.5


def _window_rms_db(mono: npt.NDArray, sr: int) -> npt.NDArray:
    win = max(1, int(_WIN_SEC * sr))
    n_win = len(mono) // win
    if n_win == 0:
        return np.array([-90.0])
    rms = np.sqrt(np.mean(mono[: n_win * win].reshape(n_win, win) ** 2, axis=1) + 1e-20)
    return 20.0 * np.log10(rms + 1e-12)


def _segment_phrases(takes: list[npt.NDArray], sr: int) -> list[tuple[float, float]]:
    """Phrase boundaries from the max-energy union of all takes."""
    curves = [_window_rms_db(t, sr) for t in takes]
    n = min(len(c) for c in curves)
    union = np.max(np.stack([c[:n] for c in curves]), axis=0)

    floor = float(np.percentile(union, 10))
    gate = floor + _GATE_OPEN_DB
    active = union > gate

    win_sec = _WIN_SEC
    min_phrase = max(1, int(_MIN_PHRASE_SEC / win_sec))
    merge_gap = max(1, int(_MERGE_GAP_SEC / win_sec))

    phrases: list[tuple[float, float]] = []
    i = 0
    while i < n:
        if not active[i]:
            i += 1
            continue
        j = i
        gap = 0
        while j < n and gap <= merge_gap:
            gap = 0 if active[j] else gap + 1
            j += 1
        end = j - gap
        if end - i >= min_phrase:
            phrases.append((i * win_sec, end * win_sec))
        i = j
    return phrases


def phrase_intonation(seg: npt.NDArray, sr: int,
                      reference_hz: float | None = None) -> dict[str, float] | None:
    """(duration-weighted mean |cents| off the grid, mean detrended wobble)
    of one phrase, or None when it holds no measurable notes."""
    from khensu_services.analysis.pitch_accuracy import note_deviations

    measured = note_deviations(seg, sr, fmin=70.0, fmax=800.0, reference_hz=reference_hz)
    notes = measured["notes"]
    if not notes:
        return None
    dur = np.array([max(1e-3, n["end_sec"] - n["start_sec"]) for n in notes])
    dev = np.array([abs(n["mean_cents"]) for n in notes])
    wob = np.array([n["wobble_cents"] for n in notes])
    return {"mean_abs_cents": float(np.average(dev, weights=dur)),
            "wobble_cents": float(np.average(wob, weights=dur))}


def pitch_score_from(intonation: dict[str, float] | None) -> float:
    """0..1 intonation score (1 = on the grid, steady)."""
    if intonation is None:
        return 0.0
    error = intonation["mean_abs_cents"] + _WOBBLE_WEIGHT * max(
        0.0, intonation["wobble_cents"] - _VIBRATO_ALLOWANCE_CENTS)
    return float(np.clip(1.0 - error / _PITCH_FULL_SCALE_CENTS, 0.0, 1.0))


def _phrase_scores(seg: npt.NDArray, sr: int,
                   reference_hz: float | None = None) -> dict[str, Any]:
    """Raw per-phrase metrics for one take (scored relative to the other
    takes later)."""
    import librosa

    rms = float(np.sqrt(np.mean(seg ** 2) + 1e-20))
    rms_db = 20.0 * np.log10(rms + 1e-12)

    centroid = float(np.mean(librosa.feature.spectral_centroid(y=seg, sr=sr))) \
        if len(seg) > 2048 else 0.0

    intonation = None
    if len(seg) > 4096:
        try:
            intonation = phrase_intonation(seg, sr, reference_hz)
        except Exception:
            intonation = None

    return {"rms_db": rms_db, "centroid_hz": centroid, "intonation": intonation}


def analyze_comping(take_paths: list[str], render: bool = False,
                    output_path: str | None = None,
                    reference_hz: float | None = None) -> dict[str, Any]:
    """Score every take per phrase and (``render``) write the comp.

    ``output_path`` is the render destination (already validated by the
    router); None writes ``<take1>_comp.wav``. ``reference_hz`` is the mix's
    tuning reference the pitch score measures against (None: A=440)."""
    import soundfile as sf

    if len(take_paths) < 2:
        raise ValueError("Comping needs at least 2 takes")

    from khensu_services.copilot.dsp import require_finite_audio
    takes: list[npt.NDArray] = []
    sr0 = 0
    for p in take_paths:
        y, sr = load_audio(p, sr=None if sr0 == 0 else sr0, mono=True)
        require_finite_audio(y, "comping take")  # audit CQ-01
        if sr0 == 0:
            sr0 = int(sr)
        takes.append(y)

    phrases = _segment_phrases(takes, sr0)

    results: list[dict[str, Any]] = []
    for (start, end) in phrases:
        s0, s1 = int(start * sr0), int(end * sr0)
        raw = []
        for t in takes:
            seg = t[s0 : min(s1, len(t))]
            raw.append(_phrase_scores(seg, sr0, reference_hz) if len(seg) > 256
                       else {"rms_db": -90.0, "centroid_hz": 0.0, "intonation": None})

        med_rms = float(np.median([r["rms_db"] for r in raw]))
        med_cent = float(np.median([r["centroid_hz"] for r in raw if r["centroid_hz"] > 0]) or 0.0)

        scored = []
        for ti, r in enumerate(raw):
            pitch_score = pitch_score_from(r["intonation"])
            level_score = float(np.clip(1.0 - abs(r["rms_db"] - med_rms) / 12.0, 0.0, 1.0))
            tone_score = 1.0
            if med_cent > 0 and r["centroid_hz"] > 0:
                tone_score = float(np.clip(
                    1.0 - abs(np.log2(r["centroid_hz"] / med_cent)), 0.0, 1.0))
            total = _W_PITCH * pitch_score + _W_LEVEL * level_score + _W_TONE * tone_score
            scored.append({
                "take_index": ti,
                "pitch_score": round(pitch_score, 3),
                "level_score": round(level_score, 3),
                "tone_score": round(tone_score, 3),
                "total": round(total, 3),
                "mean_abs_cents": (round(r["intonation"]["mean_abs_cents"], 1)
                                   if r["intonation"] else None),
                "wobble_cents": (round(r["intonation"]["wobble_cents"], 1)
                                 if r["intonation"] else None),
            })

        best = max(range(len(scored)), key=lambda i: scored[i]["total"])
        results.append({
            "start_sec": round(start, 3),
            "end_sec": round(end, 3),
            "takes": scored,
            "best_take": best,
        })

    rendered_path = None
    if render and results:
        n_out = max(len(t) for t in takes)
        out = np.zeros(n_out, dtype=np.float64)
        xfade = max(1, int(_XFADE_SEC * sr0))
        for p in results:
            ti = p["best_take"]
            s0, s1 = int(p["start_sec"] * sr0), int(p["end_sec"] * sr0)
            s0r, s1r = max(0, s0 - xfade), min(len(takes[ti]), s1 + xfade)
            seg = takes[ti][s0r:s1r].astype(np.float64)
            env = np.ones(len(seg))
            ramp = min(xfade, len(seg) // 2)
            if ramp > 0:
                env[:ramp] = np.sin(np.linspace(0, np.pi / 2, ramp)) ** 2
                env[-ramp:] = np.cos(np.linspace(0, np.pi / 2, ramp)) ** 2
            out[s0r : s0r + len(seg)] += seg * env

        peak = float(np.max(np.abs(out)) + 1e-12)
        if peak > 0.999:
            out *= 0.999 / peak
        if output_path:
            rendered_path = output_path
        else:
            base, _ = os.path.splitext(take_paths[0])
            rendered_path = base + "_comp.wav"
        sf.write(rendered_path, out.astype(np.float32), sr0)

    return {
        "sample_rate": sr0,
        "take_count": len(takes),
        "phrases": results,
        "rendered_path": rendered_path,
    }
