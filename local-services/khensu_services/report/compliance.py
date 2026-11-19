"""Delivery / compliance sheet.

One structured pass over a finished master that answers "is this ready to
ship?": integrated loudness + range, true peak, mono-bass compatibility, DC
offset, hard (flat-top) clipping, bit depth / sample rate, container metadata
hygiene, and how each streaming platform will normalise it. Every check gets a
pass / warn / fail verdict plus the measured number, and there's a markdown
one-pager for the delivery report.
"""
from __future__ import annotations

from typing import Any

import numpy as np
import soundfile as sf


def _lufs_and_lra(y_frames: np.ndarray, sr: int) -> tuple[float, float]:
    """Integrated LUFS (BS.1770-4) and EBU Tech 3342 loudness range, from the
    shared loudness core. NaN where not measurable (silence / < 400 ms)."""
    from khensu_services.analysis.loudness import integrated_lufs, loudness_range
    try:
        integrated = integrated_lufs(y_frames.T, sr)
        lra = loudness_range(y_frames.T, sr)
    except Exception:
        return float("nan"), float("nan")
    return (float(integrated) if integrated is not None else float("nan"),
            float(lra) if lra is not None else float("nan"))


def _check(status: str, value: Any, note: str) -> dict[str, Any]:
    return {"status": status, "value": value, "note": note}


def _read_metadata(file_path: str) -> dict[str, Any]:
    tags: dict[str, Any] = {"title": None, "artist": None, "album": None}
    try:
        import mutagen
        mf = mutagen.File(file_path, easy=True)
        if mf is not None and getattr(mf, "tags", None):
            for k in ("title", "artist", "album"):
                v = mf.tags.get(k)
                if v:
                    tags[k] = v[0] if isinstance(v, list) else str(v)
    except Exception:
        pass
    return tags


def build_compliance_sheet(file_path: str) -> dict[str, Any]:
    """Never raises on the DSP path. Returns per-check verdicts + a summary."""
    from khensu_services.analysis.signal_integrity import analyze_true_peak, analyze_mono_bass
    from khensu_services.copilot.platform_render import PLATFORM_TARGETS

    from khensu_services.analysis.context import load_audio
    decoded, sr = load_audio(file_path, sr=None, mono=False)  # the decode cache (REF-4)
    y = np.atleast_2d(decoded).T.astype(np.float64)           # (frames, channels)
    info = sf.info(file_path)
    bit_depth = next((int(b) for b in ("32", "24", "16", "8") if b in (info.subtype or "")), None)

    integrated, lra = _lufs_and_lra(y, sr)
    tp = float(analyze_true_peak(y.T, sr)["true_peak_dbtp"])
    mono = analyze_mono_bass(y.T, sr)
    mono_corr = float(mono.get("correlation_below_cutoff", 1.0))
    dc = float(np.max(np.abs(np.mean(y, axis=0))))          # worst-channel DC offset
    # CPL-23: flat-top run detection (the post-render guardrail's detector),
    # not a sample-level >= 0.9995 test - a legitimate 0 dBFS master has
    # isolated full-scale samples, a clipped one has flat-topped runs.
    from khensu_services.copilot.render_guardrails import count_flat_top_samples
    clipped = int(count_flat_top_samples(y.T))

    checks: dict[str, Any] = {}
    checks["true_peak_dbtp"] = _check(
        "fail" if tp > -0.1 else "warn" if tp > -1.0 else "pass",
        round(tp, 2), "Ceiling -1.0 dBTP for lossy-codec safety.")
    checks["dc_offset"] = _check(
        "fail" if dc > 0.003 else "warn" if dc > 0.001 else "pass",
        round(dc, 5), "DC offset wastes headroom and can click on edits.")
    checks["clipping"] = _check(
        "fail" if clipped > 0 else "pass", clipped, "Samples in hard-clipped (flat-topped) runs; inter-sample overs are judged by true peak.")
    checks["mono_bass"] = _check(
        "warn" if mono_corr < 0.5 else "pass", round(mono_corr, 3),
        "Low-band L/R correlation; <0.5 loses bass when summed to mono.")
    checks["bit_depth"] = _check(
        "warn" if (bit_depth is not None and bit_depth < 16) else "pass",
        bit_depth, "Delivery is usually >=16-bit.")
    checks["loudness_range_lu"] = _check(
        "pass", round(lra, 1) if np.isfinite(lra) else None,
        "EBU Tech 3342 loudness range (gated short-term P95 - P10).")

    meta = _read_metadata(file_path)
    missing = [k for k in ("title", "artist") if not meta.get(k)]
    checks["metadata"] = _check(
        "warn" if missing else "pass", meta,
        ("Missing " + ", ".join(missing)) if missing else "Title/artist present.")

    from khensu_services.analysis.streaming_normalization import (
        normalization_gain_db, playback_policy)

    platforms = []
    for pid, spec in PLATFORM_TARGETS.items():
        norm_gain = None
        if np.isfinite(integrated):
            policy = playback_policy(pid)   # alias-aware (apple -> apple_music)
            # Streaming platforms apply their published policy (turn-down only,
            # peak-capped turn-up); delivery targets without one get the plain
            # offset to target.
            norm_gain = (normalization_gain_db(integrated, policy, tp) if policy is not None
                         else spec["target_lufs"] - integrated)
        platforms.append({
            "platform_id": pid, "name": spec["name"],
            "target_lufs": spec["target_lufs"], "ceiling_dbtp": spec["ceiling_dbtp"],
            "normalization_gain_db": round(norm_gain, 1) if norm_gain is not None else None,
            "true_peak_ok": tp <= spec["ceiling_dbtp"],
        })

    fails = sum(1 for c in checks.values() if c["status"] == "fail")
    warns = sum(1 for c in checks.values() if c["status"] == "warn")
    overall = "fail" if fails else "warn" if warns else "pass"

    return {
        "file_path": file_path,
        "format": {"sample_rate": sr, "bit_depth": bit_depth,
                   "channels": int(y.shape[1]), "subtype": info.subtype},
        "integrated_lufs": round(integrated, 1) if np.isfinite(integrated) else None,
        "checks": checks,
        "platforms": platforms,
        "summary": {"overall": overall, "fail": fails, "warn": warns},
        "markdown": _to_markdown(file_path, integrated, checks, platforms, overall,
                                 _broadcast_results(y, sr)),
    }


def _broadcast_results(y_frames: np.ndarray, sr: int) -> list[dict[str, Any]]:
    """Broadcast / podcast target grading for the report text (never raises)."""
    from khensu_services.analysis.delivery_targets import evaluate, measure, targets_in
    try:
        meas = measure(y_frames.T, sr)
    except Exception:
        return []
    return [evaluate(meas, t) for t in targets_in("broadcast", "podcast")]


def _to_markdown(file_path: str, integrated: float, checks: dict[str, Any],
                 platforms: list[dict[str, Any]], overall: str,
                 broadcast: list[dict[str, Any]] | None = None) -> str:
    import os
    icon = {"pass": "OK", "warn": "!", "fail": "X"}
    lines = [f"# Delivery compliance — {os.path.basename(file_path)}",
             f"**Overall: {overall.upper()}**", "",
             f"Integrated loudness: {integrated:.1f} LUFS" if np.isfinite(integrated)
             else "Integrated loudness: n/a", "", "## Checks"]
    for name, c in checks.items():
        lines.append(f"- [{icon.get(c['status'], '?')}] {name}: {c['value']} — {c['note']}")
    lines += ["", "## Platform normalisation"]
    for p in platforms:
        g = p["normalization_gain_db"]
        lines.append(f"- {p['name']}: target {p['target_lufs']} LUFS, "
                     f"turned {'+' if (g or 0) >= 0 else ''}{g} dB, "
                     f"true-peak {'OK' if p['true_peak_ok'] else 'over ceiling'}")
    if broadcast:
        from khensu_services.analysis.delivery_targets import compliance_markdown
        lines += ["", "## Broadcast & podcast delivery"]
        lines += compliance_markdown(broadcast)
    return "\n".join(lines)
