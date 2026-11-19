# local-services/khensu_services/analysis/mono_compat.py
"""Cross-system mono-compatibility score.

Different playback systems sum different frequency ranges to mono: a club PA
and a vinyl cutter fold the low end, a phone / Bluetooth speaker folds the whole
signal. Anti-phase (decorrelated) content partially cancels when summed, so
"the bass vanished on the big system" is really per-band fold-down loss. This
reports per-band L/R correlation + mono fold-down loss (dB), then a 0–100
compatibility score per real-world system based on which bands it sums.

Reuses the masking band grid; scipy band-passing. Stateless.
"""
from __future__ import annotations

from typing import Any

import numpy as np
import scipy.signal as sig

from khensu_services.analysis.masking import _MASKING_BANDS

# Which band indices each system sums to mono AND can reproduce (indices into
# _MASKING_BANDS: 20-60, 60-120, 120-250, 250-500, 500-1k, 1k-2k, 2k-4k, 4k-8k,
# 8k-12k, 12k-20k). A fold-down loss in a band the system cannot play is not
# heard on it, so the phone and the Bluetooth speaker - both full mono sums -
# differ in range: a phone's micro-driver plays ~250 Hz-12 kHz, a Bluetooth
# speaker ~60 Hz-20 kHz. (They were identical profiles before.)
_SYSTEMS: list[dict[str, Any]] = [
    {"id": "club_pa",  "name": "Club PA (mono sub)",   "bands": [0, 1]},
    {"id": "vinyl",    "name": "Vinyl (mono low end)", "bands": [0, 1, 2]},
    {"id": "phone",    "name": "Phone speaker (mono)",  "bands": [3, 4, 5, 6, 7, 8]},
    {"id": "bt_mono",  "name": "Bluetooth mono speaker", "bands": [1, 2, 3, 4, 5, 6, 7, 8, 9]},
]


_CHUNK = 1 << 20


def _band_stats(sos: np.ndarray, left: np.ndarray, right: np.ndarray) -> tuple[float, float, float]:
    """(L/R correlation, folded-mono energy, mean channel energy) of one band.

    The band filter runs chunk by chunk with its state carried across chunks
    (the same output as one whole-signal ``sosfilt``), keeping only running
    sums: two float64 full-length copies per band were this route's peak
    (2026-11-18). The correlation follows ``dsp_metrics.stereo_correlation``:
    1.0 when both sides are silent, 0.0 when one is."""
    import scipy.signal as sig

    n = min(left.shape[0], right.shape[0])
    if n == 0:
        return 1.0, 0.0, 0.0
    zl = np.zeros((sos.shape[0], 2))
    zr = np.zeros((sos.shape[0], 2))
    sl = sr_ = sll = srr = slr = 0.0
    for i in range(0, n, _CHUNK):
        j = min(n, i + _CHUNK)
        lb, zl = sig.sosfilt(sos, np.asarray(left[i:j], dtype=np.float64), zi=zl)
        rb, zr = sig.sosfilt(sos, np.asarray(right[i:j], dtype=np.float64), zi=zr)
        sl += float(np.sum(lb))
        sr_ += float(np.sum(rb))
        sll += float(np.dot(lb, lb))
        srr += float(np.dot(rb, rb))
        slr += float(np.dot(lb, rb))
    ml, mr = sl / n, sr_ / n
    std_l = max(sll / n - ml * ml, 0.0) ** 0.5
    std_r = max(srr / n - mr * mr, 0.0) ** 0.5
    if std_l < 1e-9 and std_r < 1e-9:
        corr = 1.0
    elif std_l < 1e-9 or std_r < 1e-9:
        corr = 0.0
    else:
        corr = float(np.clip((slr / n - ml * mr) / (std_l * std_r), -1.0, 1.0))
    e_mono = (sll + srr + 2.0 * slr) / (4.0 * n)
    e_ref = (sll + srr) / (2.0 * n)
    return corr, e_mono, e_ref


def analyze_mono_compat(file_path: str) -> dict[str, Any]:
    from khensu_services.analysis.context import AnalysisContext

    y, sr = AnalysisContext(file_path).load(sr=None, mono=False)   # shared decode cache
    sr = int(sr)
    y = np.asarray(y)
    if y.ndim < 2 or y.shape[0] < 2:  # mono file: perfectly mono-compatible
        bands = [{"band_hz": [lo, hi], "correlation": 1.0, "fold_loss_db": 0.0}
                 for lo, hi in _MASKING_BANDS]
        systems = [{"id": s["id"], "name": s["name"], "score": 100, "worst_band_hz": None}
                   for s in _SYSTEMS]
        return {"is_stereo": False, "bands": bands, "systems": systems}

    left, right = y[0], y[1]

    band_reports: list[dict[str, Any]] = []
    for (lo, hi) in _MASKING_BANDS:
        hi = min(hi, sr / 2.0 - 1.0)
        if hi <= lo:
            band_reports.append({"band_hz": [lo, hi], "correlation": 1.0, "fold_loss_db": 0.0})
            continue
        sos = sig.butter(4, [lo, hi], btype="band", fs=sr, output="sos")
        corr, e_mono, e_ref = _band_stats(sos, left, right)
        fold_loss = 10.0 * np.log10((e_mono + 1e-20) / (e_ref + 1e-20))
        band_reports.append({"band_hz": [round(lo, 1), round(hi, 1)],
                             "correlation": round(corr, 3),
                             "fold_loss_db": round(float(fold_loss), 2)})

    systems: list[dict[str, Any]] = []
    for s in _SYSTEMS:
        losses = [band_reports[i]["fold_loss_db"] for i in s["bands"]]
        avg_loss = float(np.mean(losses)) if losses else 0.0
        worst_i = min(s["bands"], key=lambda i: band_reports[i]["fold_loss_db"])
        # Map average fold loss to 0–100: 0 dB loss = 100, -12 dB = 0.
        score = int(round(100.0 * max(0.0, min(1.0, 1.0 + avg_loss / 12.0))))
        systems.append({"id": s["id"], "name": s["name"], "score": score,
                        "worst_band_hz": band_reports[worst_i]["band_hz"],
                        "worst_band_loss_db": band_reports[worst_i]["fold_loss_db"]})

    return {"is_stereo": True, "bands": band_reports, "systems": systems}
