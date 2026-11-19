"""Helpers shared between quick_analyzer.py and advanced_analyzer.py.

Both modules independently duplicated (a) the BPM-confidence-from-beat-times
calculation and (b) the cluster-label -> contiguous-section-list conversion
used by their respective section-detection paths. Extracted here so the two
analyzers can't drift out of sync.

Also home to the beat-level rhythm helpers both use: the beat-fit tempo
(:func:`refine_bpm` - every reported BPM goes through it instead of the
quantised tempogram bin), downbeat-phase and meter (time-signature)
estimation from beat-synchronous accent patterns, and the octave-folded tempo
comparison used wherever a detected BPM is matched against a genre's tempo
range.
"""
from __future__ import annotations

import math
from typing import Any

import numpy as np


def bpm_confidence_from_beats(beat_times: list[float]) -> float:
    """BPM confidence from inter-beat interval consistency.

    A steady tempo has near-constant inter-beat intervals (low coefficient of
    variation); confidence is 1 - cv, clamped to [0, 1]. Falls back to 0.5
    when there are too few beats to estimate reliably.
    """
    if len(beat_times) < 4:
        return 0.5
    ibi = np.diff(beat_times)
    cv = float(np.std(ibi) / (np.mean(ibi) + 1e-9))
    return float(np.clip(1.0 - cv, 0.0, 1.0))


_MIN_BEATS_FOR_REFINED_BPM = 8


def refine_bpm(beat_times: Any, fallback: float,
               min_beats: int = _MIN_BEATS_FOR_REFINED_BPM) -> float:
    """Tempo from the tracked beat times, not the tempogram bin.

    ``librosa.beat.beat_track`` reports its tempo as a tempogram bin, which is
    quantised (128 BPM reads 129.2, 140 at 22.05 kHz reads 143.6), while the
    beats it tracks are spaced at the real period. This fits beat time against
    beat index by least squares and returns ``60 / slope``.

    Robust to tracker slips: each beat's index advances by the number of
    median periods since the previous kept beat (so a missed beat counts as
    two steps and a spurious extra beat is dropped), and after the first fit
    beats more than a fifth of a period off the line are trimmed and the line
    refitted. Falls back to ``fallback`` with fewer than ``min_beats`` beats
    or a degenerate fit.
    """
    try:
        bt = np.asarray(beat_times, dtype=float).reshape(-1)
    except (TypeError, ValueError):
        return float(fallback)
    bt = np.sort(bt[np.isfinite(bt)])
    if bt.size < max(2, min_beats):
        return float(fallback)
    period = float(np.median(np.diff(bt)))
    if not math.isfinite(period) or period <= 1e-3:
        return float(fallback)

    idx, times = [0.0], [float(bt[0])]
    for t in bt[1:]:
        steps = round((float(t) - times[-1]) / period)
        if steps < 1:
            continue                      # extra beat inside half a period
        idx.append(idx[-1] + steps)
        times.append(float(t))
    if len(times) < max(2, min_beats):
        return float(fallback)
    x, y = np.asarray(idx), np.asarray(times)
    slope, intercept = np.polyfit(x, y, 1)
    resid = y - (slope * x + intercept)
    keep = np.abs(resid) <= 0.2 * abs(slope)
    if int(np.count_nonzero(keep)) >= max(2, min_beats) and not bool(np.all(keep)):
        slope, intercept = np.polyfit(x[keep], y[keep], 1)
    if not math.isfinite(slope) or slope <= 1e-3:
        return float(fallback)
    bpm = 60.0 / float(slope)
    return bpm if 20.0 <= bpm <= 400.0 else float(fallback)


def tracked_tempo(tempo: Any, beat_times: Any) -> float:
    """``refine_bpm`` over beat_track's own output: the beat-fit tempo, with the
    tempogram value (``tempo``, scalar or 1-element array) as the fallback."""
    est = float(np.atleast_1d(tempo)[0]) if np.size(tempo) else 0.0
    return refine_bpm(beat_times, est if math.isfinite(est) else 0.0)


def label_sections_from_clusters(starts: list[float], labels: np.ndarray) -> list[dict]:
    """Turns per-segment cluster labels into a contiguous {start_sec, label} list.

    Assigns letters (A, B, C, ...) to clusters in first-seen order, then
    collapses consecutive segments sharing a cluster into a single section
    (so e.g. 3 consecutive "verse"-cluster segments become one "A" section,
    not three).
    """
    letter_map: dict[int, str] = {}
    letter_idx = 0
    sections: list[dict] = []

    for start, cluster in zip(starts, labels):
        cluster = int(cluster)
        if cluster not in letter_map:
            letter_map[cluster] = chr(ord("A") + letter_idx)
            letter_idx += 1
        letter = letter_map[cluster]
        if not sections or sections[-1]["label"] != letter:
            sections.append({"start_sec": float(start), "label": letter})

    return sections or [{"start_sec": 0.0, "label": "A"}]


# --- Tempo octave folding ------------------------------------------------------

# Genre tempo ranges in resources/genre_norms/genre_targets.json are quoted as
# the notated / DAW-project tempo (see that file's "tempo_convention"). Beat
# trackers routinely lock to half or double that pulse, so every comparison of
# a detected BPM against a genre range goes through these helpers.
_TEMPO_OCTAVE_FACTORS = (0.5, 1.0, 2.0)


def octave_folded_bpm_distance(bpm: float, target_bpm: float,
                               fold_penalty: float = 0.0) -> float:
    """Smallest |f*bpm - target| (+ ``fold_penalty`` when f != 1) over the
    tempo octaves f in {1/2, 1, 2}.

    A 70 BPM half-time read of a 140 BPM trap beat is the same groove, so it
    sits close to a 140 BPM anchor (and vice versa). ``fold_penalty`` (BPM)
    charges a folded match for the octave ambiguity, so a direct match is
    preferred when both are comparable - without it a 70 BPM track sat at
    the same distance from a 70 and a 140 BPM genre, blurring the two.
    """
    b, t = float(bpm), float(target_bpm)
    if not (math.isfinite(b) and math.isfinite(t)) or b <= 0.0:
        return abs(b - t) if math.isfinite(b) and math.isfinite(t) else float("inf")
    penalty = max(0.0, float(fold_penalty))
    return min(abs(f * b - t) + (0.0 if f == 1.0 else penalty) for f in _TEMPO_OCTAVE_FACTORS)


def fold_bpm_to_range(bpm: float, bpm_min: float, bpm_max: float) -> float:
    """The tempo octave (bpm * 1/2, 1, or 2) of ``bpm`` closest to [min, max].

    Returns the folded value itself (inside the range when any octave lands
    there; otherwise the octave nearest the range). Non-positive / non-finite
    input is returned unchanged.
    """
    b = float(bpm)
    if not math.isfinite(b) or b <= 0.0:
        return b
    lo, hi = sorted((float(bpm_min), float(bpm_max)))

    def _gap(v: float) -> float:
        return 0.0 if lo <= v <= hi else min(abs(v - lo), abs(v - hi))

    # Prefer the unfolded tempo on ties (1.0 is tried first).
    return min((f * b for f in (1.0, 0.5, 2.0)), key=_gap)


def bpm_in_range_folded(bpm: float, bpm_min: float, bpm_max: float) -> bool:
    """True when some tempo octave of ``bpm`` falls inside [bpm_min, bpm_max]."""
    folded = fold_bpm_to_range(bpm, bpm_min, bpm_max)
    lo, hi = sorted((float(bpm_min), float(bpm_max)))
    return math.isfinite(folded) and lo <= folded <= hi


def fold_bpm_to_window(bpm: float, lo: float = 60.0, hi: float = 200.0) -> float:
    """Fold ``bpm`` by octaves into [lo, hi) (no-op inside it; <= 0 unchanged)."""
    b = float(bpm)
    if not math.isfinite(b) or b <= 0.0 or hi < 2.0 * lo:
        return b
    while b < lo:
        b *= 2.0
    while b >= hi:
        b /= 2.0
    return b


def quarter_note_bpm(meter: dict[str, Any] | str | None, bpm: float | None,
                     beats_per_bar: int | None = None) -> float | None:
    """The quarter-note (MIDI / DAW-session) tempo of a tracked pulse (THY-09).

    One tempo convention for every consumer: MIDI ``set_tempo`` counts
    quarter notes, and ``genre_targets.json`` quotes its ranges as the
    quarter-note session tempo. In simple meters (4/4, 3/4) the tracked beat
    already is the quarter. A 6/8 bar holds three quarters, tracked either as
    six eighth notes (``beats_per_bar`` 6: quarter = pulse / 2, so a 180 BPM
    eighth pulse is 90) or as two dotted quarters (``beats_per_bar`` 2:
    quarter = pulse x 1.5, so a 60 BPM felt pulse is 90).

    ``meter`` is :func:`estimate_meter`'s dict or a "6/8"-style string (then
    pass ``beats_per_bar``; 6/8 without it assumes the eighth-note pulse).
    None / non-positive ``bpm`` -> None.
    """
    if bpm is None:
        return None
    b = float(bpm)
    if not math.isfinite(b) or b <= 0.0:
        return None
    if isinstance(meter, dict):
        signature = str(meter.get("time_signature") or DEFAULT_TIME_SIGNATURE)
        bpb = int(meter.get("beats_per_bar") or 0) if beats_per_bar is None else int(beats_per_bar)
    else:
        signature = str(meter or DEFAULT_TIME_SIGNATURE)
        bpb = int(beats_per_bar) if beats_per_bar is not None else 6
    if signature == "6/8":
        return b * 1.5 if bpb == 2 else b / 2.0
    return b


# --- Beat-level accent features ------------------------------------------------

_HOP = 512


def _beat_accent_features(y: np.ndarray, sr: int,
                          beat_times: list[float]) -> dict[str, Any] | None:
    """Per-beat accent descriptors shared by downbeat-phase and meter estimation.

    * ``bass``   - low-band (20-200 Hz) spectral flux, max within +-2 frames
                   of the beat (kick / bass-note onsets);
    * ``onset``  - full-band spectral flux at the beat (dynamic accents);
    * ``change`` - cosine distance between the beat-synchronous chroma of the
                   interval before and after the beat (chord changes);
    * ``chroma`` - unit-norm beat-synchronous chroma, (12, n_beats + 1);
    * ``flux`` / ``frames`` - the full-band flux envelope and beat frames, for
                   subdivision analysis.

    None when the signal is too short or the STFT fails.
    """
    import librosa

    n_beats = len(beat_times)
    if n_beats < 2 or len(y) < 2048:
        return None
    S = np.abs(librosa.stft(np.asarray(y, dtype=np.float32), n_fft=2048, hop_length=_HOP))
    freqs = librosa.fft_frequencies(sr=sr, n_fft=2048)
    low = np.log1p(S[(freqs > 20.0) & (freqs < 200.0)])
    if low.shape[0] == 0 or S.shape[1] < 4:
        return None
    low_flux = np.concatenate([[0.0], np.maximum(0.0, np.diff(low, axis=1)).sum(axis=0)])
    full_flux = np.concatenate([[0.0],
                                np.maximum(0.0, np.diff(np.log1p(S), axis=1)).sum(axis=0)])
    # chroma_stft would estimate tuning with piptrack over the whole power
    # spectrogram (several full-size copies); the chunked estimate is the same value.
    from khensu_services.analysis._lean_spectral import estimate_tuning_from_spectrogram

    power = S ** 2
    chroma = librosa.feature.chroma_stft(
        S=power, sr=sr, tuning=estimate_tuning_from_spectrogram(power, sr, bins_per_octave=12))
    del power
    frames = np.clip(np.round(np.asarray(beat_times) * sr / _HOP).astype(int),
                     0, S.shape[1] - 1)

    bass = np.array([float(low_flux[max(0, f - 2):f + 3].max()) for f in frames])
    onset = np.array([float(full_flux[max(0, f - 2):f + 3].max()) for f in frames])
    sync = librosa.util.sync(chroma, frames, aggregate=np.mean)  # (12, n_beats + 1)
    sync = sync / (np.linalg.norm(sync, axis=0, keepdims=True) + 1e-9)
    change = np.zeros(n_beats)
    for k in range(n_beats):
        if k + 1 < sync.shape[1]:
            change[k] = 1.0 - float(sync[:, k] @ sync[:, k + 1])
    return {"bass": bass, "onset": onset, "change": change, "chroma": sync,
            "flux": full_flux, "frames": frames}


def _mean_norm(v: np.ndarray) -> np.ndarray:
    m = float(np.mean(v))
    return v / m if m > 1e-9 else np.zeros_like(v)


def _phase_from_features(feats: dict[str, Any], beats_per_bar: int) -> int:
    score = _mean_norm(feats["bass"]) + _mean_norm(feats["change"])
    phase_scores = np.array([float(np.mean(score[p::beats_per_bar]))
                             for p in range(beats_per_bar)])
    best = int(np.argmax(phase_scores))
    ordered = np.sort(phase_scores)
    # Need a real preference (>5 % over the runner-up) to move off phase 0.
    if ordered[-1] - ordered[-2] < 0.05 * max(ordered[-1], 1e-9):
        return 0
    return best


def estimate_downbeat_phase(y: np.ndarray, sr: int, beat_times: list[float],
                            beats_per_bar: int = 4) -> int:
    """Which beat (0..beats_per_bar-1) of the tracked beat list starts a bar.

    librosa has no downbeat tracker and both analyzers used to assume the
    first tracked beat is beat 1 (``beat_times[::4]``) - wrong for every
    pickup / intro that starts off the bar. Score each candidate phase by the
    mean, over the beats it would call downbeats, of

    * low-frequency (<200 Hz) spectral flux at the beat - kicks and bass
      notes land on the bar line far more often than elsewhere, and
    * the beat-synchronous chroma change across the beat - chords change on
      the bar line far more often than mid-bar,

    each normalised by its mean over all beats so neither dominates. Returns
    0 (the historical behaviour) when there are too few beats or the scores
    don't separate. Works for any ``beats_per_bar`` (pass the value from
    :func:`estimate_meter`; 4 is the default for callers with no meter).
    """
    n_beats = len(beat_times)
    if beats_per_bar < 2 or n_beats < 2 * beats_per_bar or len(y) < 2048:
        return 0
    try:
        feats = _beat_accent_features(y, sr, beat_times)
        if feats is None:
            return 0
        return _phase_from_features(feats, beats_per_bar)
    except Exception:
        return 0


# --- Meter / time-signature estimation ------------------------------------------

DEFAULT_TIME_SIGNATURE = "4/4"
_FALLBACK_METER_CONFIDENCE = 0.2
# A non-4/4 reading must beat 4/4 by this much (accent-autocorrelation units)
# before it is reported - most music is in 4, so the prior leans that way.
_TRIPLE_MIN_MARGIN = 0.15
_MIN_BEATS_FOR_METER = 12


def _zscore(v: np.ndarray) -> np.ndarray | None:
    v = np.asarray(v, dtype=float)
    sd = float(np.std(v))
    if sd < 1e-9 * max(1.0, float(np.mean(np.abs(v)))) or sd < 1e-12:
        return None
    return (v - float(np.mean(v))) / sd


def _lag_corr(a: np.ndarray, lag: int) -> float:
    """Normalised autocorrelation of an already z-scored sequence at ``lag``."""
    if lag <= 0 or lag >= len(a) - 1:
        return 0.0
    return float(np.mean(a[:-lag] * a[lag:]))


def _accent_periodicity(feats: dict[str, Any]) -> dict[int, float] | None:
    """Mean lag-autocorrelation of the per-beat accent descriptors.

    Each of bass / onset / chroma-change is z-scored over the track and
    autocorrelated at lags 2..8 beats, and the curves are averaged weighted by
    each descriptor's relative spread (coefficient of variation, capped at 1):
    a descriptor that barely varies from beat to beat (identical clicks, a
    hop-alignment ripple in the full-band flux, chroma "change" on unpitched
    material) carries no accent and must not be amplified by the z-score.
    An accent recurring every bar makes the curve peak at the bar length and
    its multiples. None when no descriptor varies meaningfully.
    """
    curves, weights = [], []
    for key in ("bass", "onset", "change"):
        v = np.asarray(feats[key], dtype=float)
        mean = float(np.mean(v))
        if key == "change" and float(np.max(v, initial=0.0)) < 0.05:
            continue                      # no real chord changes anywhere
        z = _zscore(v)
        if z is None or mean <= 1e-9:
            continue
        w = min(1.0, float(np.std(v)) / mean)
        if w < 0.2:   # beat-to-hop alignment alone ripples the flux by ~10 %
            continue
        curves.append([_lag_corr(z, lag) for lag in range(2, 9)])
        weights.append(w)
    if not curves:
        return None
    avg = np.average(np.asarray(curves), axis=0, weights=np.asarray(weights))
    return {lag: float(avg[i]) for i, lag in enumerate(range(2, 9))}


def _harmonic_bar_similarities(chroma: np.ndarray) -> tuple[float | None, float | None]:
    """(4/4, 3/4) bar-level chroma self-similarity over EQUAL bar counts.

    Each candidate is compared at 2/4/8-bar lags (8/16/32 beats vs 6/12/24),
    but a bar count is used only when the track is long enough for the
    longer (4/4) lag: otherwise a short track would average 3/4 over 2/4/8
    bars against 4/4 over 2/4 bars only, and the extra long-lag term (lower
    similarity) biased the comparison.
    """
    n = chroma.shape[1] - 1
    bars = tuple(b for b in (2, 4, 8) if 4 * b < n - 2)
    if not bars:
        return None, None
    return (_harmonic_bar_similarity(chroma, tuple(4 * b for b in bars)),
            _harmonic_bar_similarity(chroma, tuple(3 * b for b in bars)))


def _harmonic_bar_similarity(chroma: np.ndarray, lags: tuple[int, ...]) -> float | None:
    """Mean cosine similarity of beat-synchronous chroma at the given beat lags.

    Bar-level self-similarity: chord loops repeat on 2/4/8-bar hypercycles,
    so a 4/4 track's harmony recurs at 8/16/32 beats and a 3/4 track's at
    6/12/24. Only lags the track is long enough for are used.
    """
    n = chroma.shape[1] - 1  # last sync column is the tail after the last beat
    sims = []
    for lag in lags:
        if lag < n - 2:
            a, b = chroma[:, :n - lag], chroma[:, lag:n]
            sims.append(float(np.mean(np.sum(a * b, axis=0))))
    return float(np.mean(sims)) if sims else None


def _ternary_subdivision(feats: dict[str, Any]) -> float:
    """How strongly the inter-beat onsets sit on thirds rather than halves.

    Samples the full-band flux at 1/3, 1/2 and 2/3 of every inter-beat
    interval; returns min(flux@1/3, flux@2/3) / flux@1/2 (averaged). A
    compound meter tracked at the dotted-quarter pulse (6/8 counted in 2) has
    both third-points strong and the half-point empty; a straight 4/4 is the
    reverse; a swung shuffle has only the 2/3 point, which ``min`` rejects.
    """
    flux, frames = feats["flux"], feats["frames"]
    thirds, halves = [], []

    def _at(pos: float) -> float:
        f = int(round(pos))
        return float(flux[max(0, f - 1):f + 2].max()) if 0 <= f < len(flux) else 0.0

    for a, b in zip(frames[:-1], frames[1:]):
        if b - a < 6:  # too few frames to resolve thirds vs halves
            continue
        d = float(b - a)
        thirds.append(min(_at(a + d / 3.0), _at(a + 2.0 * d / 3.0)))
        halves.append(_at(a + d / 2.0))
    if not thirds:
        return 0.0
    t, h = float(np.mean(thirds)), float(np.mean(halves))
    beat_level = float(np.mean(feats["onset"])) + 1e-9
    if t < 0.1 * beat_level:     # no real subdivision onsets at all
        return 0.0
    return t / (h + 0.05 * beat_level)


# 6/8 tracked at the eighth-note pulse: tempo gates (tracked BPM) and the
# evidence each needs. Below the lower gate the "eighths" would put the
# dotted quarter under ~43 BPM, so it is a genuine 3/4.
_COMPOUND_MIN_PULSE_BPM = 130.0
_COMPOUND_FAST_PULSE_BPM = 200.0
_COMPOUND_EVIDENCE = 0.45        # 130-200 BPM: waltz and 6/8 tempos overlap
_COMPOUND_EVIDENCE_FAST = 0.2    # >= 200 BPM: almost always an eighth pulse


def _compound_eighth_evidence(feats: dict[str, Any]) -> float | None:
    """How much the bar structure recurs every 6 tracked beats but not every 3.

    At an eighth-note pulse a 6/8 bar spans 6 beats with accents on 1 and 4,
    so the generic accent curve peaks at lag 3 exactly like a waltz. What
    differs is *which* accent recurs: the kick / bass note and the chord
    change come once per bar (every 6 eighths), while the 4th-eighth accent
    is a backbeat of a different kind. In 3/4 the bass note and the harmonic
    rhythm recur every 3 beats. Returns the weighted mean over the bass and
    chord-change descriptors of ``corr(lag 6) - corr(lag 3)`` (plus lag 12 vs
    9 when the track is long enough), or None when neither varies.
    """
    vals, weights = [], []
    for key in ("bass", "change"):
        v = np.asarray(feats[key], dtype=float)
        mean = float(np.mean(v))
        if key == "change" and float(np.max(v, initial=0.0)) < 0.05:
            continue
        z = _zscore(v)
        if z is None or mean <= 1e-9:
            continue
        w = min(1.0, float(np.std(v)) / mean)
        if w < 0.2:
            continue
        ev = _lag_corr(z, 6) - _lag_corr(z, 3)
        if len(z) > 14:
            ev = 0.5 * ev + 0.5 * (_lag_corr(z, 12) - _lag_corr(z, 9))
        vals.append(ev)
        weights.append(w)
    if not vals:
        return None
    return float(np.average(vals, weights=weights))


def estimate_meter(y: np.ndarray, sr: int, beat_times: list[float],
                   bpm: float | None = None) -> dict[str, Any]:
    """Time signature from beat-synchronous accent patterns + bar self-similarity.

    Candidates are duple (4/4, which also covers 2/4) and triple (3/4) at the
    tracked-beat level, plus 6/8 (compound duple) when the evidence says so:

    * **accent periodicity** - per-beat low-band flux (kick/bass), full-band
      flux (dynamic accent) and chroma change (harmonic rhythm) are each
      z-scored and autocorrelated over beat lags; a 4/4 accent pattern peaks
      at lags 4 and 8, a waltz at 3 and 6;
    * **bar-level self-similarity** - beat-synchronous chroma compared at the
      2/4/8-bar hypercycle lags of each candidate (8/16/32 vs 6/12/24 beats);
    * **6/8** - reported when a duple grouping wins *and* the inter-beat
      onsets fall on thirds (dotted-quarter pulse, ``beats_per_bar`` 2), or
      when a triple grouping wins at an eighth-note pulse (>= 130 BPM) whose
      kick/bass and chord changes recur every 6 beats rather than every 3
      (``beats_per_bar`` 6; see :func:`_compound_eighth_evidence`). A fast
      tracked pulse (>= 200 BPM) needs less evidence: a waltz quarter that
      fast is rare, a 6/8 eighth pulse is not.

    Falls back to 4/4 with low confidence when the track is too short or the
    accents don't separate. Returns ``{"time_signature", "beats_per_bar",
    "meter_confidence", "downbeat_phase"}``; ``beats_per_bar`` counts tracked
    beats per bar and ``downbeat_phase`` is the index of the first downbeat in
    ``beat_times`` (see :func:`estimate_downbeat_phase`).
    """
    fallback: dict[str, Any] = {"time_signature": DEFAULT_TIME_SIGNATURE, "beats_per_bar": 4,
                                "meter_confidence": 0.0, "downbeat_phase": 0}
    n_beats = len(beat_times)
    if n_beats < _MIN_BEATS_FOR_METER or len(y) < 2048:
        # Too short to judge the meter; keep the 4/4 assumption but still
        # place its downbeats (estimate_downbeat_phase has its own minimum).
        fallback["downbeat_phase"] = estimate_downbeat_phase(y, sr, beat_times, 4)
        return fallback
    try:
        feats = _beat_accent_features(y, sr, beat_times)
    except Exception:
        feats = None
    if feats is None:
        return fallback

    def _result(sig: str, bpb: int, conf: float) -> dict[str, Any]:
        phase = _phase_from_features(feats, bpb) if n_beats >= 2 * bpb else 0
        return {"time_signature": sig, "beats_per_bar": bpb,
                "meter_confidence": round(float(np.clip(conf, 0.0, 1.0)), 3),
                "downbeat_phase": int(phase)}

    r = _accent_periodicity(feats)
    harm4, harm3 = _harmonic_bar_similarities(feats["chroma"])
    harm_margin = (harm4 - harm3) if harm4 is not None and harm3 is not None else 0.0

    if r is None:
        # No accent structure at all (e.g. identical clicks): only harmony
        # can speak, and only weakly.
        if harm_margin < -_TRIPLE_MIN_MARGIN:
            return _result("3/4", 3, min(0.5, -harm_margin))
        return _result(DEFAULT_TIME_SIGNATURE, 4, _FALLBACK_METER_CONFIDENCE * 0.5)

    def _avg(*lags: int) -> float:
        vals = [r[lag] for lag in lags if lag in r and lag < n_beats - 1]
        return float(np.mean(vals)) if vals else 0.0

    duple = _avg(4, 8)
    triple = _avg(3, 6)
    margin = (duple - triple) + 0.5 * harm_margin   # > 0 favours 4/4

    if margin <= -_TRIPLE_MIN_MARGIN:
        conf = min(1.0, -margin)
        tempo = float(bpm) if bpm else 60.0 / max(float(np.median(np.diff(beat_times))), 1e-6)
        if tempo >= _COMPOUND_MIN_PULSE_BPM:
            evidence = _compound_eighth_evidence(feats)
            needed = _COMPOUND_EVIDENCE_FAST if tempo >= _COMPOUND_FAST_PULSE_BPM else _COMPOUND_EVIDENCE
            # The full-band accent alone separating lag 6 from lag 3 (the
            # bar's second dotted quarter weaker than its first) also counts.
            accent_says_six = r.get(6, 0.0) - r.get(3, 0.0) > 0.3
            if accent_says_six or (evidence is not None and evidence > needed):
                return _result("6/8", 6, conf * 0.8)
        return _result("3/4", 3, conf)

    if margin < _TRIPLE_MIN_MARGIN:
        # Unclear: keep the 4/4 assumption, but say it is a guess.
        return _result(DEFAULT_TIME_SIGNATURE, 4, _FALLBACK_METER_CONFIDENCE)

    conf = min(1.0, margin)
    if _ternary_subdivision(feats) > 2.0:
        # Duple grouping of a dotted-quarter pulse whose beats split in three:
        # compound duple. Bars span 2 tracked beats.
        return _result("6/8", 2, conf * 0.8)
    return _result(DEFAULT_TIME_SIGNATURE, 4, conf)


# librosa's HPSS default median-filter width (frames along time, bins along
# frequency).
_HPSS_KERNEL = 31


def _odd_at_most(limit: int, want: int) -> int:
    k = max(1, min(want, limit))
    return k if k % 2 else max(1, k - 1)


def hpss(S, *, kernel_size: int = _HPSS_KERNEL, **kwargs):
    """``librosa.decompose.hpss`` with each median kernel clamped to the
    spectrogram's extent along its axis.

    scipy's ``median_filter`` (mode="reflect", which librosa uses) returns
    NaN for some inputs when the kernel is longer than the axis it slides
    along - a clip under ~0.7 s has fewer than 31 STFT frames, and its
    harmonic/percussive split came back partly or wholly NaN, data-
    dependently (bad-input sweep 2026-10-16: /analyze/drums 500'd on a 50 ms
    file, sound_recreation and stem_taxonomy reported NaN). Spectrograms at
    least 31 frames long - every normal track - get librosa's defaults
    unchanged.
    """
    import librosa
    n_bins, n_frames = S.shape[-2], S.shape[-1]
    kernel = (_odd_at_most(n_frames, kernel_size), _odd_at_most(n_bins, kernel_size))
    return librosa.decompose.hpss(S, kernel_size=kernel, **kwargs)


def hpss_signal(y, *, n_fft: int = 2048, hop_length: int = 512, **kwargs):
    """Time-domain ``librosa.effects.hpss`` built on :func:`hpss` (same STFT
    defaults), returning ``(harmonic, percussive)`` signals of ``len(y)``."""
    import librosa
    D = librosa.stft(y, n_fft=n_fft, hop_length=hop_length)
    H, P = hpss(D, **kwargs)
    istft = dict(n_fft=n_fft, hop_length=hop_length, length=y.shape[-1])
    return librosa.istft(H, **istft).astype(y.dtype), librosa.istft(P, **istft).astype(y.dtype)
