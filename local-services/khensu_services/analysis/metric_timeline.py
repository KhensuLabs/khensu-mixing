# local-services/khensu_services/analysis/metric_timeline.py
"""Unified per-window mix metric timeline (pipeline #6).

Generalizes the per-window localization pattern (first built for AI-artifact
confidence, `artifacts/engine._temporal_localization`) to the everyday mix
metrics, so the dashboard can answer "WHICH section is loud / squashed / harsh"
instead of showing one whole-track scalar. Aligns three same-windowed curves —
short-term loudness (LUFS), crest factor (dynamics), and harshness — into one
series, and flags the hotspot window for each so a caller can jump straight to
the worst spot. Mix-level (no stems required)."""
from __future__ import annotations

from typing import Any

import numpy as np


def _hotspot(values: list[float | None], want: str) -> dict[str, Any] | None:
    """The index/value of the most extreme window. `want`="max" for
    harshness/loudness, "min" for crest (least dynamic = most squashed).
    Silent windows (None) are skipped, never picked."""
    finite = [(i, float(v)) for i, v in enumerate(values)
              if v is not None and np.isfinite(v)]
    if not finite:
        return None
    pick = max if want == "max" else min
    idx, val = pick(finite, key=lambda iv: iv[1])
    return {"window_index": idx, "value": round(val, 3)}


def _opt_round(v: float | None, nd: int) -> float | None:
    return round(float(v), nd) if v is not None and np.isfinite(v) else None


def analyze_metric_timeline(file_path: str, block_sec: float = 3.0,
                            hop_sec: float = 1.0, ctx=None) -> dict[str, Any]:
    """Return aligned per-window {loudness_lufs, crest_db, harshness} plus a
    hotspot summary. `ctx` is an optional shared AnalysisContext (#1).

    All three curves use the same window grid (window k = [k*hop, k*hop +
    block)), one entry per window: a digitally silent window reports
    loudness_lufs / crest_db = None instead of being dropped, so window i
    is the same stretch of audio in every curve."""
    from khensu_services.analysis.extended_analyzer import windowed_harshness_curve
    from khensu_services.analysis.loudness import (
        _windowed_crest_curve, as_channels, short_term_loudness_series,
    )

    from khensu_services.analysis.context import AnalysisContext

    # Shared decode cache when the caller has no context of its own.
    y, sr = (ctx if ctx is not None else AnalysisContext(file_path)).load(sr=None, mono=False)
    sr = int(sr)
    x = as_channels(y)   # mono stays one channel (BS.1770 G = 1)

    lufs = [float(v) if np.isfinite(v) else None
            for v in short_term_loudness_series(x, sr, block_sec, hop_sec)]
    crest = _windowed_crest_curve(x, sr, block_sec, hop_sec)
    harsh = windowed_harshness_curve(x, sr, block_sec, hop_sec)

    # Same block/hop grid -> same length; min() is only a guard.
    n = min(len(lufs), len(crest), len(harsh))
    windows = [{
        "start_sec": round(i * hop_sec, 3),
        "end_sec": round(i * hop_sec + block_sec, 3),
        "loudness_lufs": _opt_round(lufs[i], 2),
        "crest_db": _opt_round(crest[i], 2),
        "harshness": round(float(harsh[i]), 3),
    } for i in range(n)]

    return {
        "duration_sec": float(x.shape[1] / sr) if sr > 0 else 0.0,
        "block_sec": float(block_sec),
        "hop_sec": float(hop_sec),
        "n_windows": n,
        "windows": windows,
        # "Which section" answers: the worst window per metric.
        "hotspots": {
            "loudest": _hotspot(lufs[:n], "max"),
            "most_squashed": _hotspot(crest[:n], "min"),
            "harshest": _hotspot(harsh[:n], "max"),
        },
    }
