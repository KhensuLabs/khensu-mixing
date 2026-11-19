"""Butterworth filters, gain and band split used by the chain renderer (ENG-6, from dsp.py)."""
from __future__ import annotations

import numpy as np


# ---------------------------------------------------------------------------
# Butterworth high/low-pass (audit CPL-05). pedalboard's HighpassFilter and
# LowpassFilter are first-order (6 dB/oct): an 80 Hz "high-pass" was only
# -7 dB at 40 Hz and -12.3 dB at 20 Hz, leaving most sub rumble in place.
# Engineers mean 12-24 dB/oct, so the chain stages are Butterworth with a
# ``slope_db_per_oct`` (order = slope / 6). Causal (minimum-phase, like an
# analogue/console filter) so a chain still sounds like one; -3 dB at the
# cutoff for every order.
# ---------------------------------------------------------------------------
DEFAULT_FILTER_SLOPE_DB_PER_OCT = 12.0


def filter_order_for_slope(slope_db_per_oct: float | None) -> int:
    """Butterworth order for a slope in dB/oct (6 per order, 1..8)."""
    slope = DEFAULT_FILTER_SLOPE_DB_PER_OCT if slope_db_per_oct is None else float(slope_db_per_oct)
    if not np.isfinite(slope):
        slope = DEFAULT_FILTER_SLOPE_DB_PER_OCT
    return int(min(8, max(1, round(slope / 6.0))))


def butterworth_filter(audio: np.ndarray, sr: int, cutoff_frequency_hz: float,
                       btype: str = "high",
                       slope_db_per_oct: float | None = DEFAULT_FILTER_SLOPE_DB_PER_OCT) -> np.ndarray:
    """Causal Butterworth high/low-pass along the last axis.

    ``audio`` is (samples,) or (channels, samples). ``btype`` is "high" or
    "low". The cutoff is held below 0.45 x sr so a low-rate source never
    receives an unstable design.
    """
    from scipy.signal import butter, sosfilt

    if btype not in ("high", "low"):
        raise ValueError(f"butterworth_filter: btype must be 'high' or 'low', got {btype!r}")
    nyq = sr * 0.5
    cutoff = float(min(max(1.0, float(cutoff_frequency_hz)), sr * 0.45))
    sos = butter(filter_order_for_slope(slope_db_per_oct), cutoff / nyq, btype=btype, output="sos")
    return sosfilt(sos, np.asarray(audio, dtype=np.float64), axis=-1)


def _apply_highpass(audio: np.ndarray, sr: int, cutoff_frequency_hz: float = 50.0,
                    slope_db_per_oct: float = DEFAULT_FILTER_SLOPE_DB_PER_OCT) -> np.ndarray:
    return butterworth_filter(audio, sr, cutoff_frequency_hz, "high", slope_db_per_oct)


def _apply_lowpass(audio: np.ndarray, sr: int, cutoff_frequency_hz: float = 50.0,
                   slope_db_per_oct: float = DEFAULT_FILTER_SLOPE_DB_PER_OCT) -> np.ndarray:
    # pedalboard's LowpassFilter defaulted to 50 Hz too; the clamp lifts any
    # sub-500 Hz request to 500 Hz before it gets here.
    return butterworth_filter(audio, sr, cutoff_frequency_hz, "low", slope_db_per_oct)


def _apply_gain(audio: np.ndarray, sr: int, gain_db: float = 0.0) -> np.ndarray:
    """Plain gain (pedalboard ``Gain`` semantics), routed to ALL channels."""
    del sr
    return np.asarray(audio, dtype=np.float64) * (10.0 ** (float(gain_db) / 20.0))


def _band_split(audio: np.ndarray, sr: int, lo_hz: float, hi_hz: float) -> np.ndarray:
    """Zero-phase band-limit for multiband branches (Linkwitz-style via
    back-to-back Butterworth; zero-phase keeps summed bands comb-free)."""
    from scipy.signal import butter, sosfiltfilt
    nyq = sr * 0.5
    out = audio
    if lo_hz and lo_hz > 20.0:
        sos = butter(4, min(lo_hz, nyq * 0.9) / nyq, btype="high", output="sos")
        out = sosfiltfilt(sos, out, axis=-1)
    if hi_hz and hi_hz < nyq * 0.95:
        sos = butter(4, hi_hz / nyq, btype="low", output="sos")
        out = sosfiltfilt(sos, out, axis=-1)
    return out
