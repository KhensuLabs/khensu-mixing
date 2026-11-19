# local-services/khensu_services/copilot/render_validator.py
"""Blocking pre-render safety validator (2026-07-15 safety audit, P1).

Before any apply_chain / apply_graph render runs, prove the operation is safe
to attempt at all. This fails *closed*: a hard block returns without rendering
and hands the UI an actionable reason, rather than producing a technically-
successful-but-musically-unsafe result. Per-parameter ranges are already
clamped in dsp._clamped_params; this covers what that can't see — the source's
channel layout / sample rate / length, and chain-level risks like widening a
mono source or stacking large make-up gain into no headroom.

Post-render regressions are caught separately by render_guardrails.py.
"""
from __future__ import annotations

import os
from typing import Any

import soundfile as sf

from khensu_services.common.channel_layout import ChannelLayout, UnsupportedLayoutError

_MIN_DURATION_SEC = 0.5
_MIN_SR = 8000
_WIDENER_PHASE_RISK = 1.6      # width beyond this risks mono cancellation
_MASTER_GAIN_WARN_DB = 9.0     # make-up gain beyond this eats headroom fast


class InvalidRenderParameter(ValueError):
    """A chain parameter the validator must read is not a finite number.

    Raised (not swallowed into a block) so the HTTP layer answers 422 - an
    invalid request - instead of the 500 a bare ``float("loud")`` used to
    produce (audit CPL-09)."""


def _num(effect: str, params: dict, key: str, default: float = 0.0) -> float:
    """``params[key]`` as a finite float; ``InvalidRenderParameter`` otherwise."""
    import math

    raw = params.get(key, default)
    if raw is None:
        return float(default)
    if isinstance(raw, bool):
        raise InvalidRenderParameter(f"{effect}.{key} must be a number, got {raw!r}")
    try:
        value = float(raw)
    except (TypeError, ValueError) as exc:
        raise InvalidRenderParameter(f"{effect}.{key} must be a number, got {raw!r}") from exc
    if not math.isfinite(value):
        raise InvalidRenderParameter(f"{effect}.{key} must be finite, got {raw!r}")
    return value


# Which parameter carries each stage's added level (dB). GlueCompressor,
# SidechainCompressor and MultibandCompressor call it ``makeup_db`` (dsp.py's
# clamp table); the validator used to read a ``makeup_gain_db`` key no stage
# has, so their make-up gain was never counted (audit CPL-09).
_GAIN_KEYS = {
    "Gain": "gain_db",
    "GlueCompressor": "makeup_db",
    "SidechainCompressor": "makeup_db",
    "MultibandCompressor": "makeup_db",
}


def _iter_effects(effects: list[dict] | None, graph: dict | None) -> list[dict]:
    out: list[dict] = []
    for e in (effects or []):
        out.append(e)
    if graph:
        # The flat graph form {"effects": [...]} is a single wet branch in
        # dsp.apply_graph, so its effects are validated too.
        out.extend(graph.get("effects") or [])
        for b in graph.get("branches") or []:
            out.extend(b.get("effects") or [])
        out.extend(graph.get("post") or [])
    return out


def validate_chain_parameters(effects: list[dict] | None = None, graph: dict | None = None) -> None:
    """The parameter checks of ``validate_render_request`` that need no source
    file: raises ``InvalidRenderParameter`` for a non-numeric value the
    validator reads. Job endpoints call it before admission so a malformed
    chain is a 422 at submit time, not a failed job later."""
    for e in _iter_effects(effects, graph):
        name = str(e.get("effect"))
        params = e.get("parameters") or {}
        if name == "StereoWidener":
            _num(name, params, "width", 1.0)
        key = _GAIN_KEYS.get(name)
        if key is not None:
            _num(name, params, key, 0.0)
    if graph:
        for i, b in enumerate(graph.get("branches", [])):
            _num(f"branch[{i}]", b, "gain_db", 0.0)


def validate_render_request(
    file_path: str,
    effects: list[dict] | None = None,
    graph: dict | None = None,
) -> dict[str, Any]:
    blocks: list[str] = []
    warnings: list[str] = []

    # --- Source header (cheap; no full decode) ---
    channels = 0
    sr = 0
    duration = 0.0
    low_corr: float | None = None
    try:
        info = sf.info(file_path)
        channels, sr = int(info.channels), int(info.samplerate)
        duration = float(info.frames) / sr if sr else 0.0
    except Exception as exc:
        return {"ok": False, "blocks": [f"Could not read source audio: {exc}"], "warnings": []}

    # Channel layout (audit AQ-01). Mono/stereo/5.1/7.1 are supported; anything
    # else fails closed. For surround, every requested effect must additionally
    # have a declared channel routing - an unclassified effect is refused rather
    # than approximated onto channels it was never designed for.
    layout = None
    try:
        layout = ChannelLayout.from_channel_count(channels)
    except UnsupportedLayoutError as exc:
        blocks.append(str(exc))

    if sr < _MIN_SR:
        blocks.append(f"Sample rate {sr} Hz is below the {_MIN_SR} Hz minimum — analysis/render is unreliable.")
    if duration < _MIN_DURATION_SEC:
        blocks.append(f"Source is only {duration:.2f}s — too short to render safely.")

    effs = _iter_effects(effects, graph)

    # --- Effects must have a declared channel routing on multichannel sources ---
    if layout is not None and layout.num_channels > 2:
        from khensu_services.copilot.dsp import routing_for
        unclassified = sorted({
            str(e.get("effect")) for e in effs
            if e.get("effect") and routing_for(str(e.get("effect"))) is None
        })
        if unclassified:
            blocks.append(
                f"No channel routing is declared for {', '.join(unclassified)}, so it "
                f"cannot be applied safely to a {layout.name} source. Render this as "
                "mono/stereo, or remove the effect.")

    # --- Chain-level risks ---
    has_widener = any(e.get("effect") == "StereoWidener" for e in effs)
    if has_widener:
        if channels < 2:
            warnings.append("StereoWidener on a mono source does nothing useful — skip it.")
        else:
            # Only measure low-band correlation if we're actually widening.
            # StereoWidener is a front-pair operation, so measure the front pair
            # rather than the whole field on a surround source (AQ-01).
            try:
                y, _ = sf.read(file_path, always_2d=True)
                if y.shape[1] >= 2:
                    from khensu_services.analysis.signal_integrity import analyze_mono_bass
                    front = y[:, :2] if layout is None else y[:, list(layout.front_pair())]
                    low_corr = float(analyze_mono_bass(front.T, sr)["correlation_below_cutoff"])
            except Exception:
                low_corr = None
        for e in effs:
            if e.get("effect") != "StereoWidener":
                continue
            width = _num("StereoWidener", e.get("parameters") or {}, "width", 1.0)
            if width > _WIDENER_PHASE_RISK:
                msg = f"StereoWidener width {width:.2f} risks mono cancellation of the low end"
                if low_corr is not None and low_corr < 0.6:
                    blocks.append(msg + f" (low-band correlation already {low_corr:.2f}).")
                else:
                    warnings.append(msg + ".")

    # Stacked make-up / branch gain into no headroom.
    total_gain_db = 0.0
    for e in effs:
        p = e.get("parameters") or {}
        key = _GAIN_KEYS.get(str(e.get("effect")))
        if key is not None:
            total_gain_db += _num(str(e.get("effect")), p, key, 0.0)
    if graph:
        for i, b in enumerate(graph.get("branches", [])):
            total_gain_db += _num(f"branch[{i}]", b, "gain_db", 0.0)
    if total_gain_db > _MASTER_GAIN_WARN_DB:
        warnings.append(
            f"Chain adds ~{total_gain_db:.1f} dB of make-up/branch gain — headroom and "
            "true-peak safety will be checked after render.")

    return {
        "ok": len(blocks) == 0,
        "channels": channels,
        "channel_layout": layout.name if layout is not None else None,
        "lfe_protected": bool(layout is not None and layout.has_lfe()),
        "sample_rate": sr,
        "duration_sec": round(duration, 2),
        "low_band_correlation": low_corr,
        "blocks": blocks,
        "warnings": warnings,
    }


def validate_stem_attribution(file_path: str, target_stem: str = "mix",
                              stem_paths: dict[str, str] | None = None) -> dict[str, Any]:
    """Ensure a stem-targeted instruction cannot silently render the mix.

    The caller must declare the exact stem path when target_stem is not
    ``mix``. This is provenance validation, not source-separation quality
    estimation; it blocks ambiguous attribution before DSP is applied.
    """
    target_stem = (target_stem or "mix").strip().lower()
    allowed = {"mix", "vocals", "drums", "bass", "other"}
    if target_stem not in allowed:
        return {"ok": False, "blocks": [f"Unknown target stem '{target_stem}'."]}
    if target_stem == "mix":
        return {"ok": True, "blocks": []}
    declared = (stem_paths or {}).get(target_stem, "")
    if not declared:
        return {"ok": False, "blocks": [f"Target stem '{target_stem}' has no declared source path."]}
    if os.path.abspath(declared) != os.path.abspath(file_path):
        return {"ok": False, "blocks": [
            f"Target stem '{target_stem}' does not match the file being rendered; refusing ambiguous attribution."]}
    return {"ok": True, "blocks": []}
