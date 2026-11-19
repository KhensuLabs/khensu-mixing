# local-services/khensu_services/analysis/signal_integrity.py
"""Signal Integrity: three checks that traditional peak/LUFS metering misses
until a track fails codec conversion or gets rejected by a mastering house.

  true_peak    — inter-sample peaks via 4x oversampling (ITU-R BS.1770-4 /
                 EBU R128 style). A track can read -1 dBFS on a sample-peak
                 meter and still clip after lossy encoding if the true peak
                 (the reconstructed analog waveform between samples) is hot.
  mono_bass    — low end that isn't summed to mono below ~120 Hz. Club/vinyl
                 systems and phone speakers often sum to mono; wide bass
                 partially cancels there, gutting the low end.
  noise_floor  — 50/60 Hz mains hum and broadband hiss above the program's
                 natural noise floor, detected in the quietest stretch of
                 the track (so program material doesn't mask it).

Stateless, numpy-only (mirrors extended_analyzer.py's style).
"""
from __future__ import annotations

from typing import Any

import numpy as np
import numpy.typing as npt
import scipy.signal as sig


# ---------------------------------------------------------------------------
# True peak
# ---------------------------------------------------------------------------

def analyze_true_peak(y_stereo: npt.NDArray, sr: int, oversample: int = 4) -> dict[str, Any]:
    """4x polyphase-oversampled true peak per channel + overall, in dBTP.

    y_stereo: (channels, samples), float in [-1, 1]. Mono accepted. The
    oversampling is the shared BS.1770-4 Annex 2-style polyphase FIR with
    zero padding at the ends (loudness.true_peak_per_channel_linear) — not
    a circular FFT resample, which wrapped the file's end into its start.
    """
    from khensu_services.analysis.loudness import true_peak_per_channel_linear

    y = y_stereo if y_stereo.ndim > 1 else y_stereo[np.newaxis, :]
    # max|y| == max(max y, -min y) exactly, without an |y| temporary.
    sample_peak_dbfs = (float(20.0 * np.log10(max(float(np.max(y)), -float(np.min(y))) + 1e-12))
                        if y.size else -240.0)
    per_channel_dbtp: list[float] = [
        round(float(20.0 * np.log10(p + 1e-12)), 2)
        for p in true_peak_per_channel_linear(y, oversample)
    ]

    true_peak_dbtp = float(max(per_channel_dbtp)) if per_channel_dbtp else -100.0
    # EBU R128 / most distributors: -1.0 dBTP is the safe ceiling.
    overs_count = sum(1 for v in per_channel_dbtp if v > -1.0)

    if true_peak_dbtp > -0.3:
        verdict = "will clip after lossy encoding — lower the ceiling"
    elif true_peak_dbtp > -1.0:
        verdict = "tight — some codecs may still overshoot"
    else:
        verdict = "safe true-peak margin"

    return {
        "true_peak_dbtp": round(true_peak_dbtp, 2),
        "sample_peak_dbfs": round(sample_peak_dbfs, 2),
        "inter_sample_overshoot_db": round(true_peak_dbtp - sample_peak_dbfs, 2),
        "per_channel_dbtp": per_channel_dbtp,
        "over_minus_1dbtp": overs_count > 0,
        "verdict": verdict,
    }


def true_peak_events(
    y_stereo: npt.NDArray,
    sr: int,
    threshold_dbtp: float = -1.0,
    oversample: int = 4,
    merge_sec: float = 0.05,
    max_events: int = 200,
) -> dict[str, Any]:
    """Timestamps + magnitudes of true-peak overshoots over the threshold.

    Unlike analyze_true_peak (one whole-track dBTP number), this locates
    *where* the inter-sample peaks exceed the ceiling, so overshoot density
    can be visualised on the loudness timeline (Gemini audit). Uses polyphase
    FIR oversampling (the shared loudness.oversampled_envelope — the ITU
    BS.1770 approach, and far cheaper than FFT resample on long files).

    Adjacent over-threshold samples within `merge_sec` collapse into one
    event (its loudest point). Returns at most `max_events`, the loudest kept,
    but `over_count` reports the true number of distinct events.
    """
    from khensu_services.analysis.loudness import iter_oversampled_envelope

    y = y_stereo if y_stereo.ndim > 1 else y_stereo[np.newaxis, :]
    up_sr = sr * oversample
    if y.shape[-1] == 0:
        return {"threshold_dbtp": float(threshold_dbtp), "events": [], "over_count": 0}

    thr_lin = 10.0 ** (threshold_dbtp / 20.0)
    # Only the over-threshold points are kept, chunk by chunk: the full
    # oversampled envelope was the route's largest allocation.
    idx_parts: list[np.ndarray] = []
    val_parts: list[np.ndarray] = []
    for start, chunk in iter_oversampled_envelope(y, oversample):
        hit = np.flatnonzero(chunk > thr_lin)
        if hit.size:
            idx_parts.append(hit + start)
            val_parts.append(chunk[hit])
    over_idx = np.concatenate(idx_parts) if idx_parts else np.zeros(0, dtype=np.int64)
    over_val = np.concatenate(val_parts) if val_parts else np.zeros(0)

    events: list[dict[str, float]] = []
    if over_idx.size:
        merge_gap = max(1, int(merge_sec * up_sr))
        splits = np.flatnonzero(np.diff(over_idx) > merge_gap) + 1
        for grp, vals in zip(np.split(over_idx, splits), np.split(over_val, splits)):
            k = int(np.argmax(vals))
            peak_idx = int(grp[k])
            mag = float(20.0 * np.log10(float(vals[k]) + 1e-12))
            events.append({"time_sec": round(peak_idx / up_sr, 3), "dbtp": round(mag, 2)})

    over_count = len(events)
    if over_count > max_events:
        events.sort(key=lambda e: e["dbtp"], reverse=True)
        events = events[:max_events]
    events.sort(key=lambda e: e["time_sec"])

    return {"threshold_dbtp": float(threshold_dbtp), "events": events, "over_count": over_count}


# ---------------------------------------------------------------------------
# Mono-bass compatibility
# ---------------------------------------------------------------------------

def analyze_mono_bass(y_stereo: npt.NDArray, sr: int, cutoff_hz: float = 120.0) -> dict[str, Any]:
    """Correlation of L/R below cutoff_hz — low correlation there means the
    bass will partially cancel when a system sums to mono."""
    if y_stereo.ndim < 2 or y_stereo.shape[0] < 2:
        return {"correlation_below_cutoff": 1.0, "cutoff_hz": cutoff_hz,
                "energy_at_risk_db": 0.0, "verdict": "mono source — no risk"}

    left, right = y_stereo[0], y_stereo[1]
    sos = sig.butter(4, cutoff_hz, btype="low", fs=sr, output="sos")
    l_lo = sig.sosfilt(sos, left)
    r_lo = sig.sosfilt(sos, right)

    # The same elementwise arithmetic as the straightforward expressions,
    # done in place and in an order that frees each full-length buffer as
    # soon as its last use is past (2026-11-06 intake memory pass) - every
    # value below is bit-identical to the earlier whole-array temporaries.
    l_std, r_std = float(np.std(l_lo)), float(np.std(r_lo))
    # A track with no real content below cutoff_hz still leaves a filter-
    # transition-band noise floor in l_lo/r_lo; that residual's L/R
    # correlation is essentially random and must not read as a mono-bass
    # risk. Compare the low band's RMS against the full-mix RMS rather than
    # an absolute epsilon, since the "negligible" floor scales with the
    # source's overall level, not a fixed amplitude.
    full = left + right
    full *= 0.5
    np.multiply(full, full, out=full)
    full_rms = float(np.sqrt(np.mean(full)) + 1e-12)
    del full
    mono_lo = l_lo + r_lo                       # 0.5 * (L + R) of the low band
    mono_lo *= 0.5
    np.multiply(mono_lo, mono_lo, out=mono_lo)
    mono_ms = np.mean(mono_lo)
    del mono_lo
    low_rms = float(np.sqrt(mono_ms) + 1e-12)
    negligible_low_band = low_rms < full_rms * 10 ** (-30.0 / 20.0)  # ~-30 dB
    # Energy lost if summed to mono right now: 0.5*(L+R) vs the louder of L/R alone.
    l_ms, r_ms = np.mean(l_lo * l_lo), np.mean(r_lo * r_lo)
    stereo_energy_db = 20.0 * np.log10(max(float(np.sqrt(l_ms)), float(np.sqrt(r_ms))) + 1e-12)
    mono_energy_db = 20.0 * np.log10(float(np.sqrt(mono_ms)) + 1e-12)
    if l_std < 1e-9 or r_std < 1e-9 or negligible_low_band:
        corr = 1.0
    else:
        l_lo -= l_lo.mean()
        r_lo -= r_lo.mean()
        l_lo *= r_lo
        corr = float(np.clip(np.mean(l_lo) / (l_std * r_std), -1.0, 1.0))
    del l_lo, r_lo

    # Clamp: near-total cancellation drives log(~0) toward -inf, which reads
    # as a meaningless "risk" number past a point — cap at a still-alarming 60 dB.
    energy_at_risk = round(min(60.0, stereo_energy_db - mono_energy_db), 2)

    if corr < 0.3:
        verdict = "bass will noticeably thin out or cancel in mono"
    elif corr < 0.7:
        verdict = "some low-end loss on mono playback"
    else:
        verdict = "mono-safe low end"

    return {
        "correlation_below_cutoff": round(corr, 3),
        "cutoff_hz": cutoff_hz,
        "energy_at_risk_db": energy_at_risk,
        "verdict": verdict,
    }


# ---------------------------------------------------------------------------
# Noise floor: mains hum + broadband hiss
# ---------------------------------------------------------------------------

def analyze_noise_floor(mono: npt.NDArray, sr: int) -> dict[str, Any]:
    """Finds the quietest 0.5 s window (so program material doesn't mask the
    floor), then checks for 50/60 Hz hum tonality and reports broadband
    hiss level relative to full-track RMS."""
    win = max(1, int(0.5 * sr))
    n_win = len(mono) // win
    if n_win == 0:
        return {"noise_floor_dbfs": -90.0, "hum_detected": False, "hum_hz": 0.0,
                "hiss_headroom_db": 90.0, "verdict": "insufficient audio"}

    frames = mono[: n_win * win].reshape(n_win, win)
    rms_per_win = np.sqrt(np.mean(frames ** 2, axis=1) + 1e-20)
    quietest_idx = int(np.argmin(rms_per_win))
    quiet = frames[quietest_idx]

    noise_floor_dbfs = float(20.0 * np.log10(np.sqrt(np.mean(quiet ** 2)) + 1e-12))
    full_rms_dbfs = float(20.0 * np.log10(np.sqrt(np.mean(mono ** 2)) + 1e-12))
    hiss_headroom_db = round(full_rms_dbfs - noise_floor_dbfs, 2)

    # Hum check: energy concentration at 50/60 Hz (+ 2nd harmonic) vs the
    # quiet window's overall spectral energy.
    spec = np.abs(np.fft.rfft(quiet * np.hanning(len(quiet))))
    freqs = np.fft.rfftfreq(len(quiet), d=1.0 / sr)
    total_e = float(np.sum(spec ** 2)) + 1e-12

    hum_hz, hum_ratio = 0.0, 0.0
    for base in (50.0, 60.0):
        band_ratio = 0.0
        for harmonic in (1, 2):
            f = base * harmonic
            mask = (freqs >= f - 2.0) & (freqs <= f + 2.0)
            if np.any(mask):
                band_ratio += float(np.sum(spec[mask] ** 2)) / total_e
        if band_ratio > hum_ratio:
            hum_ratio, hum_hz = band_ratio, base

    hum_detected = hum_ratio > 0.05   # >5% of quiet-window energy at the mains tone

    if hum_detected:
        verdict = f"{int(hum_hz)} Hz mains hum present — a notch filter will help"
    elif hiss_headroom_db < 40.0:
        verdict = "elevated noise floor — check preamp gain / room treatment"
    else:
        verdict = "clean noise floor"

    return {
        "noise_floor_dbfs": round(noise_floor_dbfs, 2),
        "hum_detected": hum_detected,
        "hum_hz": hum_hz if hum_detected else 0.0,
        "hiss_headroom_db": hiss_headroom_db,
        "verdict": verdict,
    }


def analyze_signal_integrity(file_path: str, ctx=None) -> dict[str, Any]:
    from khensu_services.analysis.context import AnalysisContext

    # Shared decode cache when the caller has no context of its own.
    y, sr = (ctx if ctx is not None else AnalysisContext(file_path)).load(sr=None, mono=False)
    sr = int(sr)
    # Mono stays one channel (no duplicate-to-stereo): both checks accept it.
    mono = y.mean(axis=0) if y.ndim > 1 else y

    return {
        "true_peak": analyze_true_peak(y, sr),
        "mono_bass": analyze_mono_bass(y, sr),
        "noise_floor": analyze_noise_floor(mono, sr),
    }
