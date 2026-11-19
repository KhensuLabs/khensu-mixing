# local-services/khensu_services/analysis/gain_xray.py
"""Compression X-Ray: reconstructs what dynamics processing actually did by
comparing the original audio against a processed render — the effective
gain curve over time, plus the failure signatures users can't hear until
someone points at them:

  pumping        — periodic gain modulation locked to the song's rhythm
                   (release breathing audibly with the beat)
  over-compression — sustained deep gain reduction that flattens
                   macrodynamics instead of controlling peaks

Works on any original/processed pair, so it covers the whole render
pipeline (apply_chain, mastering agent, vocal macros) without needing
per-plugin telemetry hooks.

Stateless, numpy-only.
"""
from __future__ import annotations

from typing import Any

import numpy as np
import numpy.typing as npt

_WIN_SEC = 0.020   # 20 ms windows — fast enough to see release breathing
# Pumping periods searched (s), and the detrend window: gain movement slower
# than this is levelling, not pumping, and is removed before the periodicity
# test (DSP-03).
_PUMP_MIN_SEC = 0.2
_PUMP_MAX_SEC = 2.0
_DETREND_SEC = 4.0
# An autocorrelation peak must rise this far above the trough before it to
# count as a period (a smooth curve's autocorrelation decays monotonically and
# has no such peak).
_PUMP_PROMINENCE = 0.2
# Latency search between original and processed (lookahead limiters,
# linear-phase EQs) - the 20 ms curve would smear without it.
_MAX_LATENCY_SEC = 0.25
_ALIGN_SEC = 30.0


def _autocorr(g: npt.NDArray, max_lag: int) -> npt.NDArray:
    """Normalised autocorrelation for lags 0..max_lag via FFT - O(n log n);
    ``np.correlate(mode="full")`` was O(n^2) for the ~100 lags used
    (7.3 s on a 30 min file; DSP-17)."""
    n = g.size
    nfft = 1 << int(np.ceil(np.log2(2 * n)))
    spec = np.fft.rfft(g, nfft)
    ac = np.fft.irfft(spec * np.conj(spec), nfft)[: max_lag + 1]
    return ac / (ac[0] + 1e-12)


def _pumping(dyn_gain: npt.NDArray) -> tuple[float, float]:
    """(score 0..1, period s): the height of a TRUE interior local maximum of
    the detrended gain curve's autocorrelation in the 0.2-2 s lag range,
    provided it is prominent (rises ``_PUMP_PROMINENCE`` above the trough
    before it). The old argmax over the range picked the first lag of any
    smooth, monotonically decaying curve - a random-walk levelling move
    scored 0.98 "pumping" (DSP-03)."""
    g = np.asarray(dyn_gain, dtype=np.float64)
    if g.size <= 200:
        return 0.0, 0.0
    # High-pass: subtract a moving average longer than the slowest period.
    k = max(1, int(_DETREND_SEC / _WIN_SEC))
    if g.size > k:
        kernel = np.ones(k) / k
        g = g - np.convolve(g, kernel, mode="same")
    g = g - np.mean(g)
    if float(np.std(g)) <= 0.3:
        return 0.0, 0.0
    lo = int(_PUMP_MIN_SEC / _WIN_SEC)
    hi = min(int(_PUMP_MAX_SEC / _WIN_SEC), g.size - 2)
    if hi <= lo + 1:
        return 0.0, 0.0
    ac = _autocorr(g, hi + 1)
    best, best_lag = 0.0, 0
    for lag in range(max(lo, 1), hi + 1):
        if not (ac[lag] > ac[lag - 1] and ac[lag] >= ac[lag + 1]):
            continue                      # not an interior local maximum
        prominence = ac[lag] - float(np.min(ac[1:lag + 1]))
        if prominence >= _PUMP_PROMINENCE and ac[lag] > best:
            best, best_lag = float(ac[lag]), lag
    return float(np.clip(best, 0.0, 1.0)), best_lag * _WIN_SEC


def _latency_samples(y_o: npt.NDArray, y_p: npt.NDArray, sr: int) -> int:
    """Samples by which ``y_p`` lags ``y_o`` (cross-correlation peak within
    +/- 250 ms over the first 30 s); 0 when the correlation is weak."""
    n = min(len(y_o), len(y_p), int(_ALIGN_SEC * sr))
    max_lag = int(_MAX_LATENCY_SEC * sr)
    if n < 4 * max_lag:
        return 0
    a = np.asarray(y_o[:n], dtype=np.float64)
    b = np.asarray(y_p[:n], dtype=np.float64)
    nfft = 1 << int(np.ceil(np.log2(2 * n)))
    xc = np.fft.irfft(np.fft.rfft(b, nfft) * np.conj(np.fft.rfft(a, nfft)), nfft)
    lags = np.concatenate([np.arange(0, max_lag + 1), np.arange(-max_lag, 0)])
    vals = np.concatenate([xc[: max_lag + 1], xc[nfft - max_lag:]])
    i = int(np.argmax(np.abs(vals)))
    norm = float(np.sqrt(np.dot(a, a) * np.dot(b, b))) + 1e-20
    return int(lags[i]) if abs(vals[i]) / norm > 0.3 else 0


def _window_rms_db(mono: npt.NDArray, sr: int) -> npt.NDArray:
    win = max(1, int(_WIN_SEC * sr))
    n_win = len(mono) // win
    if n_win == 0:
        return np.array([-90.0])
    rms = np.sqrt(np.mean(mono[: n_win * win].reshape(n_win, win) ** 2, axis=1) + 1e-20)
    return 20.0 * np.log10(rms + 1e-12)


def _window_brightness_db(mono: npt.NDArray, sr: int) -> npt.NDArray:
    """Per-window tonal-balance measure: 10*log10(high-band / low-band energy)
    on the same 20 ms grid as the RMS curve. Higher = brighter. The difference
    of two of these curves shows where a chain shifted tone over time, not just
    level — the spectral companion to the gain curve."""
    win = max(1, int(_WIN_SEC * sr))
    n_win = len(mono) // win
    if n_win == 0:
        return np.array([0.0])
    frames = mono[: n_win * win].reshape(n_win, win)
    spec = np.abs(np.fft.rfft(frames * np.hanning(win), axis=1)) ** 2
    freqs = np.fft.rfftfreq(win, d=1.0 / sr)
    low = (freqs >= 100.0) & (freqs < 2000.0)
    high = freqs >= 2000.0
    low_e = np.sum(spec[:, low], axis=1) + 1e-12
    high_e = np.sum(spec[:, high], axis=1) + 1e-12
    return 10.0 * np.log10(high_e / low_e)


def analyze_gain_xray(original_path: str, processed_path: str) -> dict[str, Any]:
    from khensu_services.analysis.context import AnalysisContext

    y_o, sr_o = AnalysisContext(original_path).load(sr=None, mono=True)   # shared decode cache
    sr = int(sr_o)
    y_p, _ = AnalysisContext(processed_path).load(sr=sr, mono=True)

    # Undo processing latency (lookahead, linear phase) before comparing
    # 20 ms windows.
    lag = _latency_samples(y_o, y_p, sr)
    if lag > 0:
        y_p = y_p[lag:]
    elif lag < 0:
        y_o = y_o[-lag:]

    n = min(len(y_o), len(y_p))
    if n < sr:   # under a second of overlap — nothing meaningful
        return {"window_sec": _WIN_SEC, "gain_curve_db": [], "events": [],
                "avg_gain_db": 0.0, "max_reduction_db": 0.0,
                "pumping_score": 0.0, "verdict": "insufficient overlap"}
    y_o, y_p = y_o[:n], y_p[:n]

    db_o = _window_rms_db(y_o, sr)
    db_p = _window_rms_db(y_p, sr)
    m = min(len(db_o), len(db_p))
    db_o, db_p = db_o[:m], db_p[:m]

    # Gain curve: processed minus original, only where the original has
    # signal (gain of silence is meaningless).
    valid = db_o > -60.0
    gain = np.where(valid, db_p - db_o, 0.0)

    # Static level offset (makeup gain / normalization) isn't "compression" —
    # remove the median so the curve shows *dynamic* gain movement.
    median_gain = float(np.median(gain[valid])) if np.any(valid) else 0.0
    dyn_gain = np.where(valid, gain - median_gain, 0.0)

    max_reduction = float(-np.min(dyn_gain)) if dyn_gain.size else 0.0
    avg_reduction = float(-np.mean(dyn_gain[valid & (dyn_gain < -0.5)])) \
        if np.any(valid & (dyn_gain < -0.5)) else 0.0

    # ---- Pumping: a real periodicity of the gain curve in the 0.2-2 s range. ----
    pumping_score, pump_period = _pumping(dyn_gain)

    # ---- Events: sustained deep-reduction stretches. ----
    events: list[dict[str, Any]] = []
    deep = dyn_gain < -6.0
    i = 0
    while i < deep.size:
        if not deep[i]:
            i += 1
            continue
        j = i
        while j < deep.size and deep[j]:
            j += 1
        if (j - i) * _WIN_SEC >= 1.0:   # >= 1 s of >6 dB reduction
            events.append({
                "start_sec": round(i * _WIN_SEC, 2),
                "end_sec": round(j * _WIN_SEC, 2),
                "code": "over_compression",
                "message": (f"{(j - i) * _WIN_SEC:.1f} s of sustained "
                            f">{6.0:.0f} dB reduction — this stretch is being "
                            "flattened, not controlled."),
            })
        i = j
    if pumping_score > 0.45 and pump_period > 0.0:
        events.insert(0, {
            "start_sec": 0.0, "end_sec": 0.0, "code": "pumping",
            "message": (f"Gain modulates with a ~{pump_period:.2f} s period "
                        f"(correlation {pumping_score:.2f}) — release breathing "
                        "with the beat. Slower release or lower ratio will calm it."),
        })

    if max_reduction < 1.0:
        verdict = "transparent — barely touching the dynamics"
    elif pumping_score > 0.45:
        verdict = "pumping detected"
    elif any(e["code"] == "over_compression" for e in events):
        verdict = "over-compressed stretches"
    elif avg_reduction <= 4.0:
        verdict = "musical — working in the 2-4 dB sweet spot"
    else:
        verdict = "heavy but steady"

    # Brightness (tonal) delta on the same window grid — where the chain moved
    # tone, not just level. Static tonal shifts are meaningful (unlike makeup
    # gain), so this curve is NOT median-detrended.
    br_o = _window_brightness_db(y_o, sr)
    br_p = _window_brightness_db(y_p, sr)
    bm = min(len(br_o), len(br_p), m)
    bright_delta = np.where(valid[:bm], br_p[:bm] - br_o[:bm], 0.0)

    # Downsample the curve for UI (max ~600 points).
    step = max(1, dyn_gain.size // 600)
    curve = [round(float(v), 2) for v in dyn_gain[::step]]
    bright_curve = [round(float(v), 2) for v in bright_delta[::step]]

    return {
        "window_sec": _WIN_SEC * step,
        "gain_curve_db": curve,
        "brightness_curve_db": bright_curve,
        "events": events,
        "avg_gain_db": round(-avg_reduction, 2),
        "max_reduction_db": round(max_reduction, 2),
        "static_offset_db": round(median_gain, 2),
        "pumping_score": round(pumping_score, 3),
        "verdict": verdict,
    }
