"""Changed-region detection between two versions of a file (CE-PLT-2).

After a render, most of the track is often bit-identical to (or inaudibly
different from) the source: a de-esser that only fired in the verses, a fix
applied to one chorus, a clip-gain move. Re-measuring the whole render repeats
work on audio that did not change. This module finds the time ranges that did.

Method, streamed block by block (never both whole files in memory):

1. If the two files' bytes are identical (content hash), nothing changed.
2. Both files are read in ``block_ms`` blocks. A block whose PCM bytes are
   identical in both is unchanged (cheap: a byte compare, no arithmetic).
3. Otherwise the block's *difference energy* is measured: the RMS of
   ``b - a`` relative to full scale. A block is changed when that is above
   ``threshold_db`` (default -90 dBFS, well under 16-bit dither) - so a
   re-encode that only moved the last bit does not count as a change.
4. Changed blocks are merged across gaps shorter than ``merge_gap_ms`` and
   padded by ``pad_ms`` on each side (filters and limiters smear an edit a
   little past the samples that were touched).

Two files whose sample rate, channel count or length differ are not
comparable sample-for-sample (a resample, a re-layout, a time edit): the
whole duration is reported as changed with ``comparable=False`` and the
reason, so a caller falls back to a full re-measure instead of trusting a
partial one.
"""
from __future__ import annotations

from typing import Any

import numpy as np

DEFAULT_BLOCK_MS = 50.0
DEFAULT_THRESHOLD_DB = -90.0
DEFAULT_MERGE_GAP_MS = 250.0
DEFAULT_PAD_MS = 100.0


def _whole(duration: float, reason: str, method: str) -> dict[str, Any]:
    return {
        "comparable": False,
        "reason": reason,
        "method": method,
        "duration_sec": round(float(duration), 6),
        "changed_sec": round(float(duration), 6),
        "changed_fraction": 1.0 if duration > 0 else 0.0,
        "regions": [{"start_sec": 0.0, "end_sec": round(float(duration), 6)}] if duration > 0 else [],
        "max_difference_db": None,
    }


def merge_blocks(changed: list[int], block_len: int, sr: int, total_frames: int,
                 merge_gap_ms: float, pad_ms: float) -> list[tuple[float, float]]:
    """Changed block indices -> merged, padded (start_sec, end_sec) ranges."""
    if not changed:
        return []
    gap_blocks = int(np.ceil((merge_gap_ms / 1000.0) * sr / block_len))
    runs: list[list[int]] = []
    for idx in changed:
        if runs and idx - runs[-1][1] <= gap_blocks + 1:
            runs[-1][1] = idx
        else:
            runs.append([idx, idx])
    pad = int(round(pad_ms / 1000.0 * sr))
    out: list[tuple[float, float]] = []
    for first, last in runs:
        s = max(0, first * block_len - pad)
        e = min(total_frames, (last + 1) * block_len + pad)
        if out and s / sr <= out[-1][1]:
            out[-1] = (out[-1][0], e / sr)
        else:
            out.append((s / sr, e / sr))
    return out


def find_changed_regions(file_a: str, file_b: str, *,
                         block_ms: float = DEFAULT_BLOCK_MS,
                         threshold_db: float = DEFAULT_THRESHOLD_DB,
                         merge_gap_ms: float = DEFAULT_MERGE_GAP_MS,
                         pad_ms: float = DEFAULT_PAD_MS) -> dict[str, Any]:
    """Blocking. The time ranges where ``file_b`` differs from ``file_a``."""
    import soundfile as sf

    from khensu_services.common.analysis_cache import content_hash

    with sf.SoundFile(file_a) as fa, sf.SoundFile(file_b) as fb:
        sr_a, sr_b = int(fa.samplerate), int(fb.samplerate)
        dur_b = fb.frames / sr_b if sr_b > 0 else 0.0
        if sr_a != sr_b:
            return _whole(dur_b, f"sample rates differ ({sr_a} vs {sr_b} Hz)", "incomparable")
        if fa.channels != fb.channels:
            return _whole(dur_b, f"channel counts differ ({fa.channels} vs {fb.channels})", "incomparable")
        if fa.frames != fb.frames:
            return _whole(dur_b, f"lengths differ ({fa.frames} vs {fb.frames} frames)", "incomparable")

        sr = sr_b
        total = int(fb.frames)
        duration = total / sr if sr > 0 else 0.0

        try:
            if content_hash(file_a) == content_hash(file_b):
                return {"comparable": True, "reason": "", "method": "identical_bytes",
                        "duration_sec": round(duration, 6), "changed_sec": 0.0,
                        "changed_fraction": 0.0, "regions": [], "max_difference_db": None}
        except OSError:
            pass

        block_len = max(1, int(round(block_ms / 1000.0 * sr)))
        thresh_lin = 10.0 ** (threshold_db / 20.0)
        changed: list[int] = []
        max_diff = 0.0
        # Read ~1 s of blocks at a time: bounded memory, few Python iterations.
        blocks_per_read = max(1, int(sr // block_len))
        idx = 0
        while True:
            a = fa.read(block_len * blocks_per_read, dtype="float32", always_2d=True)
            b = fb.read(block_len * blocks_per_read, dtype="float32", always_2d=True)
            n = min(len(a), len(b))
            if n == 0:
                break
            a, b = a[:n], b[:n]
            for off in range(0, n, block_len):
                ba = a[off:off + block_len]
                bb = b[off:off + block_len]
                if ba.tobytes() != bb.tobytes():
                    d = (bb.astype(np.float64) - ba.astype(np.float64))
                    rms = float(np.sqrt(np.mean(d * d))) if d.size else 0.0
                    if rms > max_diff:
                        max_diff = rms
                    if rms > thresh_lin:
                        changed.append(idx)
                idx += 1

    regions = merge_blocks(changed, block_len, sr, total, merge_gap_ms, pad_ms)
    changed_sec = sum(e - s for s, e in regions)
    return {
        "comparable": True,
        "reason": "",
        "method": "block_difference",
        "duration_sec": round(duration, 6),
        "changed_sec": round(changed_sec, 6),
        "changed_fraction": round(changed_sec / duration, 6) if duration > 0 else 0.0,
        "regions": [{"start_sec": round(s, 6), "end_sec": round(e, 6)} for s, e in regions],
        "max_difference_db": round(20.0 * np.log10(max_diff), 2) if max_diff > 0 else None,
    }


def windows_touching(regions: list[dict[str, float]], n_windows: int,
                     block_sec: float, hop_sec: float) -> list[int]:
    """Indices of the windows k = [k*hop, k*hop + block) that overlap any
    changed region - the only windows a re-measure has to recompute."""
    out: list[int] = []
    for k in range(n_windows):
        ws, we = k * hop_sec, k * hop_sec + block_sec
        if any(r["start_sec"] < we and r["end_sec"] > ws for r in regions):
            out.append(k)
    return out


#: Audio read either side of a recomputed run so the curves' IIR filters
#: (K-weighting, the harshness band-passes) have settled before the first
#: kept window - the same values a whole-file pass gives, to ~0.01 dB.
_SETTLE_SEC = 2.0


def _runs(indices: list[int]) -> list[tuple[int, int]]:
    runs: list[tuple[int, int]] = []
    for k in indices:
        if runs and k == runs[-1][1] + 1:
            runs[-1] = (runs[-1][0], k)
        else:
            runs.append((k, k))
    return runs


def incremental_metric_timeline(baseline: dict[str, Any], render_path: str,
                                regions: list[dict[str, float]]) -> dict[str, Any] | None:
    """Post-render re-measure restricted to what changed: the render's
    ``/analyze/metric_timeline`` built from the source's timeline (windows
    that don't touch a changed region are copied) plus only the windows that
    do, recomputed on a short padded excerpt of the render.

    ``baseline`` is the source's metric-timeline result for the same
    block/hop; ``regions`` come from :func:`find_changed_regions` on a
    comparable pair. Returns None when the baseline doesn't fit the render
    (the caller then measures the whole file)."""
    import soundfile as sf

    from khensu_services.analysis.extended_analyzer import windowed_harshness_curve
    from khensu_services.analysis.loudness import (
        _windowed_crest_curve, as_channels, short_term_loudness_series,
    )
    from khensu_services.analysis.metric_timeline import _hotspot, _opt_round

    block_sec = float(baseline.get("block_sec") or 0.0)
    hop_sec = float(baseline.get("hop_sec") or 0.0)
    base_windows = list(baseline.get("windows") or [])
    n = int(baseline.get("n_windows") or len(base_windows))
    if block_sec <= 0 or hop_sec <= 0 or n != len(base_windows) or n == 0:
        return None

    info = sf.info(render_path)
    sr = int(info.samplerate)
    total = int(info.frames)
    block = max(1, int(round(block_sec * sr)))
    hop = max(1, int(round(hop_sec * sr)))
    expected = 1 if total < block else (total - block) // hop + 1
    if expected != n or total < block:
        return None

    dirty = windows_touching(regions, n, block_sec, hop_sec)
    windows = [dict(w) for w in base_windows]
    settle_hops = int(np.ceil(_SETTLE_SEC * sr / hop))
    for first, last in _runs(dirty):
        k0 = max(0, first - settle_hops)
        start = k0 * hop
        stop = min(total, last * hop + block + settle_hops * hop)
        seg, _ = sf.read(render_path, start=start, stop=stop, dtype="float32", always_2d=True)
        x = as_channels(seg.T)
        lufs = short_term_loudness_series(x, sr, block_sec, hop_sec)
        crest = _windowed_crest_curve(x, sr, block_sec, hop_sec)
        harsh = windowed_harshness_curve(x, sr, block_sec, hop_sec)
        for k in range(first, last + 1):
            j = k - k0
            if j >= min(len(lufs), len(crest), len(harsh)):
                return None   # grid mismatch: never publish a partial result
            lv = float(lufs[j]) if np.isfinite(lufs[j]) else None
            windows[k] = {
                "start_sec": round(k * hop_sec, 3),
                "end_sec": round(k * hop_sec + block_sec, 3),
                "loudness_lufs": _opt_round(lv, 2),
                "crest_db": _opt_round(crest[j], 2),
                "harshness": round(float(harsh[j]), 3),
            }

    return {
        "duration_sec": float(total / sr) if sr > 0 else 0.0,
        "block_sec": block_sec,
        "hop_sec": hop_sec,
        "n_windows": n,
        "windows": windows,
        "hotspots": {
            "loudest": _hotspot([w["loudness_lufs"] for w in windows], "max"),
            "most_squashed": _hotspot([w["crest_db"] for w in windows], "min"),
            "harshest": _hotspot([w["harshness"] for w in windows], "max"),
        },
        "recomputed_windows": len(dirty),
        "reused_windows": n - len(dirty),
    }
