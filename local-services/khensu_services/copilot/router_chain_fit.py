"""POST /render/fit_chain (CE-AI-3): fit a readable EQ + compressor chain that
moves a mix toward a reference or genre target, render it, and report the
predicted vs measured error."""
from __future__ import annotations

import logging
import os
import time

from fastapi import APIRouter
from pydantic import Field

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import invalid_input, require_decode_budget, require_file
from khensu_services.common.executors import run_long_blocking

_log = logging.getLogger(__name__)
router = APIRouter(tags=["render"])


class FitChainRequest(RequestModel):
    file_path: str
    # Exactly one target: a reference file, or a genre id from genre_norms.
    reference_path: str = ""
    genre: str = Field(default="", max_length=64)
    # FEAT-3: how far toward the target (0 = nothing, 1 = all the way).
    amount: float = Field(default=1.0, ge=0.0, le=1.0)
    max_peaks: int = Field(default=4, ge=0, le=4)
    # False = fit only (no file written).
    render: bool = True
    # Empty = "<source>_fitchain_<timestamp>.wav" beside the source.
    output_path: str = ""


class FitBand(ResponseModel):
    frequency_hz: float
    gain_db: float
    q: float


class FitCompressor(ResponseModel):
    threshold_db: float
    ratio: float
    attack_ms: float
    release_ms: float
    mean_gain_reduction_db: float = 0.0


class FittedChain(ResponseModel):
    low_shelf: FitBand
    high_shelf: FitBand
    peaks: list[FitBand] = []
    compressor: FitCompressor | None = None
    makeup_gain_db: float = 0.0


class FitEffect(ResponseModel):
    effect: str
    parameters: dict[str, float] = {}


class FitTarget(ResponseModel):
    kind: str          # "reference" | "genre"
    label: str


class FitErrors(ResponseModel):
    band_error_db: float | None = None     # weighted RMS of the level-free band error
    crest_db: float | None = None
    lra_lu: float | None = None
    lufs: float | None = None
    crest_error_db: float | None = None    # signed: actual - target
    lra_error_lu: float | None = None


class FitChainResponse(ResponseModel):
    amount: float
    target: FitTarget
    chain: FittedChain
    effects: list[FitEffect] = []
    band_centers_hz: list[float] = []
    target_delta_db: list[float] = []
    predicted_delta_db: list[float] = []
    measured_delta_db: list[float] = []
    before: FitErrors
    predicted: FitErrors
    measured: FitErrors | None = None
    notes: list[str] = []
    # "validated" (rendered and passed the guardrail), "blocked", "rejected",
    # "skipped" (nothing to change); absent when render=false.
    render_state: str | None = None
    render_error: str | None = None
    output_path: str | None = None


def _render(file_path: str, effects: list[dict], output_path: str) -> dict:
    """Render through the shared validate -> quarantine -> render ->
    guardrail -> promote pipeline. Blocking."""
    from khensu_services.copilot.dsp import apply_pedalboard_chain
    from khensu_services.copilot.router_render import (
        PostRenderGuardrailError, RenderBlocked, render_through_pipeline)
    try:
        render_through_pipeline(
            file_path, output_path,
            lambda quarantine: apply_pedalboard_chain(file_path, quarantine, effects, normalize=False),
            effects=effects)
    except RenderBlocked as blocked:
        return {"render_state": "blocked",
                "render_error": "; ".join(blocked.validation.get("blocks") or []) or "blocked"}
    except PostRenderGuardrailError as exc:
        return {"render_state": exc.render_state or "rejected", "render_error": str(exc)}
    return {"render_state": "validated"}


@router.post("/render/fit_chain", response_model=FitChainResponse, response_model_exclude_unset=True)
async def fit_chain_endpoint(req: FitChainRequest) -> dict:
    require_file(req.file_path)
    has_ref = bool(req.reference_path.strip())
    has_genre = bool(req.genre.strip())
    if has_ref == has_genre:
        invalid_input("Give exactly one target: reference_path or genre.")
    if has_ref:
        require_file(req.reference_path, "Reference")
    await require_decode_budget(*([req.file_path, req.reference_path] if has_ref else [req.file_path]))

    output_path = None
    if req.render:
        from khensu_services.common.output_paths import resolve_render_output
        suffix = f"fitchain_{time.strftime('%Y%m%d_%H%M%S')}"
        output_path = resolve_render_output(
            req.output_path, req.file_path, suffix, allowed_exts=(".wav",),
            extra_sources=(req.reference_path if has_ref else None,))

    from khensu_services.copilot.chain_fit import fit_and_render
    try:
        return await run_long_blocking(
            fit_and_render, req.file_path, req.reference_path if has_ref else None,
            req.genre if has_genre else None, req.amount, req.max_peaks, output_path,
            (lambda effects, out: _render(req.file_path, effects, out)) if output_path else None)
    except ValueError as exc:
        invalid_input(str(exc))
    except Exception:
        _log.exception("fit_chain failed for %s", os.path.basename(req.file_path))
        raise
