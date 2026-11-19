# local-services/khensu_services/analysis/streaming_normalization.py
"""Streaming loudness normalization preview.

Every major platform plays tracks back at its own target loudness, turning
loud masters DOWN (and, on some services, quiet ones UP through a limiter).
So the master's own LUFS is not how it will actually sound. This computes, per
platform: the gain the platform applies, the resulting perceived loudness, the
true peak after that gain, and the headroom left below the platform's ceiling —
so "will Spotify turn me down 3 dB and will my true peak still be safe" is a
number, not a guess.

Reuses loudness + signal_integrity.analyze_true_peak. Stateless.

The platform list is NOT a table of its own any more (DSP-08 / S6): targets
and ceilings come from ``delivery_targets.DELIVERY_TARGETS`` (the one platform
table) and the turn-up policy from ``delivery_targets.playback_platforms()``.
Ids are the delivery ids (``apple_music``, ``amazon_music``); the old short
ids still resolve through ``platform_spec``.
"""
from __future__ import annotations

from typing import Any, Optional

import numpy as np

from khensu_services.analysis.delivery_targets import canonical_platform_id, playback_platforms
from khensu_services.analysis.loudness import integrated_lufs
from khensu_services.analysis.signal_integrity import analyze_true_peak

# Per platform: id, name, target_lufs, ceiling_dbtp, turns_up, peak_capped,
# limited (see delivery_targets._PLAYBACK). Shared with album_normalization
# and the compliance report, so every screen honours the same policy.
PLATFORMS: list[dict[str, Any]] = playback_platforms()
_PLATFORMS = PLATFORMS   # backward-compatible private alias
_BY_ID = {p["id"]: p for p in PLATFORMS}


def playback_policy(platform_id: str) -> Optional[dict[str, Any]]:
    """The platform's normalization spec, or None for a non-streaming id."""
    return _BY_ID.get(canonical_platform_id(platform_id))


def platform_spec(platform_id: str) -> dict[str, Any]:
    """The platform's normalization spec; unknown ids fall back to Spotify."""
    return playback_policy(platform_id) or PLATFORMS[0]


def normalization_gain_db(integrated_lufs_value: Optional[float], spec: dict[str, Any],
                          true_peak_dbtp: float | None = None) -> float:
    """Gain the platform applies: louder-than-target is always turned DOWN;
    quieter-than-target is turned UP only if the platform does so, and on a
    peak-capped platform only up to ``min(target - I, ceiling - TP)`` (never
    negative: a capped turn-up does nothing rather than turning a quiet,
    peaky track down). Without ``true_peak_dbtp`` the cap cannot be applied
    and the uncapped turn-up is returned. An unmeasurable loudness (``None``:
    silence or < 400 ms) gets 0 dB - there is nothing to normalise (DSP-09)."""
    if integrated_lufs_value is None or not np.isfinite(float(integrated_lufs_value)):
        return 0.0
    delta = float(spec["target_lufs"]) - float(integrated_lufs_value)
    if delta < 0.0:
        return delta
    if not spec["turns_up"]:
        return 0.0
    if spec.get("peak_capped") and true_peak_dbtp is not None:
        headroom = float(spec["ceiling_dbtp"]) - float(true_peak_dbtp)
        return max(0.0, min(delta, headroom))
    return delta


def analyze_streaming_normalization(file_path: str, ctx=None) -> dict[str, Any]:
    from khensu_services.analysis.context import AnalysisContext

    ctx = ctx if ctx is not None else AnalysisContext(file_path)   # shared decode cache
    y, sr = ctx.load(sr=None, mono=False)                          # (ch, n) or (n,)
    sr = int(sr)
    measured = integrated_lufs(y, sr)
    # A silent (or < 400 ms) file has no integrated loudness; it used to be
    # read as -70 LUFS and shown a +56 dB "turn-up". It now gets 0 dB on
    # every platform and is flagged not measurable. (DSP-09)
    measurable = measured is not None and np.isfinite(float(measured))
    integrated = float(measured) if measurable else -70.0

    tp = analyze_true_peak(y, sr)
    measured_tp = float(tp["true_peak_dbtp"])

    platforms: list[dict[str, Any]] = []
    for p in PLATFORMS:
        applied_gain = normalization_gain_db(measured if measurable else None, p, measured_tp)

        resulting_lufs = integrated + applied_gain
        resulting_tp = measured_tp + applied_gain
        # Only a limiting platform (Spotify Loud) pushes gain past the ceiling;
        # its limiter then holds the peak at the ceiling.
        limiter_engaged = (bool(p.get("limited")) and applied_gain > 0.0
                           and resulting_tp > p["ceiling_dbtp"])
        if limiter_engaged:
            resulting_tp = float(p["ceiling_dbtp"])
        headroom = p["ceiling_dbtp"] - resulting_tp

        platforms.append({
            "id": p["id"],
            "name": p["name"],
            "target_lufs": p["target_lufs"],
            "applied_gain_db": round(applied_gain, 2),
            "resulting_lufs": round(resulting_lufs, 2),
            "resulting_true_peak_dbtp": round(resulting_tp, 2),
            "headroom_db": round(headroom, 2),
            "turned_down": applied_gain < 0.0,
            "turned_up": applied_gain > 0.0,
            "limiter_engaged": limiter_engaged,
        })

    out: dict[str, Any] = {
        "integrated_lufs": round(integrated, 2),
        "true_peak_dbtp": round(measured_tp, 2),
        "measurable": bool(measurable),
        "platforms": platforms,
    }
    if not measurable:
        out["note"] = ("Integrated loudness is not measurable (silent or shorter than 400 ms); "
                       "no platform applies any gain.")
    return out
