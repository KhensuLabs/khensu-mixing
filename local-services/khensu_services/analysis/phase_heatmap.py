# local-services/khensu_services/analysis/phase_heatmap.py
"""Full-spectrum phase-correlation heatmap.

Generalizes the sub-bass phase view to the whole spectrum: a time x frequency
grid of L/R correlation (-1 = anti-phase / will cancel in mono, +1 = mono-safe,
0 = decorrelated). Shows exactly where (frequency) and when (time) a mix goes
out of phase — the mastering view for mono-compatibility problems that a single
correlation number hides. Bounded frame count. numpy-only, stateless.

Every STFT frame contributes (DSP-16): when the track has more frames than
``target_frames``, each output column pools a contiguous run of frames and
reports the energy-weighted correlation over that run
(``Re sum(L conj R) / sqrt(sum|L|^2 * sum|R|^2)``) instead of point-sampling
one frame in ~20. Cells with no energy (digital silence, or a band with
nothing in it) are ``None`` - not 0.0, which reads as "decorrelated".
"""
from __future__ import annotations

from typing import Any

import numpy as np

# A cell whose L*R band energy is below this fraction of the loudest cell's
# (-100 dB) has no meaningful phase - it is reported as None.
_SILENT_REL = 1e-10


def analyze_phase_heatmap(file_path: str, n_bands: int = 28, target_frames: int = 300,
                          n_fft: int = 4096) -> dict[str, Any]:
    from khensu_services.analysis.context import AnalysisContext
    from khensu_services.analysis.dsp_metrics import (
        _bincount_rows, band_bin_index, merge_log_band_edges, pool_frames,
        stft_batches, stft_frame_count,
    )

    y, sr = AnalysisContext(file_path).load(sr=None, mono=False)   # shared decode cache
    sr = int(sr)
    y = np.asarray(y)
    x = y if y.ndim == 2 else y[np.newaxis, :]
    duration = x.shape[1] / sr if sr else 0.0
    nyq = sr / 2.0
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / sr)
    # Low geomspace bands are narrower than one FFT bin; an empty band used
    # to read as 0.0 ("decorrelated") — a fake phase problem. Merge adjacent
    # bands until each owns >= 1 bin, so n_bands may come back smaller than
    # requested (band_edges_hz is authoritative).
    edges = merge_log_band_edges(np.geomspace(20.0, nyq, n_bands + 1), freqs)
    n_bands = len(edges) - 1
    edges_list = [round(float(e), 1) for e in edges]

    if x.shape[0] < 2:   # mono: perfectly correlated everywhere
        return {"duration_sec": round(float(duration), 2), "n_bands": n_bands,
                "band_edges_hz": edges_list, "n_frames": 0, "grid": [], "is_stereo": False}

    hop = n_fft // 2
    band_of_bin, valid = band_bin_index(freqs, edges)
    n = x.shape[1]
    if n < n_fft:
        return {"duration_sec": round(float(duration), 2), "n_bands": n_bands,
                "band_edges_hz": edges_list, "n_frames": 0, "grid": [], "is_stereo": True}
    n_frames = stft_frame_count(n, n_fft, hop)
    # Per frame and band: |L|^2, |R|^2 and Re(L conj R), summed over the band.
    acc = np.empty((3, n_frames, n_bands))
    for (f0, lf), (_, rf) in zip(stft_batches(x[0], n_fft, hop), stft_batches(x[1], n_fft, hop)):
        rows = slice(f0, f0 + lf.shape[0])
        acc[0, rows] = _bincount_rows(lf.real ** 2 + lf.imag ** 2, band_of_bin, valid, n_bands)
        acc[1, rows] = _bincount_rows(rf.real ** 2 + rf.imag ** 2, band_of_bin, valid, n_bands)
        acc[2, rows] = _bincount_rows(lf.real * rf.real + lf.imag * rf.imag,
                                      band_of_bin, valid, n_bands)

    # Pool contiguous frames per output column (sums -> energy-weighted
    # correlation over the column's span).
    el = pool_frames(acc[0], target_frames, reduce="sum")
    er = pool_frames(acc[1], target_frames, reduce="sum")
    cross = pool_frames(acc[2], target_frames, reduce="sum")
    denom = np.sqrt(el * er)
    floor = max(1e-20, _SILENT_REL * float(denom.max())) if denom.size else 1e-20
    live = denom > floor
    corr = np.clip(np.divide(cross, denom, out=np.zeros_like(cross), where=live), -1.0, 1.0)

    grid = [[round(float(v), 3) if ok else None for v, ok in zip(row, live_row)]
            for row, live_row in zip(corr, live)]
    return {
        "duration_sec": round(float(duration), 2),
        "n_bands": n_bands,
        "band_edges_hz": edges_list,
        "n_frames": len(grid),
        "grid": grid,
        "is_stereo": True,
    }
