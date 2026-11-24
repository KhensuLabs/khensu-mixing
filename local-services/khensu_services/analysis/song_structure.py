"""Song structure: section boundaries, repetition groups and roles (NEW-1).

The sections used to be fixed 8-bar blocks labelled ``section_N`` (MEAS-6),
so nothing downstream that says "chorus" - the loudness arc, chorus contrast,
the hook and structure findings, section mastering, the language commands -
had a real chorus to read. This finds the structure from the audio, with no
new dependency:

1. **Beats and bars.** The shared beat tracker and meter estimate give bar
   lines; every boundary lands on one.
2. **Self-similarity.** Bar-synchronous chroma (harmony) and MFCC (timbre),
   z-scored, give a bar-by-bar recurrence matrix.
3. **Boundaries.** A Foote checkerboard kernel slid along the diagonal of the
   timbre+harmony similarity gives a novelty curve; its peaks, at least
   ``_MIN_SECTION_BARS`` apart, are the boundaries.
4. **Repetition groups.** Segments are compared by their mean bar features
   (harmony weighted over timbre, so a chorus repeated with new drums still
   matches); segments closer than a threshold share a group letter (A, B, ...).
5. **Roles.** Among groups heard at least twice, the loudest is the chorus and
   the most repeated other the verse; an unrepeated quieter first / last
   segment is the intro / outro; an unrepeated segment between two choruses
   is the bridge; a short segment right before a chorus that is louder than
   the verse is the pre-chorus. Anything else is "section".

Each section carries ``label`` ("Chorus 2", for display and for the label
matches the app already makes), ``role``, ``group`` and ``boundary_strength``
(the novelty peak at its start, 0..1), and the result says how it was found
(``method``). A learned structure model can replace steps 3-5 later behind
the same output.
"""
from __future__ import annotations

from typing import Any

import numpy as np
import numpy.typing as npt

_MIN_SECTION_BARS = 4
_KERNEL_BARS = 4             # half-width of the checkerboard kernel, in bars
_GROUP_DISTANCE = 0.35       # cosine distance under which two segments repeat
_HOP = 1024
ROLES = ("intro", "verse", "pre-chorus", "chorus", "bridge", "outro", "section")


def _bar_lines(mono: npt.NDArray, sr: int, beats: list[float] | None = None) -> tuple[list[float], int]:
    """Bar start times (seconds) and beats per bar."""
    import librosa

    from khensu_services.analysis import _lean_spectral as _lean
    from khensu_services.analysis._shared_analysis import estimate_meter, tracked_tempo

    if beats is None:
        # NEW-6: the learned tracker's downbeats are the bar lines when its
        # model is present - no meter heuristic or downbeat-phase guess.
        from khensu_services.analysis import learned_beats
        if learned_beats.model_available():
            try:
                found = learned_beats.track(mono, sr)
                if len(found["downbeats"]) >= 4:
                    return list(found["downbeats"]), int(found["beats_per_bar"])
            except Exception:
                pass
        tempo, beat_frames = _lean.beat_track(y=mono, sr=sr)
        beats = librosa.frames_to_time(beat_frames, sr=sr).tolist()
    else:
        tempo = None
    if len(beats) < 8:
        return [], 4
    meter = estimate_meter(mono, sr, beats, tracked_tempo(tempo, beats) if tempo is not None else None)
    per_bar = max(1, int(meter["beats_per_bar"]))
    phase = int(meter["downbeat_phase"]) % per_bar
    bars = [float(beats[i]) for i in range(phase, len(beats), per_bar)]
    if len(bars) >= 2:
        # Quiet intros and outros often have no tracked beats: carry the bar
        # grid back to the start and on to the end at the median bar length,
        # so those stretches are segmented too instead of folding into the
        # first / last section.
        bar_len = float(np.median(np.diff(bars)))
        total = len(mono) / sr
        if bar_len > 0:
            while bars[0] - bar_len > -0.25 * bar_len:
                bars.insert(0, max(0.0, bars[0] - bar_len))
            while bars[-1] + 2 * bar_len < total:
                bars.append(bars[-1] + bar_len)
    return bars, per_bar


def _bar_features(mono: npt.NDArray, sr: int, bars: list[float]) -> tuple[npt.NDArray, npt.NDArray, npt.NDArray]:
    """(harmony, timbre, rms) per bar: chroma and MFCC means, z-scored per
    dimension, and the bar's RMS level in dB."""
    from khensu_services.analysis import _lean_spectral as _lean

    # Bounded memory, librosa's values (_lean_spectral): the whole-track
    # chroma's tuning estimate was the deep analysis's peak (2026-11-18).
    chroma = _lean.chroma_stft(mono, sr, hop_length=_HOP, n_fft=4 * _HOP)
    mfcc = _lean.mfcc(mono, sr, n_mfcc=14, hop_length=_HOP, n_fft=4 * _HOP)[1:]
    rms = _lean.rms(mono, hop_length=_HOP, frame_length=4 * _HOP)[0]
    frames = np.clip((np.asarray(bars) * sr / _HOP).astype(int), 0, chroma.shape[1] - 1)
    edges = np.concatenate([frames, [chroma.shape[1]]])
    h, t, r = [], [], []
    for a, b in zip(edges[:-1], edges[1:]):
        b = max(b, a + 1)
        h.append(chroma[:, a:b].mean(axis=1))
        t.append(mfcc[:, a:b].mean(axis=1))
        r.append(20.0 * np.log10(float(rms[a:b].mean()) + 1e-9))

    def z(x: npt.NDArray) -> npt.NDArray:
        x = np.asarray(x, dtype=float)
        return (x - x.mean(axis=0)) / (x.std(axis=0) + 1e-9)

    return z(np.array(h)), z(np.array(t)), np.asarray(r, dtype=float)


def _cosine_ssm(x: npt.NDArray) -> npt.NDArray:
    n = x / (np.linalg.norm(x, axis=1, keepdims=True) + 1e-9)
    return n @ n.T


def novelty_curve(ssm: npt.NDArray, half: int = _KERNEL_BARS) -> npt.NDArray:
    """Foote novelty: a Gaussian-tapered checkerboard kernel along the diagonal."""
    n = ssm.shape[0]
    size = 2 * half
    idx = np.arange(size) - half + 0.5
    g = np.exp(-0.5 * (idx / (0.5 * half)) ** 2)
    taper = np.outer(g, g)
    sign = np.outer(np.sign(idx), np.sign(idx))
    kernel = sign * taper  # +1 within each side, -1 across (Foote 2000)
    padded = np.pad(ssm, half, mode="edge")
    nov = np.zeros(n)
    for i in range(n):
        win = padded[i:i + size, i:i + size]
        nov[i] = float((win * kernel).sum())
    nov = np.maximum(nov, 0.0)
    return nov / nov.max() if nov.max() > 0 else nov


def pick_boundaries(nov: npt.NDArray, min_gap: int = _MIN_SECTION_BARS) -> list[int]:
    """Bar indices of section starts (always including 0): local maxima of
    the novelty above an adaptive threshold, greedily strongest first, at
    least ``min_gap`` bars apart and from either end."""
    n = len(nov)
    if n < 2 * min_gap:
        return [0]
    thresh = max(0.2, float(np.median(nov) + 1.0 * np.std(nov)))
    peaks = [i for i in range(1, n - 1) if nov[i] >= nov[i - 1] and nov[i] >= nov[i + 1] and nov[i] >= thresh]
    chosen: list[int] = []
    for i in sorted(peaks, key=lambda k: -nov[k]):
        if i < min_gap or n - i < min_gap:
            continue
        if all(abs(i - c) >= min_gap for c in chosen):
            chosen.append(i)
    return [0] + sorted(chosen)


def group_segments(seg_harmony: npt.NDArray, seg_timbre: npt.NDArray,
                   max_distance: float = _GROUP_DISTANCE) -> list[int]:
    """Repetition group per segment (0, 1, ... in first-heard order):
    single-link on cosine distance of the segments' mean features, harmony
    weighted twice timbre."""
    feats = np.hstack([2.0 * seg_harmony, seg_timbre])
    sim = _cosine_ssm(feats)
    dist = 1.0 - sim
    n = len(feats)
    group = list(range(n))

    def find(a: int) -> int:
        while group[a] != a:
            group[a] = group[group[a]]
            a = group[a]
        return a

    for i in range(n):
        for j in range(i + 1, n):
            if dist[i, j] < max_distance:
                ri, rj = find(i), find(j)
                if ri != rj:
                    group[max(ri, rj)] = min(ri, rj)
    roots = [find(i) for i in range(n)]
    order: dict[int, int] = {}
    for r in roots:
        order.setdefault(r, len(order))
    return [order[r] for r in roots]


def assign_roles(groups: list[int], levels: list[float], bars: list[int]) -> list[str]:
    """Role per segment from its repetition group, level and length (bars)."""
    n = len(groups)
    if n == 0:
        return []
    if n == 1:
        return ["section"]
    members: dict[int, list[int]] = {}
    for i, g in enumerate(groups):
        members.setdefault(g, []).append(i)

    def mean_level(g: int) -> float:
        return float(np.mean([levels[i] for i in members[g]]))

    repeated = [g for g, m in members.items() if len(m) >= 2]
    roles = ["section"] * n
    chorus = max(repeated, key=mean_level) if repeated else None
    verse = None
    if chorus is not None:
        others = [g for g in repeated if g != chorus]
        if others:
            verse = max(others, key=lambda g: (len(members[g]), -members[g][0]))
        for i in members[chorus]:
            roles[i] = "chorus"
        if verse is not None:
            for i in members[verse]:
                roles[i] = "verse"
    loud = max(levels)
    if roles[0] == "section" and len(members[groups[0]]) == 1 and levels[0] < loud - 3.0:
        roles[0] = "intro"
    if roles[-1] == "section" and len(members[groups[-1]]) == 1 and levels[-1] < loud - 3.0:
        roles[-1] = "outro"
    chorus_idx = [i for i, r in enumerate(roles) if r == "chorus"]
    verse_level = mean_level(verse) if verse is not None else None
    for i in range(n):
        if roles[i] != "section":
            continue
        nxt = roles[i + 1] if i + 1 < n else ""
        if chorus_idx and chorus_idx[0] < i < chorus_idx[-1] and len(members[groups[i]]) == 1:
            roles[i] = "bridge"
        elif nxt == "chorus" and bars[i] <= 8 and (verse_level is None or levels[i] > verse_level):
            roles[i] = "pre-chorus"
        elif chorus is not None and verse is None and i < (chorus_idx[0] if chorus_idx else n):
            roles[i] = "verse"
    return roles


def collapse_alternation(starts: list[int], groups: list[int], nb: int,
                         max_bars: int = 2 * _MIN_SECTION_BARS - 2,
                         min_run: int = 4) -> tuple[list[int], list[int]]:
    """Folds each run of short segments that alternates between two groups
    (A B A B ..., each at most ``max_bars`` - shorter than the 8-bar parts a
    real verse / chorus alternation uses) into one segment; returns the
    new starts and groups (0, 1, ... in first-heard order).

    A 4+4-bar call and response inside one part reads as two groups taking
    turns: a released song (RUN-10) came out as 20 sections, a "verse" and
    a "chorus" every 6 s. A folded run's group is the pair it alternates
    between, so the same exchange later in the song is the same part."""
    ends = starts[1:] + [nb]
    short = [b - a <= max_bars for a, b in zip(starts, ends)]
    new_starts: list[int] = []
    keys: list[tuple[int, ...]] = []
    k = 0
    while k < len(starts):
        j = k
        while (j + 1 < len(starts) and short[j] and short[j + 1]
               and groups[j + 1] != groups[j]
               and (j + 1 - k < 2 or groups[j + 1] == groups[j - 1])):
            j += 1
        new_starts.append(starts[k])
        if j - k + 1 >= min_run:
            keys.append(tuple(sorted({groups[k], groups[k + 1]})))
            k = j + 1
        else:
            keys.append((groups[k],))
            k += 1
    order: dict[tuple[int, ...], int] = {}
    for key in keys:
        order.setdefault(key, len(order))
    return new_starts, [order[key] for key in keys]


def structure_from_bars(harmony: npt.NDArray, timbre: npt.NDArray, levels_db: npt.NDArray,
                        bar_times: list[float], total_sec: float) -> list[dict[str, Any]]:
    """Sections from bar-level features (split out so tests can drive it
    with synthetic bars)."""
    nb = len(bar_times)
    if nb < 2 * _MIN_SECTION_BARS:
        return _number_labels([_section(0.0, total_sec, "section", 0, 1.0, max(1, nb))])
    ssm = _cosine_ssm(np.hstack([harmony, timbre]))
    nov = novelty_curve(ssm)
    starts = pick_boundaries(nov)
    # Group, merge neighbours that repeat each other (one verse split by a
    # fill is still one verse), and group the merged segments again.
    for _ in range(3):
        ends = starts[1:] + [nb]
        seg_h = np.array([harmony[a:b].mean(axis=0) for a, b in zip(starts, ends)])
        seg_t = np.array([timbre[a:b].mean(axis=0) for a, b in zip(starts, ends)])
        groups = group_segments(seg_h, seg_t)
        merged = [starts[0]] + [starts[k] for k in range(1, len(starts)) if groups[k] != groups[k - 1]]
        if merged == starts:
            break
        starts = merged
    starts, groups = collapse_alternation(starts, groups, nb)
    ends = starts[1:] + [nb]
    seg_l = [float(np.mean(levels_db[a:b])) for a, b in zip(starts, ends)]
    roles = assign_roles(groups, seg_l, [b - a for a, b in zip(starts, ends)])
    out = []
    for k, (a, b) in enumerate(zip(starts, ends)):
        t0 = 0.0 if k == 0 else float(bar_times[a])
        t1 = total_sec if b >= nb else float(bar_times[b])
        out.append(_section(t0, t1, roles[k], groups[k], float(nov[a]) if k else 1.0, b - a))
    return _number_labels(out)


def _section(start: float, end: float, role: str, group: int, strength: float, bars: int) -> dict[str, Any]:
    return {"start_sec": round(start, 3), "end_sec": round(end, 3), "duration_sec": round(end - start, 3),
            "role": role, "group": chr(ord("A") + group % 26), "boundary_strength": round(strength, 3),
            "bars": int(bars)}


def _number_labels(sections: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """'Verse 1', 'Chorus 2'...; a role heard once has no number."""
    totals: dict[str, int] = {}
    for s in sections:
        totals[s["role"]] = totals.get(s["role"], 0) + 1
    seen: dict[str, int] = {}
    for s in sections:
        seen[s["role"]] = seen.get(s["role"], 0) + 1
        name = s["role"].capitalize()
        s["label"] = f"{name} {seen[s['role']]}" if totals[s["role"]] > 1 else name
    return sections


def analyze_structure(y: npt.NDArray, sr: int, beat_times: list[float] | None = None) -> dict[str, Any]:
    """Sections of ``y`` (mono or (channels, samples)). ``method`` is
    ``self_similarity`` when boundaries came from the audio, ``single`` for a
    track too short or too beat-less to segment. Pass ``beat_times`` when the
    caller already tracked the beats."""
    mono = y.mean(axis=0) if y.ndim == 2 else y
    total = float(len(mono) / sr)
    try:
        bars, per_bar = _bar_lines(mono, sr, beat_times)
    except Exception:
        bars, per_bar = [], 4
    if len(bars) < 2 * _MIN_SECTION_BARS:
        return {"method": "single", "beats_per_bar": per_bar,
                "sections": _number_labels([_section(0.0, total, "section", 0, 1.0, max(1, len(bars)))])}
    harmony, timbre, levels = _bar_features(mono, sr, bars)
    return {"method": "self_similarity", "beats_per_bar": per_bar,
            "sections": structure_from_bars(harmony, timbre, levels, bars, total)}
