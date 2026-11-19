"""Render tools: device preview, delta solo, A/B critique, crossfeed A/B, reference chain (ENG-6, from router_render.py)."""
from __future__ import annotations

import logging
import os
from fastapi import APIRouter
from pydantic import Field
from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.copilot.dsp import render_delta
from khensu_services.common.errors import internal_error, invalid_input, not_found
from khensu_services.common.executors import run_blocking
from khensu_services.copilot.response_models import AbCritiqueResponse, CrossfeedAbResponse, ReferenceChainResponse
from khensu_services.copilot.router_render import _require_audio_decode_budget

_log = logging.getLogger(__name__)
router = APIRouter()


class DevicePreviewRequest(RequestModel):
    file_path: str
    device: str        # phone | earbuds | car | club


class DevicePreviewResponse(ResponseModel):
    device: str
    rendered_path: str
    note: str


@router.post("/render_device_preview", response_model=DevicePreviewResponse, response_model_exclude_unset=True)
async def render_device_preview_endpoint(req: DevicePreviewRequest) -> DevicePreviewResponse:
    """Translation Check: render the mix as a playback device presents it
    (phone / earbuds / car / club) so translation is auditioned, not
    predicted. Writes <file>_device_<name>.wav next to the source."""
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    await _require_audio_decode_budget(req.file_path)
    try:
        from khensu_services.copilot.device_sim import render_device_preview
        data = await run_blocking(render_device_preview, req.file_path, req.device)
    except ValueError as exc:
        invalid_input(str(exc), cause=exc)
    except Exception:
        _log.exception("Device preview render failed")
        internal_error()
    return DevicePreviewResponse(**data)


class RenderDeltaRequest(RequestModel):
    dry_path: str
    wet_path: str
    output_path: str
    normalize_gain_db: float = 0.0


class RenderDeltaResponse(ResponseModel):
    output_path: str
    peak_delta_dbfs: float


class AbCritiqueRequest(RequestModel):
    path_a: str
    path_b: str
    label_a: str = "A"
    label_b: str = "B"


@router.post("/ab_critique", response_model=AbCritiqueResponse, response_model_exclude_unset=True)
async def ab_critique_endpoint(req: AbCritiqueRequest) -> dict:
    """Which take is better, and why — from matched-level measurements (dynamics,
    true peak, clipping, mono compat, presence), not vibes."""
    if not os.path.isfile(req.path_a):
        not_found(f"File not found: {req.path_a}")
    if not os.path.isfile(req.path_b):
        not_found(f"File not found: {req.path_b}")
    await _require_audio_decode_budget(req.path_a, req.path_b)
    from khensu_services.copilot.ab_critique import critique_ab
    return await run_blocking(critique_ab, req.path_a, req.path_b, req.label_a, req.label_b)


class CrossfeedRequest(RequestModel):
    file_path: str
    output_dir: str = ""


@router.post("/crossfeed_ab", response_model=CrossfeedAbResponse, response_model_exclude_unset=True)
async def crossfeed_ab_endpoint(req: CrossfeedRequest) -> dict:
    """Render the in-head headphone image (A) and the crossfed speaker image (B)
    for a direct A/B, and report how much the stereo width collapses on speakers."""
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    await _require_audio_decode_budget(req.file_path)
    if req.output_dir:
        from khensu_services.common.output_paths import validate_output_path
        validate_output_path(os.path.join(req.output_dir, "probe.wav"), req.file_path)
    from khensu_services.copilot.crossfeed import render_crossfeed_ab
    return await run_blocking(render_crossfeed_ab, req.file_path, req.output_dir or None)


class ReferenceChainRequest(RequestModel):
    file_path: str
    reference_path: str
    # FEAT-3: how far toward the reference (0 = nothing, 1 = all the way).
    amount: float = Field(default=1.0, ge=0.0, le=1.0)


@router.post("/reference_chain", response_model=ReferenceChainResponse, response_model_exclude_unset=True)
async def reference_chain_endpoint(req: ReferenceChainRequest) -> dict:
    """Reverse-engineer a corrective chain (match-EQ + loudness + dynamics +
    width) to move the master toward a reference. Returns an apply_chain-ready
    effects list, the rationale, and the pre-render safety verdict. Apply it via
    /copilot/apply_chain to run the full pre+post render validators."""
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    if not os.path.isfile(req.reference_path):
        not_found(f"Reference not found: {req.reference_path}")
    await _require_audio_decode_budget(req.file_path, req.reference_path)
    from khensu_services.copilot.reference_chain import build_reference_chain
    return await run_blocking(build_reference_chain, req.file_path, req.reference_path, amount=req.amount)


@router.post("/render_delta", response_model=RenderDeltaResponse, response_model_exclude_unset=True)
async def render_delta_endpoint(req: RenderDeltaRequest) -> RenderDeltaResponse:
    """Null-test render: (wet - dry), for "delta solo" auditioning of only
    what a processing chain changed. Fast enough (one subtract) not to need
    a job queue like apply_chain/apply_graph."""
    if not os.path.isfile(req.dry_path):
        not_found(f"File not found: {req.dry_path}")
    if not os.path.isfile(req.wet_path):
        not_found(f"File not found: {req.wet_path}")
    await _require_audio_decode_budget(req.dry_path, req.wet_path)

    from khensu_services.common.output_paths import validate_output_path, reject_output_alias
    validate_output_path(req.output_path, req.dry_path)
    reject_output_alias(req.output_path, req.dry_path, req.wet_path)

    try:
        out_dir = os.path.dirname(req.output_path)
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
        result = await run_blocking(render_delta, req.dry_path, req.wet_path,
                                    req.output_path, req.normalize_gain_db)
        return RenderDeltaResponse(**result)
    except Exception:
        _log.exception("Failed to render delta for %s vs %s", req.dry_path, req.wet_path)
        internal_error()


# --- FEAT-4: stem rebalance ------------------------------------------------------

class StemRebalanceSuggestRequest(RequestModel):
    stems: dict[str, str]
    genre_id: str = ""


class StemRebalanceSuggestResponse(ResponseModel):
    available: bool
    reason: str = ""
    gains_db: dict[str, float]
    measured_share: dict[str, float]
    target_share: dict[str, float] = {}


class StemRebalanceRequest(RequestModel):
    stems: dict[str, str]
    gains_db: dict[str, float]
    output_path: str
    # The mix the stems came from: with every gain at 0 the residual to it is reported.
    original_mix: str = ""


class StemRebalanceResponse(ResponseModel):
    output_path: str
    true_peak_dbtp: float
    trim_db: float
    residual_db: float | None = None
    gains_db: dict[str, float]


def _require_stems(stems: dict[str, str]) -> None:
    if not stems:
        invalid_input("stems: at least one stem is required")
    from khensu_services.common.errors import require_file
    for name, path in stems.items():
        # require_file refuses a network (UNC / URL) path before it stats it:
        # a bare isfile on a UNC path would open an SMB connection.
        require_file(path, f"{name} stem")


@router.post("/stem_rebalance/suggest", response_model=StemRebalanceSuggestResponse,
             response_model_exclude_unset=True)
async def stem_rebalance_suggest(req: StemRebalanceSuggestRequest) -> dict:
    """Gains per stem that move each stem's share of the mix energy toward
    the genre's stem balance (FEAT-4); +/-6 dB at most."""
    _require_stems(req.stems)
    await _require_audio_decode_budget(*req.stems.values())
    from khensu_services.copilot.stem_rebalance import suggest
    return await run_blocking(suggest, req.stems, req.genre_id)


@router.post("/stem_rebalance", response_model=StemRebalanceResponse, response_model_exclude_unset=True)
async def stem_rebalance(req: StemRebalanceRequest) -> dict:
    """Re-sum the stems at their gains into a new version of the mix (FEAT-4),
    true peak held at -1 dBTP."""
    _require_stems(req.stems)
    if req.original_mix:
        from khensu_services.common.errors import require_file
        require_file(req.original_mix, "original mix")
    if any(abs(g) > 24.0 for g in req.gains_db.values()):
        invalid_input("gains_db: each gain must be within +/-24 dB")
    from khensu_services.common.output_paths import reject_output_alias, validate_output_path
    first = next(iter(req.stems.values()))
    validate_output_path(req.output_path, first)
    reject_output_alias(req.output_path, *req.stems.values(), *([req.original_mix] if req.original_mix else []))
    await _require_audio_decode_budget(*req.stems.values(), *([req.original_mix] if req.original_mix else []))
    from khensu_services.copilot.stem_rebalance import render
    return await run_blocking(render, req.stems, req.gains_db, req.output_path, req.original_mix or None)
