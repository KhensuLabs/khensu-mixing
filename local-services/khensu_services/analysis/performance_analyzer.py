"""Timing, stem-phase, reverb-tail, and harmony-timeline measurements.

All outputs are descriptive estimates from the supplied audio, not musical
ground truth.  They deliberately use the same local librosa/scipy stack as the
rest of the service and are designed to run in a bounded worker thread.
"""
from __future__ import annotations

from khensu_services.analysis.context import load_audio
from itertools import combinations
from typing import Any

import numpy as np
from khensu_services.analysis import _lean_spectral as _lean
from khensu_services.analysis.dsp_metrics import onset_strength


def _load_mono(path: str) -> tuple[np.ndarray, int]:

    y, sr = load_audio(path, sr=None, mono=True)
    return np.asarray(y, dtype=np.float64), int(sr)


def _pearson(a: np.ndarray, b: np.ndarray) -> float:
    """The shared mean-removed Pearson (dsp_metrics.pearson, float64);
    0.0 when either side has no variance."""
    from khensu_services.analysis.dsp_metrics import pearson

    return pearson(a, b, silent=0.0)


_GROOVE_HOP = 128          # ~6 ms at 22.05 kHz: finer than the timing spread we report


def _subdivision_grid(beats: np.ndarray, subdivisions: int, t_min: float, t_max: float) -> np.ndarray:
    """Grid lines from subdividing each tracked beat interval (so the grid
    follows tempo drift), extrapolated by the edge intervals to cover
    [t_min, t_max]."""
    first_period = float(beats[1] - beats[0])
    last_period = float(beats[-1] - beats[-2])
    pieces: list[np.ndarray] = []
    n_before = int(np.ceil(max(0.0, float(beats[0]) - t_min) / first_period)) + 1
    pieces.append(beats[0] - (np.arange(n_before * subdivisions, 0, -1) / subdivisions) * first_period)
    for left, right in zip(beats[:-1], beats[1:]):
        pieces.append(left + (right - left) * np.arange(subdivisions) / subdivisions)
    n_after = int(np.ceil(max(0.0, t_max - float(beats[-1])) / last_period)) + 1
    pieces.append(beats[-1] + (np.arange(n_after * subdivisions + 1) / subdivisions) * last_period)
    return np.unique(np.concatenate(pieces))


def _choose_subdivision(phases: np.ndarray, swing_ratio: float) -> tuple[int, str]:
    """Pick the grid the onsets actually sit on from the within-beat phase
    histogram: triplets when the swing ratio indicates ~2:1, else 16ths when
    a meaningful share of onsets sits on the off-8th positions, else 8ths."""
    if 1.6 <= swing_ratio <= 2.6:
        return 3, "triplet"
    if phases.size:
        off_eighth = np.minimum(np.abs(phases - 0.25), np.abs(phases - 0.75)) < 0.08
        if float(np.mean(off_eighth)) >= 0.15:
            return 4, "16th"
    return 2, "8th"


def analyze_groove_timing(file_path: str) -> dict[str, Any]:
    """Estimate grid placement, timing spread, and binary swing ratio.

    Offsets are measured to the nearest *subdivision* of the tracked beat
    grid (8th / 16th / triplet, chosen from the onset-phase histogram), not
    to the nearest beat: perfectly quantised 16ths used to read as a large
    "timing spread" because every off-beat 16th was compared with a
    quarter-note line.
    """
    import librosa

    y, sr = _load_mono(file_path)
    if len(y) < sr // 2:
        return _empty_groove()
    hop = _GROOVE_HOP
    onset_env = onset_strength(y, sr, hop_length=hop)
    tempo, beat_frames = _lean.beat_track(onset_envelope=onset_env, sr=sr, hop_length=hop)
    beats = librosa.frames_to_time(beat_frames, sr=sr, hop_length=hop)
    # Beat-fit tempo, not the quantised tempogram bin (AUDIT-015).
    from khensu_services.analysis._shared_analysis import tracked_tempo
    bpm = tracked_tempo(tempo, beats)
    onsets = librosa.onset.onset_detect(onset_envelope=onset_env, sr=sr, hop_length=hop, units="time")
    if len(beats) < 3 or len(onsets) == 0 or bpm <= 0.0:
        out = _empty_groove()
        out["bpm"] = round(bpm, 2)
        return out

    period = float(np.median(np.diff(beats)))
    # Within-beat phase (0..1) of every onset against its local beat interval.
    idx = np.clip(np.searchsorted(beats, onsets, side="right") - 1, 0, len(beats) - 2)
    local = beats[idx + 1] - beats[idx]
    phases = np.mod((onsets - beats[idx]) / np.maximum(local, 1e-9), 1.0)
    swing_intervals = [float(p) / max(1.0 - float(p), 1e-9) for p in phases if 0.2 <= p <= 0.8]
    swing_ratio = float(np.median(swing_intervals)) if swing_intervals else 1.0

    subdivisions, grid_label = _choose_subdivision(phases, swing_ratio)
    grid = _subdivision_grid(beats, subdivisions, float(onsets[0]), float(onsets[-1]))
    half_cell = period / subdivisions * 0.5
    offset_events: list[tuple[float, float]] = []
    for onset in onsets:
        nearest = grid[int(np.argmin(np.abs(grid - onset)))]
        offset = float(onset - nearest)
        if abs(offset) <= half_cell:
            offset_events.append((float(onset), offset))
    offset_ms = np.asarray([offset for _, offset in offset_events], dtype=float) * 1000.0
    median_ms = float(np.median(offset_ms)) if offset_ms.size else 0.0
    spread_ms = float(np.std(offset_ms)) if offset_ms.size else 0.0
    if median_ms < -12.0:
        pocket = "ahead_of_grid"
    elif median_ms > 12.0:
        pocket = "behind_grid"
    else:
        pocket = "on_grid"
    events = [
        {"time_sec": round(float(t), 3), "offset_ms": round(float(o) * 1000.0, 1)}
        for t, o in offset_events[:160]
    ]
    return {
        "bpm": round(bpm, 2), "grid_offset_ms": round(median_ms, 1),
        "timing_std_ms": round(spread_ms, 1), "ahead_fraction": round(float(np.mean(offset_ms < -12.0)) if offset_ms.size else 0.0, 3),
        "behind_fraction": round(float(np.mean(offset_ms > 12.0)) if offset_ms.size else 0.0, 3),
        "swing_ratio": round(swing_ratio, 3), "pocket": pocket, "events": events,
        "grid_subdivision": grid_label,
    }


def _empty_groove() -> dict[str, Any]:
    return {"bpm": 0.0, "grid_offset_ms": 0.0, "timing_std_ms": 0.0,
            "ahead_fraction": 0.0, "behind_fraction": 0.0, "swing_ratio": 1.0,
            "pocket": "insufficient_onsets", "events": [], "grid_subdivision": "none"}


def analyze_stem_phase_correlation(stem_paths: dict[str, str]) -> dict[str, Any]:
    """Pairwise broadband correlation and polarity classification for stems.

    Memory (audit M5): every stem is decoded as float32 and resampled ONCE
    to the lowest sample rate among them - the old loop held float64 copies
    and re-resampled inside every pair, O(n^2) resamples for n stems."""
    import librosa

    decoded: dict[str, tuple[np.ndarray, int]] = {}
    for name, path in stem_paths.items():
        y, sr = load_audio(path, sr=None, mono=True)
        decoded[name] = (np.asarray(y, dtype=np.float32), int(sr))
    common_sr = min((sr for _y, sr in decoded.values()), default=0)
    loaded: dict[str, np.ndarray] = {}
    for name, (y, sr) in decoded.items():
        if sr != common_sr:
            y = librosa.resample(y, orig_sr=sr, target_sr=common_sr).astype(np.float32, copy=False)
        loaded[name] = y
    decoded.clear()
    pairs: list[dict[str, Any]] = []
    sr = common_sr
    for (left_name, left), (right_name, right) in combinations(loaded.items(), 2):
        n = min(len(left), len(right))
        corr = _pearson(left[:n], right[:n]) if n >= sr // 4 else 0.0
        polarity = "inverted" if corr <= -0.65 else "aligned" if corr >= 0.65 else "decorrelated"
        pairs.append({"left": left_name, "right": right_name,
                      "correlation": round(corr, 3), "polarity": polarity})
    return {"pairs": pairs, "stem_count": len(loaded)}


# Free-decay RT60 (audit M1). A single Schroeder integral over the whole
# song measured the song's loudness envelope, so the "RT60" grew with the
# file's length. Instead, decay is measured only where the signal is
# actually decaying freely: after an onset whose envelope then falls
# (mostly monotonically) by at least _DECAY_MIN_DROP_DB before the next
# onset, which must be at least _DECAY_MIN_GAP_SEC away.
_DECAY_FRAME = 1024
_DECAY_HOP = 256
_DECAY_MIN_GAP_SEC = 0.35
_DECAY_MIN_DROP_DB = 20.0
_DECAY_MONOTONIC_FRACTION = 0.8
_DECAY_MIN_R2 = 0.85
_DECAY_MAX_SEGMENTS = 24


def _envelope_rt60(decay_db: np.ndarray, frame_rate: float) -> tuple[float | None, float]:
    """(RT60, r2) from a linear fit of one free-decay segment's level
    envelope (dB, starting at its peak), over -5 dB down to -35 dB (or 2 dB
    short of the segment's total drop when it has less range), extrapolated
    to 60 dB. The envelope slope is used rather than a per-segment Schroeder
    integral: a segment ends at the next onset, and integrating a truncated
    decay bends the Schroeder curve down and under-reads the RT60."""
    rel = np.asarray(decay_db, dtype=float) - float(decay_db[0])
    floor = max(-35.0, float(np.min(rel)) + 2.0)
    if floor > -15.0:
        return None, 0.0
    below = np.nonzero(rel <= floor)[0]
    end = int(below[0]) if below.size else len(rel)
    idx = np.arange(end)
    mask = rel[:end] <= -5.0
    if np.count_nonzero(mask) < 6:
        return None, 0.0
    t = idx[mask] / frame_rate
    d = rel[:end][mask]
    slope, intercept = np.polyfit(t, d, 1)
    residual = float(np.sum((d - (slope * t + intercept)) ** 2))
    total = float(np.sum((d - np.mean(d)) ** 2))
    r2 = max(0.0, 1.0 - residual / max(total, 1e-12))
    return (float(-60.0 / slope) if slope < -1e-9 else None), r2


def _free_decay_segments(y: np.ndarray, sr: int) -> list[np.ndarray]:
    """Level envelopes (dB per _DECAY_HOP frame, from the peak) of the
    free-decay segments (see block comment)."""
    import librosa

    rms = _lean.rms(y, frame_length=_DECAY_FRAME, hop_length=_DECAY_HOP)[0]
    if rms.size < 4:
        return []
    env_db = 20.0 * np.log10(np.maximum(rms, 1e-9))
    onsets = librosa.onset.onset_detect(onset_envelope=onset_strength(y, sr, hop_length=_DECAY_HOP),
                                        sr=sr, hop_length=_DECAY_HOP, units="frames", backtrack=False)
    starts = sorted(set(int(o) for o in np.atleast_1d(onsets)))
    bounds = starts + [len(env_db)]
    min_gap = int(np.ceil(_DECAY_MIN_GAP_SEC * sr / _DECAY_HOP))
    peak_win = max(1, int(round(0.1 * sr / _DECAY_HOP)))
    candidates: list[tuple[int, int, int]] = []
    for i, start in enumerate(starts):
        end = bounds[i + 1]
        if end - start < min_gap:
            continue
        peak = start + int(np.argmax(env_db[start:min(end, start + peak_win)]))
        tail = env_db[peak:end]
        if tail.size < 4 or float(tail[0] - np.min(tail)) < _DECAY_MIN_DROP_DB:
            continue
        # Cut the segment where the decay bottoms out, then require it to be
        # (mostly) monotonically decaying - a sustained note or a crescendo
        # isn't a free decay.
        stop = int(np.argmin(tail)) + 1
        steps = np.diff(tail[:stop])
        if steps.size < 3 or float(np.mean(steps <= 0.5)) < _DECAY_MONOTONIC_FRACTION:
            continue
        candidates.append((end - start, peak, peak + stop))
    # Longest gaps first: the cleanest decays, and a bounded amount of work.
    candidates.sort(reverse=True)
    return [env_db[p:e] for _gap, p, e in candidates[:_DECAY_MAX_SEGMENTS]]


def estimate_reverb_tail(file_path: str) -> dict[str, Any]:
    """Free-decay RT60 estimate plus onset-relative tail energy ratio.

    ``rt60_sec`` is the median envelope-slope RT60 over the detected free-decay
    segments (stops, endings, isolated hits), ``decay_segments`` how many
    contributed; ``None`` with an explanatory ``rt60_note`` when the audio
    has no usable decay (e.g. dense material with no gaps)."""

    y, sr = _load_mono(file_path)
    if y.size < sr // 2:
        return {"rt60_sec": None, "fit_r2": 0.0, "tail_energy_ratio": 0.0,
                "source_reverb_tail": "insufficient_audio", "decay_segments": 0,
                "rt60_note": "Too little audio to measure a decay."}
    fits = [_envelope_rt60(env, sr / _DECAY_HOP) for env in _free_decay_segments(y, sr)]
    good = [(rt, r2) for rt, r2 in fits if rt is not None and r2 >= _DECAY_MIN_R2
            and 0.05 <= rt <= 12.0]
    if good:
        rt60: float | None = float(np.median([rt for rt, _ in good]))
        r2 = float(np.median([fit for _, fit in good]))
        note = f"Median of {len(good)} free-decay segment(s)."
    else:
        rt60, r2 = None, 0.0
        note = ("No usable free decay found: RT60 needs a stop, ending or isolated hit "
                f"that decays by {int(_DECAY_MIN_DROP_DB)} dB before the next onset.")
    onset_env = onset_strength(y, sr)
    active = onset_env > np.percentile(onset_env, 70.0) if onset_env.size else np.array([], dtype=bool)
    tail_ratio = float(np.mean(onset_env[~active]) / max(np.mean(onset_env[active]), 1e-12)) if np.any(active) else 0.0
    label = "pronounced" if tail_ratio > 0.35 else "moderate" if tail_ratio > 0.15 else "dry_or_short"
    return {"rt60_sec": round(rt60, 3) if rt60 is not None else None, "fit_r2": round(r2, 3),
            "tail_energy_ratio": round(tail_ratio, 3), "source_reverb_tail": label,
            "decay_segments": len(good), "rt60_note": note}


# Key tracking (audit #2): Krumhansl-Schmuckler on sliding windows; a key
# change is reported only after several consecutive windows agree on the new
# key AND prefer it over the current key by a correlation margin. The old
# per-second triad-template "key" flipped on every chord of a I-IV-V loop.
_KEY_WINDOW_SEC = 12.0
_KEY_HOP_SEC = 4.0
_KEY_CHANGE_WINDOWS = 3
_KEY_CHANGE_MARGIN = 0.05


def _windowed_keys(chroma: np.ndarray, frames_per_sec: float) -> list[tuple[float, np.ndarray]]:
    """(window start sec, KS correlation table (2, 12)) per non-silent window."""
    from khensu_services.analysis._key import key_correlations

    n = chroma.shape[1]
    win = max(1, int(round(_KEY_WINDOW_SEC * frames_per_sec)))
    hop = max(1, int(round(_KEY_HOP_SEC * frames_per_sec)))
    starts = list(range(0, max(1, n - win + 1), hop))
    if starts[-1] + win < n:
        starts.append(max(0, n - win))
    out: list[tuple[float, np.ndarray]] = []
    for start in starts:
        scores = key_correlations(chroma[:, start:start + win].mean(axis=1))
        if scores is not None:
            out.append((start / frames_per_sec, scores))
    return out


def _key_entry(time_sec: float, key: tuple[int, int]) -> dict[str, Any]:
    from khensu_services.analysis._note_names import pc_name

    mode_idx, tonic = key
    mode = "major" if mode_idx == 0 else "minor"
    return {"time_sec": round(float(time_sec), 3), "key": pc_name(tonic, tonic, mode), "mode": mode}


def _track_keys(windows: list[tuple[float, np.ndarray]]
                ) -> tuple[dict[str, Any] | None, list[dict[str, Any]], list[tuple[float, int, str]]]:
    """(initial key entry, key-change entries, [(time, tonic_pc, mode)] spans)."""

    def _argmax(scores: np.ndarray) -> tuple[int, int]:
        mode_idx, tonic = np.unravel_index(int(np.argmax(scores)), scores.shape)
        return int(mode_idx), int(tonic)

    if not windows:
        return None, [], []
    current = _argmax(windows[0][1])
    initial = _key_entry(0.0, current)
    spans = [(0.0, current[1], "major" if current[0] == 0 else "minor")]
    changes: list[dict[str, Any]] = []
    candidate: tuple[int, int] | None = None
    run_start, run_len = 0.0, 0
    for start_sec, scores in windows[1:]:
        best = _argmax(scores)
        lead = float(scores[best] - scores[current])
        if best != current and lead >= _KEY_CHANGE_MARGIN:
            if best == candidate:
                run_len += 1
            else:
                candidate, run_start, run_len = best, start_sec, 1
            if run_len >= _KEY_CHANGE_WINDOWS:
                # The new key first dominates the earliest agreeing window, whose
                # newest audio is its trailing hop; date the change there.
                change_at = run_start + max(0.0, _KEY_WINDOW_SEC - _KEY_HOP_SEC)
                changes.append(_key_entry(change_at, best))
                spans.append((round(change_at, 3), best[1], "major" if best[0] == 0 else "minor"))
                current, candidate, run_len = best, None, 0
        else:
            candidate, run_len = None, 0
    return initial, changes, spans


def analyze_harmony_timeline(file_path: str, segment_sec: float = 1.0) -> dict[str, Any]:
    """Track chord progression and sustained key changes from chroma.

    The chords come from :func:`detect_chord_sequence` - the same templates
    (7ths, diminished, augmented, sus), "N" class and Viterbi smoothing as
    /analyze/chords - so both routes agree on the same audio (THY-13). The
    old triad-only matcher had no "no chord" reading, so noise always
    produced a chord. "N" segments are left out of the progression (and break
    a run, so a chord that returns after silence is listed again); each chord
    root is respelled for the key in force at its time."""
    from khensu_services.analysis import _harmony
    from khensu_services.analysis._key import safe_chroma_cqt
    from khensu_services.analysis._note_names import pc_name, spell_degree

    y, sr = _load_mono(file_path)
    hop = 512
    chroma = safe_chroma_cqt(y, sr, hop_length=hop)
    initial_key, changes, spans = _track_keys(_windowed_keys(chroma, sr / hop))

    # Two beats per segment where the beats can be tracked (NEW-8).
    from khensu_services.analysis import _lean_spectral as _lean
    import librosa
    try:
        _tempo, beat_frames = _lean.beat_track(y=y, sr=sr, hop_length=hop)
        beat_times = librosa.frames_to_time(beat_frames, sr=sr, hop_length=hop).tolist()
    except Exception:
        beat_times = []
    starts = beat_segment_frames(beat_times, sr, hop, chroma.shape[1], beats_per_segment=2)
    if starts is None:
        starts = beat_segment_frames(beat_times, sr, hop, chroma.shape[1], beats_per_segment=1)
    sequence = detect_chord_sequence(chroma, sr, hop, segment_sec=segment_sec, segment_starts=starts)
    progression: list[dict[str, Any]] = []
    previous: str | None = None
    for event in sequence["chords"]:
        parsed = _harmony.parse_chord(event["chord"])
        if parsed is None:
            previous = None
            continue
        root, quality = parsed
        time_sec = round(float(event["time_sec"]), 3)
        # Spell the chord root from its scale degree in the key in force at
        # this time (Bb, not A#, in F; bVII in C is Bb; #viio in G minor F#).
        tonic, mode = None, "major"
        for span_start, span_tonic, span_mode in spans:
            if span_start <= time_sec:
                tonic, mode = span_tonic, span_mode
        root_name = spell_degree(root, tonic, mode) if tonic is not None else pc_name(root)
        chord = root_name + quality
        if chord != previous:
            progression.append({"time_sec": time_sec, "chord": chord,
                                "confidence": round(float(event["confidence"]), 3)})
        previous = chord
    return {"progression": progression, "key_changes": changes, "initial_key": initial_key}


# --- Chord labelling for /analyze/chords (audit #5) ---------------------------

# Quality suffix -> intervals above the root. Order matters only for exact
# ties; 4-note templates get a small complexity prior so a plain triad's
# harmonics (e.g. the 7th partial) don't promote it to a seventh chord.
_CHORD_QUALITIES: tuple[tuple[str, tuple[int, ...]], ...] = (
    ("", (0, 4, 7)), ("m", (0, 3, 7)), ("7", (0, 4, 7, 10)), ("maj7", (0, 4, 7, 11)),
    ("m7", (0, 3, 7, 10)), ("dim", (0, 3, 6)), ("sus2", (0, 2, 7)), ("sus4", (0, 5, 7)),
    # The rest of the qualities _harmony can name: the fully and the half
    # diminished seventh (vii-o7 / ii-m7b5 in minor) and the augmented triad.
    ("dim7", (0, 3, 6, 9)), ("m7b5", (0, 3, 6, 10)), ("aug", (0, 4, 8)),
)
_ROOT_WEIGHT = 1.25          # chroma usually carries the root strongest (bass + harmonics)
_FOUR_NOTE_PRIOR = 0.97
NO_CHORD = "N"
_NO_CHORD_MIN_COSINE = 0.75
_NO_CHORD_MIN_MARGIN = 0.05
_CHORD_SWITCH_PENALTY = 0.05


def _full_chord_templates() -> tuple[list[tuple[int, str]], np.ndarray, list[frozenset[int]], np.ndarray]:
    labels: list[tuple[int, str]] = []
    rows: list[np.ndarray] = []
    sets: list[frozenset[int]] = []
    priors: list[float] = []
    for root in range(12):
        for quality, intervals in _CHORD_QUALITIES:
            vec = np.zeros(12)
            for iv in intervals:
                vec[(root + iv) % 12] = 1.0
            vec[root] = _ROOT_WEIGHT
            rows.append(vec / np.linalg.norm(vec))
            labels.append((root, quality))
            sets.append(frozenset((root + iv) % 12 for iv in intervals))
            priors.append(_FOUR_NOTE_PRIOR if len(intervals) == 4 else 1.0)
    return labels, np.stack(rows), sets, np.asarray(priors)


# Symmetric chords: the same pitch set on several roots (dim7 every minor
# third, aug every major third). Chroma can't tell those roots apart, so a
# tie is resolved musically instead of by template order.
_SYMMETRIC_STEP = {"dim7": 3, "aug": 4}
_SYMMETRIC_TIE = 0.02


def _resolve_symmetric_roots(path: list[int], emissions: list[np.ndarray],
                             labels: list[tuple[int, str]], tonic: int | None,
                             mode: str) -> list[int]:
    """Pick the root of each dim7 / aug segment among its equally fitting
    enharmonic roots: a dim7 is the leading-tone chord of the next chord's
    root (Bdim7 before C), else of the tonic (or a diatonic root); an aug
    prefers the tonic, the minor key's III+ and the dominant, then any
    diatonic root. Ties keep the Viterbi choice."""
    if not path:
        return path
    index = {lab: i for i, lab in enumerate(labels)}
    n_labels = len(labels)
    scale = ({(tonic + d) % 12 for d in ((0, 2, 4, 5, 7, 9, 11) if mode != "minor"
                                         else (0, 2, 3, 5, 7, 8, 11))}
             if tonic is not None else set())
    out = list(path)
    for i, state in enumerate(out):
        if state >= n_labels:
            continue
        root, quality = labels[state]
        step = _SYMMETRIC_STEP.get(quality)
        if step is None:
            continue
        em = emissions[i]
        cands = [(root + k * step) % 12 for k in range(12 // step)]
        cands = [r for r in cands if em[index[(r, quality)]] >= em[state] - _SYMMETRIC_TIE]
        if len(cands) <= 1:
            continue
        nxt = next((labels[out[j]][0] for j in range(i + 1, len(out))
                    if out[j] < n_labels and labels[out[j]] != (root, quality)
                    and labels[out[j]][1] != quality), None)
        prefs: list[int] = []
        if quality == "dim7":
            if nxt is not None:
                prefs.append((nxt - 1) % 12)
            if tonic is not None:
                prefs.append((tonic - 1) % 12)
        elif tonic is not None:
            prefs += [tonic % 12, (tonic + 3) % 12 if mode == "minor" else tonic % 12,
                      (tonic + 7) % 12]
        prefs += sorted(r for r in cands if r in scale)
        best = next((r for r in prefs if r in cands), None)
        if best is not None:
            out[i] = index[(best, quality)]
    return out


def beat_segment_frames(beat_times: list[float] | None, sr: int, hop_length: int, num_frames: int,
                        beats_per_segment: int = 1) -> list[int] | None:
    """Chroma frame indices where beat-synchronous segments start (NEW-8):
    0, then every ``beats_per_segment``-th tracked beat. None when there are
    too few beats to follow (the caller keeps fixed-length segments)."""
    if not beat_times or len(beat_times) < 8:
        return None
    ibi = np.diff(np.asarray(beat_times, dtype=float))
    med = float(np.median(ibi))
    # Only a steady, plausible pulse: an irregular track (rubato, a pad with
    # no attacks) or segments longer than 1.2 s would smear chords together.
    if med <= 0 or float(np.std(ibi)) / med > 0.15 or med * max(1, beats_per_segment) > 1.2:
        return None
    frames = sorted({0} | {int(round(t * sr / hop_length)) for t in beat_times[::max(1, beats_per_segment)]})
    frames = [f for f in frames if 0 <= f < num_frames]
    return frames if len(frames) >= 4 else None


def detect_chord_sequence(chroma: np.ndarray, sr: int, hop_length: int,
                          segment_sec: float = 0.5, segment_starts: list[int] | None = None,
                          key_profile: str = "krumhansl") -> dict[str, Any]:
    """Chord label per segment plus a key from the chosen key profile.

    Segments are ``segment_sec`` long, or - with ``segment_starts`` (chroma
    frame indices, see :func:`beat_segment_frames`) - one per beat, so a chord
    change on the beat is not smeared across two fixed windows (NEW-8).

    * templates: major, minor, 7, maj7, m7, dim, sus2, sus4, dim7, m7b5, aug
      (cosine vs. the segment's unit chroma, root-weighted);
    * "N" (no chord) when the best cosine is < 0.75, or when the runner-up
      that is a genuinely different reading (not a subset/superset of the
      winner's pitch set, e.g. C vs Cmaj7) is within 0.05;
    * Viterbi smoothing over labels with a constant switch penalty, so a
      single noisy segment can't flip the progression;
    * labels spelled for the estimated key (Bb / Eb / F in Bb major);
    * a Roman numeral ("roman"), diatonic flag and harmonic function
      ("function": diatonic / secondary / borrowed / chromatic) per chord
      relative to that key, plus the key's Camelot code, circle-of-fifths
      index and relative / parallel keys (see _harmony).

    Returns {"chords": [{"time_sec", "chord", "confidence", "roman",
    "diatonic", "function"}], "key", "mode", "key_confidence", "camelot",
    "circle_of_fifths_index", "relative_key", "parallel_key"}; key/mode are
    "unknown"/"" and the key-derived fields None for silent input.
    """
    from khensu_services.analysis import _harmony
    from khensu_services.analysis._key import estimate_key_details
    from khensu_services.analysis._note_names import pc_name, spell_degree

    labels, templates, sets, priors = _full_chord_templates()
    n_labels = len(labels)
    frames_per_seg = max(1, int(segment_sec * sr / hop_length))
    num_frames = chroma.shape[1]
    if segment_starts:
        starts = [f for f in segment_starts if 0 <= f < num_frames]
    else:
        starts = list(range(0, num_frames, frames_per_seg))
    bounds = starts[1:] + [num_frames]
    times: list[float] = []
    emissions: list[np.ndarray] = []
    for f0, f1 in zip(starts, bounds):
        seg = chroma[:, f0:max(f0 + 1, f1)].mean(axis=1)
        times.append(float(f0 * hop_length / sr))
        em = np.zeros(n_labels + 1)
        norm = float(np.linalg.norm(seg))
        if norm < 1e-6:
            em[n_labels] = 1.0                       # silence: certainly no chord
            emissions.append(em)
            continue
        sims = (templates @ (seg / norm)) * priors
        best = int(np.argmax(sims))
        rivals = [sims[i] for i in range(n_labels)
                  if not (sets[i] <= sets[best] or sets[i] >= sets[best])]
        margin = float(sims[best] - max(rivals)) if rivals else 1.0
        em[:n_labels] = sims
        em[n_labels] = (_NO_CHORD_MIN_COSINE if margin >= _NO_CHORD_MIN_MARGIN
                        else float(sims[best]) + 0.01)
        emissions.append(em)

    path: list[int] = []
    if emissions:
        E = np.stack(emissions)
        score = E[0].copy()
        back = np.zeros(E.shape, dtype=int)
        for t in range(1, len(E)):
            best_prev = int(np.argmax(score))
            switch = score[best_prev] - _CHORD_SWITCH_PENALTY
            stay_better = score >= switch
            back[t] = np.where(stay_better, np.arange(E.shape[1]), best_prev)
            score = np.maximum(score, switch) + E[t]
        state = int(np.argmax(score))
        path = [state]
        for t in range(len(E) - 1, 0, -1):
            state = int(back[t][state])
            path.append(state)
        path.reverse()

    key_info = estimate_key_details(chroma.mean(axis=1) if num_frames else np.zeros(12), key_profile)
    tonic, mode = key_info["tonic_pc"], key_info["mode"] or "major"
    path = _resolve_symmetric_roots(path, emissions, labels, tonic, mode)
    chords: list[dict[str, Any]] = []
    for t, state, em in zip(times, path, emissions):
        if state == n_labels:
            chords.append({"time_sec": t, "chord": NO_CHORD, "confidence": 0.0,
                           "roman": None, "diatonic": None, "function": None})
        else:
            root, quality = labels[state]
            # Root spelled from its scale degree, so it always agrees with
            # the Roman numeral (bVII in C -> "Bb", #viio in G minor -> "F#").
            root_name = spell_degree(root, tonic, mode) if tonic is not None else pc_name(root)
            label = root_name + quality
            chords.append({
                "time_sec": t, "chord": label,
                "confidence": float(np.clip(em[state], 0.0, 1.0)),
                "roman": _harmony.roman_numeral(label, tonic, mode) if tonic is not None else None,
                "diatonic": _harmony.is_diatonic(label, tonic, mode) if tonic is not None else None,
                "function": _harmony.chord_function(label, tonic, mode) if tonic is not None else None,
            })
    relations: dict[str, Any] = {"camelot": None, "circle_of_fifths_index": None,
                                 "relative_key": None, "parallel_key": None}
    if tonic is None:
        key, out_mode = "unknown", ""
    else:
        key, out_mode = pc_name(tonic, tonic, mode), mode
        relations = _harmony.key_relations(tonic, mode)
    return {"chords": chords, "key": key, "mode": out_mode,
            "key_confidence": float(key_info["confidence"]), **relations}
