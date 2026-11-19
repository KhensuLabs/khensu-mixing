"""Reference-matched full corrective chain.

Reverse-engineers a corrective processing chain to move a master toward a loaded
reference — beyond match-EQ. It derives:

* Match-EQ: a handful of PeakFilter bands from the averaged reference spectral
  delta (boost where the master is shy, cut where it is hot), clamped for safety.
* Loudness: a Gain toward the reference integrated LUFS, driving a true-peak
  Limiter that closes the chain (so the boost survives the output stage).
* Dynamics: a gentle Compressor when the master is meaningfully more dynamic
  (higher crest factor) than the reference.
* Width: a StereoWidener move toward the reference's side/mid ratio.

The chain is emitted in the /copilot/apply_chain effect format, so applying it
runs through the existing pre-render validator and post-render guardrail. This
module also runs the pre-render validator itself and returns the verdict.
"""
from __future__ import annotations

from typing import Any

import numpy as np
import soundfile as sf


def _clamp(v: float, lo: float, hi: float) -> float:
    return float(max(lo, min(hi, v)))


# The limiter that closes a loudness move: true-peak ceiling, dBTP.
_CEILING_DBTP = -1.0


def _integrated_lufs(y_frames: np.ndarray, sr: int) -> float:
    """Shared, layout-aware BS.1770 meter (CPL-11); NaN when unmeasurable."""
    from khensu_services.copilot.render_guardrails import _integrated_lufs as shared
    return shared(y_frames, sr)


def _crest_db(y_frames: np.ndarray) -> float:
    """Shared per-channel crest (``loudness.crest_factor_db``); 0 for silence."""
    from khensu_services.analysis.loudness import crest_factor_db
    crest = crest_factor_db(np.asarray(y_frames).T)
    return float(crest) if crest is not None else 0.0


def _rms_db(mono: np.ndarray) -> float:
    return 20.0 * float(np.log10(np.sqrt(np.mean(mono ** 2)) + 1e-12))


def _width(y_frames: np.ndarray) -> float:
    if y_frames.ndim < 2 or y_frames.shape[1] < 2:
        return 0.0
    mid = (y_frames[:, 0] + y_frames[:, 1]) * 0.5
    side = (y_frames[:, 0] - y_frames[:, 1]) * 0.5
    return float(np.sqrt(np.mean(side ** 2)) / (np.sqrt(np.mean(mid ** 2)) + 1e-12))


def match_eq_bands(target_path: str, reference_path: str, n_eq_bands: int = 5,
                   max_eq_gain_db: float = 6.0, amount: float = 1.0):
    """The match-EQ toward a reference (CON-9: the one engine - the reference
    chain and Reference Compare's match curve both read it): PeakFilter
    effects, the per-band tonal delta before, and after. Whole-file delta on
    one shared band grid, dB of mean power both sides (AUDIT-007)."""
    from khensu_services.analysis.reference_spectral_delta import average_tonal_delta
    sd = average_tonal_delta(target_path, reference_path)
    avg_delta = np.asarray(sd["delta_db"], dtype=float)  # (bands,): +hot/-shy
    edges = np.asarray(sd["band_edges_hz"], dtype=float)
    after_delta = avg_delta.copy()
    effects: list[dict[str, Any]] = []
    if avg_delta.size and edges.size >= 2:
        n_bands = len(avg_delta)
        groups = np.array_split(np.arange(n_bands), min(n_eq_bands, n_bands))
        for g in groups:
            if g.size == 0:
                continue
            corrective = -float(np.mean(avg_delta[g]))   # invert: undo hot/shy
            gain = round(amount * _clamp(corrective, -max_eq_gain_db, max_eq_gain_db), 1)
            if abs(gain) < 0.7:                           # skip inaudible moves
                continue
            after_delta[g] = avg_delta[g] + gain
            lo = edges[g[0]]
            hi = edges[min(g[-1] + 1, len(edges) - 1)]
            centre = round(float(np.sqrt(max(lo, 1.0) * max(hi, 1.0))), 1)
            effects.append({"effect": "PeakFilter",
                            "parameters": {"cutoff_frequency_hz": centre,
                                           "gain_db": gain, "q": 1.0}})
    return effects, avg_delta, after_delta, edges


def build_reference_chain(target_path: str, reference_path: str,
                          n_eq_bands: int = 5, max_eq_gain_db: float = 6.0,
                          amount: float = 1.0) -> dict[str, Any]:
    """Derive the corrective chain + rationale. Never raises on the DSP path.

    ``amount`` (FEAT-3, 0..1) is how far toward the reference: every EQ gain
    and the loudness gain scale by it, the compressor ratio and the width
    move that share of the way from "no change". ``predicted_curve`` is the
    tonal delta per band before and after the match-EQ, so the move can be
    judged before rendering."""
    amount = float(min(1.0, max(0.0, amount)))
    yt, srt = sf.read(target_path, always_2d=True)
    yr, srr = sf.read(reference_path, always_2d=True)
    tgt_mono = yt.mean(axis=1)

    effects: list[dict[str, Any]] = []
    rationale: list[str] = []

    # --- Match-EQ from the averaged spectral delta -------------------------
    eq, avg_delta, after_delta, edges = match_eq_bands(target_path, reference_path, n_eq_bands, max_eq_gain_db,
                                                       amount)
    effects.extend(eq)
    if eq:
        rationale.append(f"Match-EQ: {len(eq)} corrective band(s) toward the reference tone.")

    # --- Dynamics: compress when the master is more dynamic than the ref ----
    tgt_crest, ref_crest = _crest_db(yt), _crest_db(yr)
    if tgt_crest > ref_crest + 2.0:
        threshold = round(_clamp(_rms_db(tgt_mono), -40.0, -6.0), 1)
        ratio = round(1.0 + 1.0 * amount, 2)
        if ratio > 1.05:
            effects.append({"effect": "Compressor",
                            "parameters": {"threshold_db": threshold, "ratio": ratio,
                                           "attack_ms": 20.0, "release_ms": 200.0}})
        rationale.append(
            f"Dynamics: master crest {tgt_crest:.1f} dB vs reference {ref_crest:.1f} dB — "
            "gentle 2:1 compression to tighten it.")

    # --- Width: move toward the reference side/mid ratio --------------------
    tgt_w, ref_w = _width(yt), _width(yr)
    if tgt_w > 1e-3 and abs(ref_w - tgt_w) / tgt_w > 0.15:
        width = round(1.0 + (_clamp(ref_w / tgt_w, 0.5, 1.6) - 1.0) * amount, 2)
        if abs(width - 1.0) >= 0.02:
            effects.append({"effect": "StereoWidener", "parameters": {"width": width}})
        rationale.append(
            f"Width: side/mid {tgt_w:.2f} vs reference {ref_w:.2f} — "
            f"{'widen' if width > 1.0 else 'narrow'} toward the reference.")

    # --- Loudness toward the reference: Gain INTO a limiter, limiter last ---
    # A bare Gain at the end of the chain (no limiter) was scaled straight
    # back down to -1 dBTP by the render's output stage, so loudness barely
    # moved while the rationale still claimed "X -> reference LUFS" (CPL-07).
    # Driving a true-peak Limiter instead makes the boost real: the limiter
    # holds the ceiling and the output stage, which honours the chain's own
    # limiter ceiling (CPL-12), has nothing left to undo.
    tgt_lufs, ref_lufs = _integrated_lufs(yt, srt), _integrated_lufs(yr, srr)
    if np.isfinite(tgt_lufs) and np.isfinite(ref_lufs):
        gain_db = round(amount * _clamp(ref_lufs - tgt_lufs, -12.0, 12.0), 1)
        if abs(gain_db) >= 0.5:
            effects.append({"effect": "Gain", "parameters": {"gain_db": gain_db}})
            rationale.append(
                f"Loudness: {tgt_lufs:.1f} -> {ref_lufs:.1f} LUFS ({gain_db:+.1f} dB into a "
                f"{_CEILING_DBTP:g} dBTP true-peak limiter).")
    if any(e["effect"] == "Gain" and e["parameters"]["gain_db"] > 0.0 for e in effects):
        effects.append({"effect": "Limiter",
                        "parameters": {"threshold_db": _CEILING_DBTP, "release_ms": 60.0}})

    # --- Pre-render safety verdict -----------------------------------------
    # A validator that cannot run is a failed check, never a pass (CPL-07).
    try:
        from khensu_services.copilot.render_validator import validate_render_request
        pre_render: dict[str, Any] = validate_render_request(target_path, effects, None)
    except Exception as exc:
        pre_render = {"ok": False, "blocks": [f"Pre-render validator could not run: {type(exc).__name__}"],
                      "warnings": []}

    centres = [float(np.sqrt(max(edges[i], 1.0) * max(edges[i + 1], 1.0))) for i in range(len(edges) - 1)] \
        if edges.size >= 2 else []
    n = min(len(centres), int(avg_delta.size))
    return {
        "target_path": target_path,
        "reference_path": reference_path,
        "amount": amount,
        "predicted_curve": {
            "band_centers_hz": [round(c, 1) for c in centres[:n]],
            "delta_before_db": [round(float(x), 2) for x in avg_delta[:n]],
            "delta_after_db": [round(float(x), 2) for x in after_delta[:n]],
        },
        "effects": effects,
        "rationale": rationale,
        "pre_render": pre_render,
        "measured": {
            "target_lufs": round(tgt_lufs, 1) if np.isfinite(tgt_lufs) else None,
            "reference_lufs": round(ref_lufs, 1) if np.isfinite(ref_lufs) else None,
            "target_crest_db": round(tgt_crest, 1),
            "reference_crest_db": round(ref_crest, 1),
            "target_width": round(tgt_w, 3),
            "reference_width": round(ref_w, 3),
        },
    }
