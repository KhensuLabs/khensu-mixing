# local-services/khensu_services/analysis/mid_side_spectrogram.py
"""Mid/Side spectrogram: two time x frequency grids, one for the center (mid)
and one for the sides. The mastering staple for spotting a lopsided image —
where the stereo width lives across frequency and time, and whether the center
is carrying (or losing) the low end. Bounded frame count for a small response.
Stateless, numpy-only.
"""
from __future__ import annotations

from typing import Any

import numpy as np
import numpy.typing as npt

# 10*log10(0 + 1e-12): the dB value of a cell with no energy at all.
_EMPTY_DB = -119.0


def _raw_grid_db(mono: npt.NDArray, sr: int, n_bands: int, target_frames: int,
                n_fft: int = 2048) -> tuple[npt.NDArray, npt.NDArray]:
    """Raw per-band energy in dB over time (NOT yet normalized), plus edges.

    Band power comes from the shared ``dsp_metrics.band_power_frames`` and,
    on a long track, each output column is the MEAN power of a contiguous run
    of frames (``pool_frames``) - every frame counts, instead of one frame in
    ~20 being point-sampled (DSP-16)."""
    from khensu_services.analysis.dsp_metrics import (
        band_power_frames, merge_log_band_edges, pool_frames,
    )

    nyq = sr / 2.0
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / sr)
    # Merge bin-less low bands (an empty band summed to 0 -> the -120 dB
    # epsilon, which then set the shared normalisation floor).
    edges = merge_log_band_edges(np.geomspace(20.0, nyq, n_bands + 1), freqs)
    grid = band_power_frames(mono, sr, edges, n_fft, max(1, n_fft // 2))
    grid = pool_frames(grid, target_frames, reduce="mean")
    return 10.0 * np.log10(grid + 1e-12), edges


def analyze_mid_side_spectrogram(file_path: str, n_bands: int = 32,
                                 target_frames: int = 300) -> dict[str, Any]:
    from khensu_services.analysis.context import AnalysisContext

    y, sr = AnalysisContext(file_path).load(sr=None, mono=False)   # shared decode cache
    sr = int(sr)
    x = np.asarray(y, dtype=np.float64)
    x = x if x.ndim == 2 else x[np.newaxis, :]
    if x.shape[0] < 2:
        mono = x[0]
        mid, side = mono, np.zeros_like(mono)
    else:
        left, right = x[0], x[1]
        mid = (left + right) * 0.5
        side = (left - right) * 0.5

    mid_db, edges = _raw_grid_db(mid, sr, n_bands, target_frames)
    side_db, _ = _raw_grid_db(side, sr, n_bands, target_frames)
    n_bands = len(edges) - 1   # after merging bin-less bands

    # Normalize BOTH grids against a SHARED dB range, so a near-silent channel
    # stays dark instead of self-normalizing to full scale. Cells at the
    # zero-energy epsilon (digital silence / an all-zero side channel) are
    # excluded from the range so they can't stretch it to -120 dB; they
    # clamp to 0 (dark).
    both = np.concatenate([mid_db.ravel(), side_db.ravel()]) if mid_db.size and side_db.size else np.array([0.0])
    live = both[both > _EMPTY_DB]
    ref = live if live.size else both
    lo, hi = float(np.min(ref)), float(np.max(ref))
    span = (hi - lo) if hi > lo else 1.0

    def _norm(gdb: npt.NDArray) -> list[list[float]]:
        n = np.clip((gdb - lo) / span, 0.0, 1.0) if gdb.size else gdb
        return [[round(float(v), 4) for v in row] for row in n]

    mid_frames = _norm(mid_db)
    side_frames = _norm(side_db)
    duration = x.shape[1] / sr if sr else 0.0
    edges_list = [round(float(e), 1) for e in edges]

    return {
        "duration_sec": round(float(duration), 2),
        "n_bands": n_bands,
        "band_edges_hz": edges_list,
        "n_frames": len(mid_frames),
        "mid_frames": mid_frames,
        "side_frames": side_frames,
    }
