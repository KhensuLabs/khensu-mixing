# local-services/khensu_services/analysis/reference_spectral_delta.py
"""Time-varying reference spectral-delta.

Today's Reference Compare gives one static match-EQ callout. This shows, across
YOUR track's timeline, how its instantaneous tonal balance deviates from the
reference's overall balance — a time x frequency delta grid (dB, +hot / -shy).
Because target and reference are different songs, we don't try to time-align
them: the reference contributes a single averaged tonal-balance profile, and the
target's per-frame profile is compared against it. numpy-only, stateless.

Both files are measured on ONE shared band-edge set (capped at 20 kHz and at
the lower of the two Nyquists, merged so every band holds bins on both FFT
grids) with the same FFT bin width in Hz, and averaged profiles are the dB of
mean power on both sides (AUDIT-007). Measuring each file on its own
``geomspace(20, sr/2)`` grid made band *b* cover different frequencies in a
44.1 kHz reference and a 48/96 kHz master, and invented match-EQ.
"""
from __future__ import annotations

from typing import Any

import numpy as np
import numpy.typing as npt

from khensu_services.analysis.dsp_metrics import merge_log_band_edges

_LOW_EDGE_HZ = 20.0
_TOP_EDGE_HZ = 20000.0
# One analysis bin width for every sample rate (4096-point FFT at 44.1 kHz).
# Equal bin spacing in Hz means a band selects the same bins - and the same
# window leakage - in both files, whatever their rates.
_BIN_HZ = 44100.0 / 4096.0


def _n_fft(sr: int) -> int:
    return max(256, int(round(sr / _BIN_HZ)))


def shared_band_edges(n_bands: int, sr_a: int, sr_b: int) -> npt.NDArray:
    """One log-spaced edge set valid for both rates' FFT grids."""
    top = min(_TOP_EDGE_HZ, sr_a / 2.0, sr_b / 2.0)
    edges = np.geomspace(_LOW_EDGE_HZ, top, n_bands + 1)
    for sr in (sr_a, sr_b):
        edges = merge_log_band_edges(edges, np.fft.rfftfreq(_n_fft(sr), d=1.0 / sr))
    return edges


def _band_power_frames(mono: npt.NDArray, sr: int, edges: npt.NDArray) -> npt.NDArray:
    """(frames, bands) linear band power, half-overlapped Hann frames (the
    shared ``dsp_metrics.band_power_frames``)."""
    from khensu_services.analysis.dsp_metrics import band_power_frames
    return band_power_frames(mono, sr, edges, _n_fft(sr))


def _balance_db(power: npt.NDArray) -> npt.NDArray:
    """Level-independent tonal balance: dB per band minus the band mean."""
    db = 10.0 * np.log10(power + 1e-12)
    return db - db.mean(axis=-1, keepdims=True)


def _avg_profile(mono: npt.NDArray, sr: int, edges: npt.NDArray) -> npt.NDArray:
    frames = _band_power_frames(mono, sr, edges)
    power = frames.mean(axis=0) if frames.shape[0] else np.zeros(len(edges) - 1)
    return _balance_db(power)   # dB of mean power


def _frame_profiles(mono: npt.NDArray, sr: int, edges: npt.NDArray,
                    target_frames: int) -> npt.NDArray:
    from khensu_services.analysis.dsp_metrics import pool_frames
    # Each column is the mean POWER of a contiguous run of frames (every frame
    # counts; DSP-16), then its tonal balance.
    return _balance_db(pool_frames(_band_power_frames(mono, sr, edges), target_frames))


def _load_mono(path: str) -> tuple[npt.NDArray, int]:
    # Through the process-wide decode cache (REF-4): the target is the
    # session's source, which the intake already decoded.
    from khensu_services.analysis.context import load_audio
    y, sr = load_audio(path, sr=None, mono=True)
    return y.astype(np.float64), int(sr)


def average_tonal_delta(target_path: str, reference_path: str,
                        n_bands: int = 28) -> dict[str, Any]:
    """Whole-file tonal-balance delta (target - reference, dB, +hot/-shy).

    Both sides are the dB of mean band power on the shared edges — the input
    the match-EQ needs. (Averaging the per-frame display grid instead would be
    a mean of dB on one side only: a Jensen bias that reads as fake EQ.)
    """
    tgt, srt = _load_mono(target_path)
    ref, srr = _load_mono(reference_path)
    edges = shared_band_edges(n_bands, srt, srr)
    delta = _avg_profile(tgt, srt, edges) - _avg_profile(ref, srr, edges)
    return {"band_edges_hz": [float(e) for e in edges],
            "delta_db": [float(v) for v in delta]}


def analyze_reference_spectral_delta(target_path: str, reference_path: str,
                                     n_bands: int = 28, target_frames: int = 300) -> dict[str, Any]:
    tgt, srt = _load_mono(target_path)
    ref, srr = _load_mono(reference_path)

    edges = shared_band_edges(n_bands, srt, srr)
    ref_profile = _avg_profile(ref, srr, edges)
    tgt_frames = _frame_profiles(tgt, srt, edges, target_frames)

    delta = tgt_frames - ref_profile[np.newaxis, :] if tgt_frames.size else tgt_frames
    duration = len(tgt) / srt if srt else 0.0

    return {
        "duration_sec": round(float(duration), 2),
        # Bands narrower than one FFT bin are merged, so this can be < requested.
        "n_bands": int(len(edges) - 1),
        "band_edges_hz": [round(float(e), 1) for e in edges],
        "n_frames": int(delta.shape[0]) if delta.size else 0,
        "grid": [[round(float(v), 2) for v in row] for row in delta],  # dB delta, +hot/-shy
    }
