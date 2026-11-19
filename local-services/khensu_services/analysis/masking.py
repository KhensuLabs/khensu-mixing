# local-services/khensu_services/analysis/masking.py
"""
Masking & Frequency Collision Detector: per-stem band-energy profiles and
pairwise frequency-collision (masking-risk) scores, built on top of the
existing stem-separation pipeline's output files.

Stateless, numpy-only (mirrors extended_analyzer.py's style).
"""
from __future__ import annotations
import numpy as np
import numpy.typing as npt
import librosa
from typing import Any

# Wider, coarser bands than extended_analyzer's mono-compat bands — fine
# enough to localize a collision, coarse enough that the stem-pair matrix
# UI stays readable.
_MASKING_BANDS: list[tuple[float, float]] = [
    (20.0, 60.0), (60.0, 120.0), (120.0, 250.0), (250.0, 500.0),
    (500.0, 1000.0), (1000.0, 2000.0), (2000.0, 4000.0),
    (4000.0, 8000.0), (8000.0, 12000.0), (12000.0, 20000.0),
]


# Level gates (DSP-04). Band energies are measured on a sine-peak scale (a
# full-scale sine in a band reads 0 dB), so the floors are dBFS-like.
# A band only counts as a collision when BOTH stems put real energy there and
# neither is more than _GATE_REL_DB below the other: a -90 dBFS Demucs bleed
# under a loud stem is not "two parts fighting for the same space".
_GATE_REL_DB = 20.0
_BAND_FLOOR_DB = -80.0
# Heatmap frames in which either stem is below this (whole-frame level) are
# not collisions at all.
_FRAME_FLOOR_DB = -60.0


def _edges() -> npt.NDArray:
    from khensu_services.analysis.dsp_metrics import bands_to_edges
    return bands_to_edges(_MASKING_BANDS)


def _sine_scale(n_fft: int) -> float:
    """``|X|^2`` of a full-scale sine under an ``n_fft`` Hann window."""
    return (float(np.sum(np.hanning(n_fft))) / 2.0) ** 2


def _band_energies(mono: npt.NDArray, sr: int, n_fft: int = 4096) -> npt.NDArray:
    """Absolute per-band energy: frame-averaged ``sum |X|^2`` per band
    (``dsp_metrics.band_power_frames`` - the mean of the POWER, not the square
    of the mean magnitude; DSP-15), scaled so a full-scale sine reads 1.0."""
    from khensu_services.analysis.dsp_metrics import band_power_frames
    frames = band_power_frames(mono, sr, _edges(), n_fft)
    return frames.mean(axis=0) / _sine_scale(n_fft)


def _normalise(energies: npt.NDArray) -> npt.NDArray:
    total = float(energies.sum())
    return energies / total if total > 0.0 else energies


def _band_energy_profile(mono: npt.NDArray, sr: int, n_fft: int = 4096) -> npt.NDArray:
    """Normalized (sums to 1) per-band energy distribution, averaged over the
    whole track via overlapping Hann-windowed FFTs."""
    return _normalise(_band_energies(mono, sr, n_fft))


def _level_gate(ea: npt.NDArray, eb: npt.NDArray) -> npt.NDArray:
    """Per band: True when both absolute energies are above the floor and
    within ``_GATE_REL_DB`` of each other."""
    floor = 10.0 ** (_BAND_FLOOR_DB / 10.0)
    lo, hi = np.minimum(ea, eb), np.maximum(ea, eb)
    return (lo > floor) & (lo >= hi * 10.0 ** (-_GATE_REL_DB / 10.0))


def _collision_score(a: npt.NDArray, b: npt.NDArray,
                     gate: npt.NDArray | None = None) -> float:
    """Overlap coefficient ``sum min / sum max`` of two normalized profiles;
    bands outside ``gate`` contribute no overlap (they still count in the
    union, so a gated band lowers the score)."""
    mins = np.minimum(a, b)
    if gate is not None:
        mins = np.where(gate, mins, 0.0)
    overlap = float(mins.sum())
    union = float(np.maximum(a, b).sum())
    return overlap / union if union > 0.0 else 0.0


def _gated_pair(ea: npt.NDArray, eb: npt.NDArray) -> tuple[float, list[float]]:
    """(collision score, per-band scores) from two ABSOLUTE band-energy
    vectors: the shape overlap of the normalized profiles, counted only in
    bands where the two stems are level-comparable (DSP-04)."""
    pa, pb = _normalise(ea), _normalise(eb)
    gate = _level_gate(ea, eb)
    band_scores = [float(v) for v in np.where(gate, np.minimum(pa, pb), 0.0)]
    return _collision_score(pa, pb, gate), band_scores


def _windowed_collisions(ya: npt.NDArray, sra: int, yb: npt.NDArray, srb: int,
                          window_sec: float) -> list[dict[str, float]]:
    """Same collision-score formula as the whole-track average, computed per
    fixed-length time window so a masking hotspot in the chorus doesn't get
    diluted into a misleadingly-low track-wide number (and a quiet-verse
    non-issue doesn't get inflated by a loud chorus elsewhere)."""
    duration = min(len(ya) / sra, len(yb) / srb)
    windows: list[dict[str, float]] = []
    t = 0.0
    while t < duration:
        end = min(t + window_sec, duration)
        sa = ya[int(t * sra):int(end * sra)]
        sb = yb[int(t * srb):int(end * srb)]
        if len(sa) < 256 or len(sb) < 256:
            break
        score, _ = _gated_pair(_band_energies(sa, sra), _band_energies(sb, srb))
        windows.append({
            "start_sec": t,
            "end_sec": end,
            "collision_score": score,
        })
        t += window_sec
    return windows


def _stft_band_matrix(mono: npt.NDArray, sr: int, block: int, hop: int) -> npt.NDArray:
    """Per-frame ABSOLUTE band energy (sine-peak scale) -> [n_frames][n_bands].

    A short-block companion to _band_energies: one row per `block`-sample
    frame, so masking can be tracked at transient scale (Gemini audit)."""
    from khensu_services.analysis.dsp_metrics import band_power_frames
    return band_power_frames(mono, sr, _edges(), block, hop) / _sine_scale(block)


def masking_heatmap(ya: npt.NDArray, sra: int, yb: npt.NDArray, srb: int,
                    block_ms: float = 80.0, max_frames: int = 400) -> dict[str, Any]:
    """Time x frequency masking heatmap between two stems: per short block,
    per band, the collision intensity min(energy_a, energy_b) — where both
    stems put energy in the same band at the same instant. Resolves the
    transient-scale dynamic masking a 5 s window average smears out.

    Frames are bounded to `max_frames` by widening the block on long tracks,
    so the response stays small regardless of track length.
    """
    sr = sra
    if srb != sra:
        yb = librosa.resample(yb, orig_sr=srb, target_sr=sra)
    n = min(len(ya), len(yb))
    ya, yb = ya[:n], yb[:n]

    block = max(256, int(block_ms / 1000.0 * sr))
    est_frames = max(1, (n - block) // block + 1)
    if est_frames > max_frames:
        block *= int(np.ceil(est_frames / max_frames))
    hop = block  # non-overlapping blocks — one column per time slice

    ea = _stft_band_matrix(ya, sr, block, hop)
    eb = _stft_band_matrix(yb, sr, block, hop)
    m = min(len(ea), len(eb))
    ea, eb = ea[:m], eb[:m]
    if m > 0:
        # Per frame: shape overlap of the normalized profiles, only in bands
        # where both stems are level-comparable, and only in frames where
        # both stems are actually playing (DSP-04).
        def _norm_rows(e: npt.NDArray) -> npt.NDArray:
            tot = e.sum(axis=1, keepdims=True)
            return np.divide(e, tot, out=np.zeros_like(e), where=tot > 0.0)

        frame_floor = 10.0 ** (_FRAME_FLOOR_DB / 10.0)
        live = (ea.sum(axis=1) > frame_floor) & (eb.sum(axis=1) > frame_floor)
        gate = _level_gate(ea, eb) & live[:, None]
        grid = np.where(gate, np.minimum(_norm_rows(ea), _norm_rows(eb)), 0.0)
    else:
        grid = np.zeros((0, len(_MASKING_BANDS)))
    gmax = float(grid.max()) if grid.size else 0.0
    if gmax > 0.0:
        grid = grid / gmax   # normalize for display contrast

    return {
        "block_ms": round(block / sr * 1000.0, 1),
        "n_frames": int(m),
        "bands_hz": [lo for lo, _ in _MASKING_BANDS],
        "grid": [[round(float(v), 4) for v in row] for row in grid],
    }


def compute_stem_masking(stem_paths: dict[str, str], window_sec: float = 5.0) -> dict[str, Any]:
    """Loads each named stem file, computes its normalized band-energy
    profile, and scores pairwise frequency collision as the overlap
    coefficient (sum of per-band min / sum of per-band max) between each
    pair of stems that were actually provided.

    collision_score: 1.0 = the two stems occupy an identical frequency
    distribution at comparable levels (high masking risk, likely to fight for
    the same space); 0.0 = fully disjoint spectra, or one stem so much quieter
    in every shared band (> 20 dB, or below -80 dB) that it is not competing,
    e.g. separation bleed (DSP-04). Each pair also gets
    collisions_over_time, the same score computed per `window_sec`-second
    window, so collisions can be localized to song sections rather than
    only reported as a whole-track average.
    """
    from khensu_services.analysis.context import AnalysisContext

    raw: dict[str, tuple[npt.NDArray, int]] = {}
    for name, path in stem_paths.items():
        if not path:
            continue
        y, sr = AnalysisContext(path).load(sr=None, mono=True)   # shared decode cache
        raw[name] = (np.asarray(y), int(sr))

    energies = {name: _band_energies(y, sr) for name, (y, sr) in raw.items()}

    names = list(energies.keys())
    pairs: list[dict[str, Any]] = []
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            score, band_scores = _gated_pair(energies[names[i]], energies[names[j]])

            ya, sra = raw[names[i]]
            yb, srb = raw[names[j]]
            collisions_over_time = _windowed_collisions(ya, sra, yb, srb, window_sec)

            pairs.append({
                "stem_a": names[i],
                "stem_b": names[j],
                "collision_score": score,
                "band_scores": band_scores,
                "collisions_over_time": collisions_over_time,
            })

    # Sub-second time x frequency heatmap for the single worst-colliding pair,
    # preferring one that involves vocals (the Gemini item's focus). Only one
    # heatmap so the response stays bounded.
    heatmap: dict[str, Any] | None = None
    if pairs:
        def _rank(p: dict[str, Any]) -> tuple[int, float]:
            involves_vocals = 1 if ("vocals" in (p["stem_a"], p["stem_b"])) else 0
            return (involves_vocals, p["collision_score"])

        worst = max(pairs, key=_rank)
        ya, sra = raw[worst["stem_a"]]
        yb, srb = raw[worst["stem_b"]]
        hm = masking_heatmap(ya, sra, yb, srb)
        hm["stem_a"] = worst["stem_a"]
        hm["stem_b"] = worst["stem_b"]
        heatmap = hm

    return {
        "pairs": pairs,
        "bands_hz": [lo for lo, _ in _MASKING_BANDS],
        "heatmap": heatmap,
    }
