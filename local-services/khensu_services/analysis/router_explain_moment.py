# local-services/khensu_services/analysis/router_explain_moment.py
"""CE-AI-6: ``/explain/moment`` - what makes this region different from the
rest of the track (and from the genre norm), in measured numbers and plain
English."""
from __future__ import annotations

from typing import Any, Optional

from fastapi import APIRouter
from pydantic import Field

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import invalid_input, require_file
from khensu_services.common.executors import run_blocking, run_llm_blocking
from khensu_services.common.lru import BoundedCache, file_cache_key

router = APIRouter()

_track_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=16)


class ExplainMomentRequest(RequestModel):
    file_path: str
    start_s: float = Field(..., ge=0.0)
    end_s: float = Field(..., gt=0.0)
    genre_id: Optional[str] = None
    stem_paths: Optional[dict[str, str]] = Field(
        None, description="Optional separated stems (name -> path) for masking / vocal audibility.")
    rephrase: bool = Field(False, description="Ask the configured LLM to reword the measured facts.")


class ExplainBandLevel(ResponseModel):
    center_hz: float
    level_db: float


class ExplainMomentMetrics(ResponseModel):
    duration_s: float
    lufs: Optional[float] = None
    tilt_db_per_octave: float
    centroid_hz: float
    band_levels: list[ExplainBandLevel]
    width: float
    correlation: float
    crest_db: Optional[float] = None
    onsets_per_s: float
    silent: bool
    masking_collision: Optional[float] = None
    masking_worst_pair: Optional[str] = None
    vocal_partial_ratio: Optional[float] = None


class ExplainDeviation(ResponseModel):
    metric: str
    label: str
    region_value: float
    reference_value: float
    delta: float
    unit: str
    against: str
    score: float
    notable: bool
    direction: str
    sentence: str


class ExplainMomentResponse(ResponseModel):
    file_path: str
    start_s: float
    end_s: float
    track_duration_s: float
    genre_id: str
    genre_found: bool
    region: ExplainMomentMetrics
    track: ExplainMomentMetrics
    deviations: list[ExplainDeviation]
    facts: list[str]
    summary: str
    text: str
    rephrased: bool
    rephrase_source: str
    rephrase_reason: str


def _compute(req: ExplainMomentRequest, stems: dict[str, str]) -> dict[str, Any]:
    from khensu_services.analysis.explain_moment import explain_moment, track_profile
    key = file_cache_key({"file": req.file_path, **{f"stem:{k}": v for k, v in stems.items()}})
    track = _track_cache.get(key) if key is not None else None
    if track is None:
        track = track_profile(req.file_path, stems or None)
        if key is not None:
            _track_cache[key] = track
    try:
        return explain_moment(req.file_path, req.start_s, req.end_s, genre_id=req.genre_id or None,
                              stem_paths=stems or None, track=track)
    except ValueError as exc:
        if "region" in str(exc):
            invalid_input(f"Invalid region: {exc}")
        raise


@router.post("/explain/moment", response_model=ExplainMomentResponse,
             response_model_exclude_unset=True)
async def explain_moment_endpoint(req: ExplainMomentRequest) -> dict[str, Any]:
    require_file(req.file_path)
    if req.end_s <= req.start_s:
        invalid_input("end_s must be greater than start_s")
    stems = {k: v for k, v in (req.stem_paths or {}).items() if v}
    for name, p in stems.items():
        require_file(p, f"{name} stem")
    data = await run_blocking(_compute, req, stems)
    if req.rephrase:
        from khensu_services.analysis.explain_moment import rephrase_facts
        r = await run_llm_blocking(rephrase_facts, data["summary"])
        data = {**data, "text": r["text"], "rephrased": r["rephrased"],
                "rephrase_source": r["source"], "rephrase_reason": r["reason"]}
    return data
