"""One-click platform-target master render set.

Renders a finished master to each streaming/club target loudness, true-peak
safe, in a single action — Spotify -14, Apple -16, club -9, YouTube -14 by
default, each capped at its own true-peak ceiling from the shared delivery
table (``analysis.delivery_targets``). Every render is loudness-normalised to the platform target
and then guaranteed under the true-peak ceiling, and each is run through the
existing post-render guardrail so a target that damages the master (clipping,
mono-bass collapse, crushed dynamics) is flagged rather than shipped silently.

DSP order per target:
  1. Measure integrated LUFS (shared BS.1770 meter, layout-aware). A source
     whose loudness cannot be measured (silence, too short, unsupported
     layout) fails loudly — it is never treated as "0 dB of gain".
  2. Pre-gain to the target LUFS.
  3. ``tp_limiter.true_peak_limit`` to the platform ceiling (4x-oversampled,
     lookahead, sample-aligned). pedalboard's ``Limiter`` is a maximiser with
     a 0 dBFS hard clip, not a ceiling, and missed targets by several LU
     (AUDIT-006).
  4. Limiting removes loudness, so the makeup is re-measured and re-applied a
     few times; a render still more than ``TARGET_TOLERANCE_LU`` from target
     is reported as ``could_not_reach`` instead of silently shipping short.
"""
from __future__ import annotations

import logging
import os
from typing import Any, Callable, Optional

import numpy as np
import soundfile as sf
from khensu_services.common.job_status import JobStatus

_log = logging.getLogger(__name__)

def _load_platform_targets() -> dict[str, dict[str, Any]]:
    """The render targets, from THE delivery table (CPL-18 / DSP-08):
    ``analysis.delivery_targets.platform_master_targets()``. Canonical ids
    only; legacy ids (``apple``, ``amazon``) resolve through
    ``resolve_platform_id``. This module used to keep its own four-entry
    table, which disagreed with the delivery table (Amazon -2 vs -1 dBTP, club
    ceiling) and could not render Tidal, Amazon or Deezer at all."""
    from khensu_services.analysis.delivery_targets import PLATFORM_ALIASES, platform_master_targets
    return {pid: {"name": t["label"], "target_lufs": t["lufs"], "ceiling_dbtp": t["true_peak_dbtp"]}
            for pid, t in platform_master_targets().items() if pid not in PLATFORM_ALIASES}


PLATFORM_TARGETS: dict[str, dict[str, Any]] = _load_platform_targets()


def resolve_platform_id(platform_id: str) -> str | None:
    """Canonical target id for ``platform_id`` (legacy aliases accepted), or
    None when it is not a renderable target."""
    from khensu_services.analysis.delivery_targets import PLATFORM_ALIASES
    pid = PLATFORM_ALIASES.get(platform_id, platform_id)
    return pid if pid in PLATFORM_TARGETS else None


# The one-click default set (legacy ids kept so output names don't change).
DEFAULT_PLATFORMS = ["spotify", "apple", "club", "youtube"]

# A master more than this far from its target counts as a miss.
TARGET_TOLERANCE_LU = 0.5
# Pre-gain -> limit -> re-measure passes before giving up on the target.
_MAKEUP_PASSES = 4


def _integrated_lufs(y_frames_ch: np.ndarray, sr: int) -> float:
    """Integrated LUFS for a (frames, channels) buffer (shared BS.1770 meter).
    NaN for silence / too-short audio; an unsupported layout raises."""
    from khensu_services.analysis.loudness import integrated_lufs
    v = integrated_lufs(np.asarray(y_frames_ch).T, sr)
    return float(v) if v is not None else float("nan")


def _true_peak_dbtp(y_frames_ch: np.ndarray, sr: int) -> float:
    from khensu_services.analysis.signal_integrity import analyze_true_peak
    return float(analyze_true_peak(y_frames_ch.T, sr)["true_peak_dbtp"])


# Brickwall gain reduction beyond this is reported as a warning on the entry:
# a -20 LUFS source pushed to the -9 LUFS club target needs ~11 dB of it.
MAX_GAIN_REDUCTION_WARN_DB = 6.0


def _max_gain_reduction_db(driven: np.ndarray, limited: np.ndarray) -> float:
    """Peak gain reduction the true-peak limiter applied (dB, >= 0), from the
    linked (max-over-channels) level before and after it (audit CPL-02)."""
    a = np.max(np.abs(np.asarray(driven)), axis=1) if np.ndim(driven) > 1 else np.abs(driven)
    b = np.max(np.abs(np.asarray(limited)), axis=1) if np.ndim(limited) > 1 else np.abs(limited)
    mask = a > 1e-6
    if not mask.any():
        return 0.0
    ratio = np.maximum(b[mask], 1e-12) / a[mask]
    return float(max(0.0, -20.0 * np.log10(float(np.min(ratio)))))


def render_platform_master(src_path: str, out_path: str, *,
                           target_lufs: float, ceiling_dbtp: float,
                           keep_loudness: bool = False) -> dict[str, Any]:
    """Render one true-peak-safe master. Blocking.

    By default (CUT-9 "safety variant", ``keep_loudness``) the master keeps
    its own loudness and is only made safe for the platform's true-peak
    ceiling; ``keep_loudness=False`` also brings it to the platform's
    loudness target.

    Raises ``ValueError`` when the source loudness cannot be measured.
    """
    from khensu_services.copilot.dsp import require_finite_audio
    from khensu_services.copilot.tp_limiter import true_peak_limit

    y, sr = sf.read(src_path, always_2d=True)      # (frames, channels), float64
    require_finite_audio(y, "platform-master source")  # audit CQ-01
    measured = _integrated_lufs(y, sr)
    if not np.isfinite(measured):
        # AUDIT-035: NaN used to become "gain 0" and an unnormalised file was
        # written and reported as a platform master.
        raise ValueError(
            "Cannot measure the source's integrated loudness (silent, shorter "
            "than 400 ms, or no audio above the -70 LUFS gate) - no master rendered.")

    if keep_loudness:
        target_lufs = float(measured)  # the target is the master itself
    gain_db = float(target_lufs - measured)
    limited = y
    driven = y
    # A safety variant limits once and never pushes back up into the limiter:
    # the loudness it loses to the ceiling is the price of being peak-safe.
    for _ in range(1 if keep_loudness else _MAKEUP_PASSES):
        driven = y * (10.0 ** (gain_db / 20.0))
        # Turn-downs stay under the ceiling and pass through untouched.
        limited = true_peak_limit(driven.T, sr, ceiling_dbtp=float(ceiling_dbtp)).T
        achieved = _integrated_lufs(limited, sr)
        if not np.isfinite(achieved):
            break
        shortfall = float(target_lufs - achieved)
        if abs(shortfall) <= TARGET_TOLERANCE_LU / 4.0:
            break
        gain_db += shortfall

    max_gr_db = _max_gain_reduction_db(driven, limited)

    # 24-bit PCM (soundfile's default for .wav is undithered 16-bit, whose
    # requantisation noise is audible on quiet fades), written atomically.
    # Undithered: at 24 bits the requantisation error sits ~144 dB down.
    from khensu_services.copilot.dsp import _atomic_render_write
    _atomic_render_write(out_path, lambda tmp: sf.write(tmp, limited, sr, subtype="PCM_24"))

    # Report what was actually WRITTEN (post-quantisation), not the float
    # buffer: the achieved numbers are the file's numbers.
    written, _ = sf.read(out_path, always_2d=True)
    final_lufs = _integrated_lufs(written, sr)
    final_tp = _true_peak_dbtp(written, sr)
    miss = float(final_lufs - target_lufs) if np.isfinite(final_lufs) else None
    return {
        "output_path": out_path,
        "gain_db": round(gain_db, 2),
        "max_gain_reduction_db": round(max_gr_db, 2),
        "achieved_lufs": round(final_lufs, 2) if np.isfinite(final_lufs) else None,
        "achieved_true_peak_dbtp": round(final_tp, 2) if np.isfinite(final_tp) else None,
        "target_lufs": target_lufs,
        "ceiling_dbtp": ceiling_dbtp,
        "loudness_miss_lu": round(miss, 2) if miss is not None else None,
        # A safety variant asked only for peak safety, so it is "reached" once
        # the file is written; loudness_miss_lu says what the ceiling cost.
        "target_reached": miss is not None and (keep_loudness or abs(miss) <= TARGET_TOLERANCE_LU),
    }


def _out_name(src_path: str, out_dir: str, platform_id: str) -> str:
    base = os.path.splitext(os.path.basename(src_path))[0]
    return os.path.join(out_dir, f"{base}_{platform_id}.wav")


def render_platform_set(src_path: str, out_dir: str,
                        platform_ids: Optional[list[str]] = None,
                        progress_cb: Optional[Callable[[str, int, int], bool]] = None,
                        match_loudness: bool = False) -> list[dict[str, Any]]:
    """Render every requested platform master through the shared render
    pipeline (audit CPL-02 / S2): each master is rendered into quarantine,
    judged by the post-render guardrail, and only promoted to
    ``<src>_<platform>.wav`` when it passes. A failed guardrail is status
    ``rejected`` (the file stays in quarantine for the expert override), never
    "completed". Returns one result dict per target. progress_cb(platform_id,
    done, total) may return False to cancel; already-rendered results are kept.
    """
    from khensu_services.common.output_paths import reject_output_alias, validate_output_path
    from khensu_services.copilot.router_render import (
        PostRenderGuardrailError, RENDER_STATE_VALIDATED, RenderCancelled, render_through_pipeline,
    )

    ids = platform_ids or DEFAULT_PLATFORMS
    ids = [p for p in ids if resolve_platform_id(p) is not None]
    total = len(ids)
    results: list[dict[str, Any]] = []

    for i, pid in enumerate(ids):
        if progress_cb is not None and not progress_cb(pid, i, total):
            break
        spec = PLATFORM_TARGETS[resolve_platform_id(pid)]
        out_path = _out_name(src_path, out_dir, pid)
        base: dict[str, Any] = {"platform_id": pid, "platform_name": spec["name"]}
        try:
            validate_output_path(out_path, src_path)
            reject_output_alias(out_path, src_path)
            rendered_box: dict[str, Any] = {}

            def _render(quarantine: str, _spec=spec, _box=rendered_box) -> dict:
                _box.update(render_platform_master(
                    src_path, quarantine,
                    target_lufs=_spec["target_lufs"], ceiling_dbtp=_spec["ceiling_dbtp"],
                    keep_loudness=not match_loudness))
                return _box

            # A loudness-only render has no effect chain to pre-validate.
            rendered, guardrail, _validation = render_through_pipeline(
                src_path, out_path, _render, validate=False,
                should_cancel=(lambda: progress_cb is not None and not progress_cb(pid, i, total))
                if progress_cb is not None else None)
            rendered = dict(rendered)
            rendered["output_path"] = out_path
            entry: dict[str, Any] = {**base, "status": JobStatus.COMPLETED.value, "render_state": RENDER_STATE_VALIDATED,
                                     **rendered, "guardrail": guardrail}
            if not rendered["target_reached"]:
                # The file is written and peak-safe, but it is not the master
                # that was asked for: say so instead of shipping it as one.
                entry["status"] = "could_not_reach"
                entry["error"] = (
                    f"Could not reach {spec['target_lufs']:g} LUFS within the "
                    f"{spec['ceiling_dbtp']:g} dBTP ceiling (landed at "
                    f"{rendered['achieved_lufs']} LUFS).")
            elif rendered.get("max_gain_reduction_db", 0.0) > MAX_GAIN_REDUCTION_WARN_DB:
                guardrail.setdefault("warnings", []).append(
                    f"Reaching {spec['target_lufs']:g} LUFS took "
                    f"{rendered['max_gain_reduction_db']:.1f} dB of brickwall limiting - "
                    "audition for pumping/flattened transients.")
            results.append(entry)
        except RenderCancelled:
            break
        except PostRenderGuardrailError as exc:
            _log.warning("Platform master %s rejected by the post-render guardrail: %s", pid, exc)
            entry = {**base, "status": "rejected", "render_state": exc.render_state,
                     "error": str(exc), "quarantine_path": exc.quarantine_path}
            entry.update({k: v for k, v in rendered_box.items() if k != "output_path"})
            if isinstance(exc.measurements, dict):
                entry["guardrail"] = exc.measurements
            results.append(entry)
        except Exception as exc:
            _log.exception("Platform master render failed for %s", pid)
            detail = getattr(exc, "detail", None)
            results.append({**base, "status": JobStatus.FAILED.value,
                            "error": str(detail) if detail else str(exc)})

    if progress_cb is not None:
        progress_cb("done", total, total)
    return results
