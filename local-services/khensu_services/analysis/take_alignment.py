# local-services/khensu_services/analysis/take_alignment.py
"""Doubles / ad-lib timing alignment (VocAlign-style, onset based).

Two modes:

* ``align`` — onsets of each take are matched to the nearest reference onset
  within ``max_shift_ms``; the offset to that onset is the correction.
* ``quantize`` — the target is the nearest grid line. With an explicit
  ``bpm`` the grid is constant (spacing ``60/bpm * 4/grid_division`` s),
  phase-anchored at the reference's first beat (snapped to the nearest
  reference onset so the anchor carries onset-detector precision, not
  beat-frame quantisation). Without ``bpm`` the grid is built by
  subdividing each pair of consecutive *tracked* reference beats (each beat
  snapped to a nearby reference onset), so it follows the performance's
  tempo drift instead of extrapolating one period from the first beat.

Each take is cut at its own onsets (segment = onset to next onset) and every
segment is moved by ``strength * offset`` (0 when unmatched). The render
rebuilds a same-length file by placing the moved segments with short linear
crossfades that sit *before* each onset, so transients are never faded and a
zero shift reproduces the input exactly. Segments are never reordered and the
"before" region of a source segment is never read past the next onset (no
flams); a segment moved later leaves a short silence ahead of its onset.

Stateless; librosa for onset/beat detection, soundfile for the render.
"""
from __future__ import annotations

import math
import os
from typing import Any

import numpy as np
from khensu_services.analysis import _lean_spectral as _lean
from khensu_services.analysis.context import load_audio
import numpy.typing as npt

_XFADE_SEC = 0.005
_ONSET_HOP_SEC = 0.002          # ~2 ms onset resolution
_ONSET_N_FFT = 1024
_ANCHOR_SNAP_SEC = 0.060
_BEAT_SNAP_SEC = 0.030          # interior beats: tighter, so a nearby 16th can't capture them
VALID_GRID_DIVISIONS = (4, 8, 16, 32)


def _load(path: str) -> tuple[npt.NDArray, int]:
    from khensu_services.copilot.dsp import require_finite_audio

    y, sr = load_audio(path, sr=None, mono=False)
    require_finite_audio(y, "alignment source")
    if y.ndim == 1:
        y = y[np.newaxis, :]
    return y.astype(np.float64), int(sr)


def detect_onsets(mono: npt.NDArray, sr: int) -> npt.NDArray:
    """Onset times (s) at ~2 ms resolution."""
    import librosa

    if len(mono) < _ONSET_N_FFT:
        return np.zeros(0)
    hop = max(16, int(round(_ONSET_HOP_SEC * sr)))
    env = librosa.onset.onset_strength(y=mono.astype(np.float32), sr=sr,
                                       hop_length=hop, n_fft=_ONSET_N_FFT)
    return np.asarray(librosa.onset.onset_detect(
        onset_envelope=env, sr=sr, hop_length=hop, units="time"), dtype=float)


def _grid_params(ref_mono: npt.NDArray, sr: int, ref_onsets: npt.NDArray,
                 bpm: float | None) -> tuple[float, float, npt.NDArray]:
    """Return (bpm, anchor_sec, onset-snapped beat times) for quantize mode."""
    import librosa

    kwargs: dict[str, Any] = {"y": ref_mono.astype(np.float32), "sr": sr}
    if bpm:
        kwargs["bpm"] = float(bpm)
    tempo, beats = _lean.beat_track(**kwargs)
    beat_times = np.asarray(librosa.frames_to_time(np.asarray(beats), sr=sr), dtype=float)
    # Beat-fit tempo, not the quantised tempogram bin (AUDIT-015).
    from khensu_services.analysis._shared_analysis import tracked_tempo
    est = tracked_tempo(tempo, beat_times)
    used = float(bpm) if bpm else est
    if not math.isfinite(used) or used <= 0:
        raise ValueError("Could not estimate a tempo from the reference; pass bpm explicitly")
    if len(beat_times):
        anchor = float(beat_times[0])
        if len(ref_onsets):
            near = float(ref_onsets[np.argmin(np.abs(ref_onsets - anchor))])
            if abs(near - anchor) <= _ANCHOR_SNAP_SEC:
                anchor = near
        snapped = beat_times.copy()
        snapped[0] = anchor
        if len(ref_onsets):
            for i in range(1, len(snapped)):
                near = float(ref_onsets[np.argmin(np.abs(ref_onsets - snapped[i]))])
                if abs(near - snapped[i]) <= _BEAT_SNAP_SEC:
                    snapped[i] = near
        beat_times = np.unique(snapped)
    elif len(ref_onsets):
        anchor = float(ref_onsets[0])
    else:
        anchor = 0.0
    return used, anchor, beat_times


def _beat_subdivided_grid(beat_times: npt.NDArray, grid_division: int) -> npt.NDArray | None:
    """Grid lines from splitting every tracked beat interval into
    ``grid_division / 4`` equal parts; extended back to t=0 with the first
    interval. None when fewer than two beats were tracked."""
    if len(beat_times) < 2:
        return None
    sub = max(1, grid_division // 4)
    pieces = [left + (right - left) * np.arange(sub) / sub
              for left, right in zip(beat_times[:-1], beat_times[1:])]
    step0 = float(beat_times[1] - beat_times[0]) / sub
    n_before = int(math.ceil(float(beat_times[0]) / step0)) if step0 > 0 else 0
    head = beat_times[0] - step0 * np.arange(n_before, 0, -1)
    return np.unique(np.concatenate([head, *pieces, beat_times[-1:]]))


def _nearest_on_grid(t: float, grid: npt.NDArray) -> float:
    """Nearest grid line; beyond either end, continue with the edge spacing."""
    if t <= grid[0] or t >= grid[-1]:
        edge, step = (float(grid[0]), float(grid[1] - grid[0])) if t <= grid[0]             else (float(grid[-1]), float(grid[-1] - grid[-2]))
        return edge + round((t - edge) / step) * step if step > 0 else edge
    i = int(np.searchsorted(grid, t))
    lo, hi = float(grid[i - 1]), float(grid[i])
    return lo if t - lo <= hi - t else hi


def _target_offsets(take_onsets: npt.NDArray, mode: str, ref_onsets: npt.NDArray,
                    grid_sec: float, anchor: float, max_shift: float,
                    grid: npt.NDArray | None = None) -> list[float | None]:
    offsets: list[float | None] = []
    for t in take_onsets:
        if mode == "quantize":
            if grid is not None and len(grid) >= 2:
                target = _nearest_on_grid(float(t), grid)
            else:
                target = anchor + round((t - anchor) / grid_sec) * grid_sec
        else:
            if len(ref_onsets) == 0:
                offsets.append(None)
                continue
            target = float(ref_onsets[np.argmin(np.abs(ref_onsets - t))])
        off = float(target - t)
        offsets.append(off if abs(off) <= max_shift else None)
    return offsets


def _render_segments(y: npt.NDArray, src: list[int], dst: list[int], xf: int) -> npt.NDArray:
    """Place source pieces (cut at ``src`` onsets) so each onset lands at its
    ``dst`` sample. ``src[0] == dst[0] == 0`` is the leading piece."""
    n = y.shape[1]
    out = np.zeros_like(y)
    k_last = len(src) - 1
    for k in range(len(src)):
        lead = xf if k > 0 else 0
        d0 = max(0, dst[k] - lead)
        s0 = src[k] - (dst[k] - d0)
        if s0 < 0:
            d0 -= s0
            s0 = 0
        d1 = dst[k + 1] if k < k_last else n
        length = d1 - d0
        if length <= 0:
            continue
        src_cap = src[k + 1] if k < k_last else n
        avail = max(0, min(length, src_cap - s0, n - s0))
        w = np.ones(length)
        if k > 0:
            f = min(xf, length // 2, dst[k] - d0)
            if f > 0:
                w[:f] *= (np.arange(f) + 0.5) / f
        if k < k_last:
            f = min(xf, length // 2)
            if f > 0:
                w[length - f:] *= 1.0 - (np.arange(f) + 0.5) / f
        if avail < length:
            w[avail:] = 0.0
            f = min(xf, avail)
            if f > 0:
                w[avail - f:avail] *= 1.0 - (np.arange(f) + 0.5) / f
        chunk = np.zeros((y.shape[0], length))
        chunk[:, :avail] = y[:, s0:s0 + avail]
        out[:, d0:d1] += chunk * w[np.newaxis, :]
    return out


def _process_take(path: str, mode: str, ref_onsets: npt.NDArray, grid_sec: float,
                  anchor: float, strength: float, max_shift: float,
                  rendered_path: str | None,
                  grid: npt.NDArray | None = None) -> dict[str, Any]:
    y, sr = _load(path)
    n = y.shape[1]
    xf = max(1, int(round(_XFADE_SEC * sr)))
    onsets = detect_onsets(y.mean(axis=0), sr)
    # Keep onsets far enough from the file edges for the crossfades.
    onsets = onsets[(onsets * sr >= 2 * xf) & (onsets * sr <= n - 2 * xf)]
    offsets = _target_offsets(onsets, mode, ref_onsets, grid_sec, anchor, max_shift, grid)

    src = [0] + [int(round(t * sr)) for t in onsets]
    dst = [0]
    applied: list[float] = []
    for i, off in enumerate(offsets):
        want = src[i + 1] + int(round(strength * off * sr)) if off is not None else src[i + 1]
        lo, hi = dst[-1] + 2 * xf, n - 2 * xf
        if lo > hi:
            # No room left for this boundary (and its crossfades) before the
            # end of the file: the old max(lo, hi) clamp pushed it past
            # n - 2*xf. Drop it and every later onset instead.
            break
        d = min(max(want, lo), hi)
        dst.append(d)
        applied.append((d - src[i + 1]) / sr)
    kept = len(applied)
    onsets, offsets, src = onsets[:kept], list(offsets)[:kept], src[:kept + 1]

    before = [abs(off) for off in offsets if off is not None]
    after = [abs(off - sh) for off, sh in zip(offsets, applied) if off is not None]
    duration = n / sr
    segments = []
    for i, t in enumerate(onsets):
        end = float(onsets[i + 1]) if i + 1 < len(onsets) else duration
        segments.append({"start_sec": round(float(t), 4), "end_sec": round(end, 4),
                         "shift_ms": round(applied[i] * 1e3, 2)})

    rendered: str | None = None
    if rendered_path:
        import soundfile as sf
        from khensu_services.copilot.dsp import _atomic_render_write

        out_audio = _render_segments(y, src, dst, xf)
        rendered = rendered_path
        _atomic_render_write(rendered, lambda tmp: sf.write(
            tmp, out_audio.T, sr, format="WAV", subtype="FLOAT"))

    return {
        "path": path,
        "segments": segments,
        "mean_abs_offset_ms_before": round(float(np.mean(before)) * 1e3, 2) if before else 0.0,
        "mean_abs_offset_ms_after": round(float(np.mean(after)) * 1e3, 2) if after else 0.0,
        "rendered_path": rendered,
    }


def render_suffix(mode: str) -> str:
    return "quantized" if mode == "quantize" else "aligned"


def align_takes(reference_path: str, take_paths: list[str], mode: str = "align",
                bpm: float | None = None, grid_division: int = 16,
                strength: float = 1.0, max_shift_ms: float = 80.0,
                render: bool = True,
                output_paths: list[str] | None = None) -> dict[str, Any]:
    """``output_paths`` (one per take) overrides the default
    ``<take>_aligned.wav`` / ``<take>_quantized.wav``; the router passes
    paths already validated by ``resolve_render_output``."""
    if mode not in ("align", "quantize"):
        raise ValueError(f"Unknown mode {mode!r}")
    ref, ref_sr = _load(reference_path)
    ref_mono = ref.mean(axis=0)
    ref_onsets = detect_onsets(ref_mono, ref_sr)

    grid_sec, anchor, used_bpm = 0.0, 0.0, bpm
    grid: npt.NDArray | None = None
    if mode == "quantize":
        used_bpm, anchor, beat_times = _grid_params(ref_mono, ref_sr, ref_onsets, bpm)
        grid_sec = 60.0 / used_bpm * 4.0 / grid_division
        if not bpm:
            # No explicit tempo: follow the tracked beats (tempo drift) rather
            # than extrapolating a constant period from the first beat.
            grid = _beat_subdivided_grid(beat_times, grid_division)

    strength = float(np.clip(strength, 0.0, 1.0))
    max_shift = max(0.0, float(max_shift_ms)) * 1e-3
    if not render:
        outs: list[str | None] = [None] * len(take_paths)
    elif output_paths is not None:
        outs = list(output_paths)
    else:
        suffix = render_suffix(mode)
        outs = [os.path.splitext(p)[0] + f"_{suffix}.wav" for p in take_paths]
    takes = [_process_take(p, mode, ref_onsets, grid_sec, anchor, strength,
                           max_shift, out, grid) for p, out in zip(take_paths, outs)]
    return {"mode": mode,
            "bpm": round(float(used_bpm), 3) if used_bpm else None,
            "takes": takes}
