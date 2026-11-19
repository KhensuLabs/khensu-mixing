# local-services/khensu_services/analysis/sub_bass_phase.py
"""Sub-bass phase alignment between the kick and the bass fundamental.

Where the kick thump and the bass note share the sub/low band, their phase
relationship decides whether the low end sums cleanly (in phase) or partially
cancels on mono/club/phone systems (out of phase) — the classic "the low end
disappears on the big system" problem. This locates it with:

  * the dominant phase difference (amplitude-weighted, via the analytic signal)
  * a linear correlation over the low band (-1..1)
  * a windowed correlation history, so a section where they fight is visible
  * a decimated (kick, bass) point cloud for a goniometer/Lissajous plot

Stateless, numpy/scipy only (mirrors signal_integrity.py's style). The kick is
approximated by low-passing the drums stem, so callers can pass the Demucs
`drums` stem directly — the cutoff isolates the kick's sub energy.
"""
from __future__ import annotations

from typing import Any

import numpy as np
import numpy.typing as npt
import scipy.signal as sig


def _effective_cutoff(sr: int, cutoff_hz: float) -> float:
    """The cutoff `_lowpass` can actually realise at `sr` (audit AUD-01).

    `cutoff_hz` arrives straight off the request model, which sets no upper
    bound, so a cutoff at or above Nyquist is caller-reachable. `sig.butter`
    requires 0 < Wn < 1 and raises ValueError otherwise, which surfaced as an
    HTTP 500 rather than a degraded-but-valid result. 0.45*sr keeps the 4th-order
    design comfortably stable instead of ringing at the very edge of the band.
    """
    return min(float(cutoff_hz), sr * 0.45)


def _lowpass(mono: npt.NDArray, sr: int, cutoff_hz: float) -> npt.NDArray:
    cutoff = _effective_cutoff(sr, cutoff_hz)
    if cutoff <= 0.0:
        # A non-positive cutoff passes nothing; the caller's energy gate then
        # reports "insufficient sub-bass" rather than the filter design failing.
        return np.zeros_like(mono, dtype=np.float64)
    sos = sig.butter(4, cutoff, btype="low", fs=sr, output="sos")
    return np.asarray(sig.sosfilt(sos, mono), dtype=np.float64)


def _pearson(a: npt.NDArray, b: npt.NDArray) -> float:
    """The shared ``dsp_metrics.pearson`` (0.0 when either side is silent)."""
    from khensu_services.analysis.dsp_metrics import pearson
    if a.size < 2 or b.size < 2:
        return 0.0
    return pearson(a, b, silent=0.0)


def _load_mono(path: str) -> tuple[npt.NDArray, int]:
    from khensu_services.analysis.context import AnalysisContext
    y, sr = AnalysisContext(path).load(sr=None, mono=True)   # shared decode cache
    return np.asarray(y, dtype=np.float64), int(sr)


def analyze_sub_bass_phase(
    kick_path: str,
    bass_path: str,
    cutoff_hz: float = 120.0,
    window_sec: float = 0.5,
    max_scatter: int = 800,
    max_series: int = 300,
) -> dict[str, Any]:
    yk, srk = _load_mono(kick_path)
    yb, srb = _load_mono(bass_path)

    sr = min(srk, srb) or 44100
    # Resolve the cutoff against the rate we actually filter at, so every
    # downstream path (including the early returns) reports the cutoff that was
    # used rather than the one that was asked for (audit AUD-01).
    cutoff_hz = _effective_cutoff(sr, cutoff_hz)
    if srk != sr:
        import librosa
        yk = librosa.resample(yk, orig_sr=srk, target_sr=sr)
    if srb != sr:
        import librosa
        yb = librosa.resample(yb, orig_sr=srb, target_sr=sr)

    n = min(len(yk), len(yb))
    yk, yb = yk[:n], yb[:n]
    if n < sr // 4:  # under ~0.25 s of overlap — nothing meaningful to say
        return _empty(cutoff_hz)

    lk = _lowpass(yk, sr, cutoff_hz)
    lb = _lowpass(yb, sr, cutoff_hz)

    # Not enough sub energy in one of the stems -> no alignment to judge.
    ek = float(np.sqrt(np.mean(lk * lk)))
    eb = float(np.sqrt(np.mean(lb * lb)))
    if ek < 1e-4 or eb < 1e-4:
        out = _empty(cutoff_hz)
        out["verdict"] = "Insufficient sub-bass content in kick and/or bass to judge phase."
        return out

    correlation = _pearson(lk, lb)

    # Dominant phase difference: amplitude-weighted circular mean of the
    # instantaneous phase difference via the analytic signal. 0deg = in phase
    # (sub sums), +/-180deg = out of phase (sub cancels on mono).
    zk = sig.hilbert(lk)
    zb = sig.hilbert(lb)
    cross = np.sum(zk * np.conj(zb))
    phase_deg = float(np.degrees(np.angle(cross)))

    # Windowed correlation history.
    win = max(256, int(window_sec * sr))
    series: list[dict[str, float]] = []
    for start in range(0, n - win + 1, win):
        c = _pearson(lk[start:start + win], lb[start:start + win])
        series.append({"time_sec": round(start / sr, 3), "correlation": round(c, 3)})
    if len(series) > max_series:
        step = int(np.ceil(len(series) / max_series))
        series = series[::step]

    # Goniometer point cloud: decimated, silence-gated (kick, bass) pairs,
    # each normalized to its own peak so the shape (not the level) reads.
    kpk = float(np.max(np.abs(lk))) + 1e-12
    bpk = float(np.max(np.abs(lb))) + 1e-12
    xn, yn = lk / kpk, lb / bpk
    step = max(1, n // max_scatter)
    scatter: list[list[float]] = []
    for i in range(0, n, step):
        if abs(xn[i]) < 0.02 and abs(yn[i]) < 0.02:
            continue
        scatter.append([round(float(xn[i]), 3), round(float(yn[i]), 3)])

    abs_phase = abs(phase_deg)
    if abs_phase <= 45.0 and correlation >= 0.25:
        verdict = "Aligned — kick and bass sub sum cleanly."
    elif abs_phase >= 135.0 or correlation <= -0.25:
        verdict = "Out of phase — sub cancels on mono / club / phone systems."
    else:
        verdict = "Partially aligned — some low-end loss on mono playback."

    return {
        "cutoff_hz": float(cutoff_hz),
        "correlation": round(correlation, 3),
        "phase_degrees": round(phase_deg, 1),
        "verdict": verdict,
        "correlation_over_time": series,
        "goniometer": scatter,
    }


def _empty(cutoff_hz: float) -> dict[str, Any]:
    return {
        "cutoff_hz": float(cutoff_hz),
        "correlation": 0.0,
        "phase_degrees": 0.0,
        "verdict": "No sub-bass phase data.",
        "correlation_over_time": [],
        "goniometer": [],
    }
