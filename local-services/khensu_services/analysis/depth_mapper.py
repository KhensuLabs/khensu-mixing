# local-services/khensu_services/analysis/depth_mapper.py
"""Depth & Dimension Mapper: estimates each stem's front-to-back placement
from the psychoacoustic cues that create depth perception in a mix:

  wetness    — reverberant energy persisting in the gaps after phrases
               (distant sources carry more room than direct sound)
  darkness   — high-frequency rolloff (air absorbs highs; distant = darker)
  softness   — blunted transients (distance smears attacks)

Each cue is normalized 0-1 and combined into a depth score: 0 = right at
the listener's nose, 1 = back wall. Depth is the least-visualized mixing
domain — level and panning get meters, front-to-back gets guesswork.

Stateless, numpy/librosa-only (mirrors extended_analyzer.py's style).
"""
from __future__ import annotations

from typing import Any

import numpy as np
from khensu_services.analysis import _lean_spectral as _lean
import numpy.typing as npt
from khensu_services.analysis.dsp_metrics import onset_strength

_WIN_SEC = 0.050


def _wetness(mono: npt.NDArray, sr: int) -> float:
    """Gap-to-phrase energy ratio: how much sound persists when the source
    stops. Dry close-mic'd material drops to the noise floor instantly;
    reverberant material leaves a tail."""
    win = max(1, int(_WIN_SEC * sr))
    n_win = len(mono) // win
    if n_win < 8:
        return 0.0
    rms_db = 20.0 * np.log10(
        np.sqrt(np.mean(mono[: n_win * win].reshape(n_win, win) ** 2, axis=1) + 1e-20) + 1e-12)

    floor = float(np.percentile(rms_db, 10))
    loud = float(np.percentile(rms_db, 90))
    if loud - floor < 12.0:
        return 0.5   # continuous material — wetness unobservable, assume middle

    gate = floor + (loud - floor) * 0.5
    active = rms_db > gate

    # Collect the first 2 windows after each phrase end (the "tail" zone).
    tail_vals: list[float] = []
    for i in range(1, n_win - 2):
        if active[i - 1] and not active[i]:
            tail_vals.extend(rms_db[i : i + 2])
    if not tail_vals:
        return 0.0
    tail = float(np.mean(tail_vals))
    # Wetness = where the tail sits between the floor (0, bone dry) and the
    # phrase level, scaled so a tail 80 % of the way up that range (or more)
    # reads 1 (swimming) - a fraction of the floor-to-phrase range, not a
    # fixed dB distance.
    return float(np.clip((tail - floor) / max(loud - floor, 1e-6) / 0.8, 0.0, 1.0))


def _darkness(mono: npt.NDArray, sr: int) -> float:
    """Spectral-centroid-based: 5 kHz+ centroid = bright/close (0),
    <800 Hz centroid = dark/distant (1)."""
    centroid = float(np.mean(_lean.spectral_centroid(mono, sr)))
    return float(np.clip(1.0 - (np.log2(max(centroid, 100.0)) - np.log2(800.0))
                         / (np.log2(5000.0) - np.log2(800.0)), 0.0, 1.0))


def _softness(mono: npt.NDArray, sr: int) -> float:
    """Transient bluntness: strong, peaky onset envelope = close (0);
    smeared, flat onset envelope = distant (1)."""
    env = onset_strength(mono, sr)
    if env.size == 0 or float(np.max(env)) <= 0.0:
        return 0.5
    # Peak-to-mean of the onset envelope: percussive/close material is very
    # top-heavy; washed-out material is flat.
    ratio = float(np.max(env) / (np.mean(env) + 1e-9))
    return float(np.clip(1.0 - (ratio - 3.0) / 12.0, 0.0, 1.0))


def analyze_depth(stems: dict[str, str]) -> dict[str, Any]:
    """stems: {stem_name: file_path}. Returns per-stem depth + cues +
    placement suggestions."""
    from khensu_services.analysis.context import AnalysisContext

    results: list[dict[str, Any]] = []
    for name, path in stems.items():
        y, sr = AnalysisContext(path).load(sr=None, mono=True)   # shared decode cache
        sr = int(sr)
        rms = float(np.sqrt(np.mean(y ** 2) + 1e-20))
        if rms < 1e-5:   # silent stem — skip rather than reporting noise
            continue
        wet = _wetness(y, sr)
        dark = _darkness(y, sr)
        soft = _softness(y, sr)
        depth = float(np.clip(0.45 * wet + 0.30 * dark + 0.25 * soft, 0.0, 1.0))
        results.append({
            "stem": name,
            "depth": round(depth, 3),
            "wetness": round(wet, 3),
            "darkness": round(dark, 3),
            "softness": round(soft, 3),
        })

    # Placement commentary: depth is relational — a mix where everything sits
    # at the same distance is flat regardless of the absolute numbers.
    suggestions: list[str] = []
    if len(results) >= 2:
        depths = [r["depth"] for r in results]
        spread = max(depths) - min(depths)
        if spread < 0.15:
            suggestions.append(
                "All stems sit at nearly the same distance (spread "
                f"{spread:.2f}) — the mix reads as flat. Classic staging: lead "
                "vocal in front (dry, bright), pads and room behind it. Try "
                "more pre-delay/reverb on background elements or drying up the lead.")
        by_depth = sorted(results, key=lambda r: r["depth"])
        vocal = next((r for r in results if r["stem"] == "vocals"), None)
        if vocal is not None and by_depth[-1]["stem"] == "vocals" and spread >= 0.15:
            suggestions.append(
                "The vocal measures as the most distant element — usually the "
                "opposite of intent. Reduce vocal reverb level or add 30-60 ms "
                "pre-delay so the dry voice detaches from its tail.")

    return {"stems": results, "suggestions": suggestions}
