# local-services/khensu_services/analysis/quick_stats.py
"""Track Quick Stats aggregate (spec §2.2).

One call that returns every number the desktop's Track Quick Stats card
shows. Before this existed the card could only display BPM/key/genre (from
/analyze) because the level/format numbers lived across several analyzers
that the card never called. This module is deliberately aggregation, not new
DSP: it reuses the true-peak and LRA math that already ships and adds only
the cheap file/level facts.

Stateless; mirrors signal_integrity.py's numpy style.
"""
from __future__ import annotations

from typing import Any

import numpy as np
from khensu_services.analysis.context import load_audio


def _count_clip_events(y: np.ndarray) -> tuple[int, list[float]]:
    """Flat-top clip events (``loudness.clip_events``, the shared detector).

    Kept under this name for the existing callers (artifact engine, tests).
    Returns (event_count, first-10 event start positions as fractions 0..1)
    so the UI can mark where in the song clipping happens without shipping
    the whole sample map.
    """
    from khensu_services.analysis.loudness import clip_events
    return clip_events(y)


def analyze_quick_stats(file_path: str, ctx=None) -> dict[str, Any]:
    import soundfile as sf

    from khensu_services.analysis.dsp_metrics import stereo_width
    from khensu_services.analysis.loudness import (
        crest_factor_db, integrated_lufs, loudness_range,
    )
    from khensu_services.analysis.signal_integrity import analyze_true_peak

    info = sf.info(file_path)

    y, sr = (ctx.load(sr=None, mono=False) if ctx is not None
             else load_audio(file_path, sr=None, mono=False))
    sr = int(sr)
    # Mono stays ONE channel for BS.1770 metering (G = 1): duplicating it to
    # stereo would read +3 LU hot.
    mono = y.mean(axis=0) if y.ndim > 1 else y
    n = mono.shape[0]
    duration_sec = n / sr if sr > 0 else 0.0

    # --- Loudness (BS.1770 / Tech 3342) ---
    lufs = integrated_lufs(y, sr)
    lra = loudness_range(y, sr)

    # --- Peaks / dynamics ---
    tp = analyze_true_peak(y, sr)
    crest_db = crest_factor_db(y)   # per channel, max (see loudness.py)

    # --- Signal facts ---
    dc_offset = float(np.mean(mono)) if n else 0.0
    clip_events, clip_positions = _count_clip_events(y)

    # Stereo width (shared definition, same as the C++ meter): 0 % = mono,
    # 100 % = uncorrelated equal-power L/R (anti-phase clamps to 100 %).
    if y.ndim > 1 and y.shape[0] >= 2:
        width_pct = round(100.0 * stereo_width(y[0], y[1]), 1)
    else:
        width_pct = 0.0

    return {
        "duration_sec": round(duration_sec, 3),
        "sample_rate_hz": sr,
        "bit_depth": info.subtype or "unknown",
        "channels": int(info.channels),
        "format": info.format or "unknown",
        # -70 LUFS (the BS.1770 absolute gate) for silence / < 400 ms audio.
        "integrated_lufs": round(lufs, 2) if lufs is not None else -70.0,
        "loudness_range_lu": round(lra, 2) if lra is not None else None,
        "true_peak_dbtp": tp["true_peak_dbtp"],
        "sample_peak_dbfs": tp["sample_peak_dbfs"],
        "crest_factor_db": round(crest_db, 2) if crest_db is not None else None,
        "dc_offset": round(dc_offset, 6),
        "stereo_width_pct": width_pct,
        "clipping_event_count": clip_events,
        "clipping_positions": clip_positions,  # fractions of duration, first 10
    }
