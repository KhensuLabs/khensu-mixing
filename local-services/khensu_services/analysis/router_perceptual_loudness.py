# local-services/khensu_services/analysis/router_perceptual_loudness.py
"""CE-AI-5: ``/analyze/partial_loudness`` - Moore-Glasberg partial loudness of
a target stem inside the mix (or inside explicit masker stems)."""
from __future__ import annotations

from typing import Any, Optional

from fastapi import APIRouter
from pydantic import Field

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import invalid_input, require_decode_budget, require_file
from khensu_services.common.executors import run_blocking
from khensu_services.common.lru import BoundedCache, file_cache_key

router = APIRouter()

_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=16)


class PartialLoudnessRequest(RequestModel):
    target_path: str = Field(..., description="The stem whose audibility is measured (e.g. vocals).")
    mix_path: Optional[str] = Field(None, description="Full mix; masker = mix - target.")
    masker_paths: Optional[list[str]] = Field(
        None, description="Explicit masker stems (summed), used when mix_path is not given.")
    full_scale_sine_db_spl: float = Field(100.0, ge=60.0, le=130.0,
                                          description="SPL a full-scale digital sine plays at.")
    start_s: Optional[float] = Field(None, ge=0.0)
    end_s: Optional[float] = Field(None, ge=0.0)
    timeline_window_s: float = Field(1.0, ge=0.25, le=30.0)


class PartialLoudnessBand(ResponseModel):
    lo_hz: float
    hi_hz: float
    unmasked_sone: float
    partial_sone: float
    ratio: float


class PartialLoudnessPoint(ResponseModel):
    start_s: float
    end_s: float
    unmasked_sone: float
    partial_sone: float
    ratio: float


class PartialLoudnessCalibration(ResponseModel):
    full_scale_sine_db_spl: float
    reference: str
    model: str


class PartialLoudnessResponse(ResponseModel):
    target_loudness_sone: float
    target_loudness_phon: float
    target_partial_loudness_sone: float
    target_partial_loudness_phon: float
    masker_loudness_sone: float
    masker_loudness_phon: float
    partial_ratio: float
    masked_pct: float
    verdict: str
    worst_band: Optional[PartialLoudnessBand] = None
    bands: list[PartialLoudnessBand] = []
    timeline: list[PartialLoudnessPoint] = []
    frames: int
    hop_s: float
    masker_source: str
    target_share_db: Optional[float] = None
    duration_s: float
    sample_rate: int
    calibration: PartialLoudnessCalibration


def _compute(req: PartialLoudnessRequest) -> dict[str, Any]:
    from khensu_services.analysis.perceptual_loudness import analyze_partial_loudness
    try:
        return analyze_partial_loudness(
            req.target_path, req.mix_path or None, req.masker_paths or None,
            full_scale_sine_db_spl=req.full_scale_sine_db_spl,
            start_s=req.start_s, end_s=req.end_s,
            timeline_window_s=req.timeline_window_s)
    except ValueError as exc:
        if "region" in str(exc) or "masker" in str(exc):
            invalid_input(f"Invalid partial-loudness request: {exc}")
        raise


@router.post("/analyze/partial_loudness", response_model=PartialLoudnessResponse,
             response_model_exclude_unset=True)
async def analyze_partial_loudness_endpoint(req: PartialLoudnessRequest) -> dict[str, Any]:
    require_file(req.target_path, "Target stem")
    maskers = [p for p in (req.masker_paths or []) if p]
    if not req.mix_path and not maskers:
        invalid_input("Give mix_path (masker = mix - target) or at least one masker_paths entry")
    if req.mix_path:
        require_file(req.mix_path, "Mix")
    for p in maskers:
        require_file(p, "Masker stem")
    if req.start_s is not None and req.end_s is not None and req.end_s <= req.start_s:
        invalid_input("end_s must be greater than start_s")
    await require_decode_budget(req.target_path, req.mix_path, *maskers, skip_missing=True)

    inputs = {"target": req.target_path, **({"mix": req.mix_path} if req.mix_path else {}),
              **{f"m{i}": p for i, p in enumerate(maskers)}}
    key = file_cache_key(inputs, req.full_scale_sine_db_spl, req.start_s, req.end_s,
                         req.timeline_window_s)
    cached = _cache.get(key) if key is not None else None
    if cached is not None:
        return cached
    data = await run_blocking(_compute, req)
    if key is not None:
        _cache[key] = data
    return data
