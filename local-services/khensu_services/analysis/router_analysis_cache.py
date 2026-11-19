# local-services/khensu_services/analysis/router_analysis_cache.py
"""CE-PLT-2 routes: the persistent analysis cache's stats / clear, and
changed-region detection between two versions of a file."""
from __future__ import annotations

import logging
from typing import Any

from fastapi import APIRouter
from pydantic import Field

from khensu_services.common import analysis_cache
from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import internal_error, require_decode_budget, require_file
from khensu_services.common.executors import run_blocking, run_io_blocking

_log = logging.getLogger(__name__)
router = APIRouter()


class AnalysisCacheAnalyzerStats(ResponseModel):
    analyzer: str
    entries: int
    bytes: int


class AnalysisCacheStatsResponse(ResponseModel):
    enabled: bool
    directory: str
    entries: int
    bytes: int
    budget_bytes: int
    hits: int
    misses: int
    puts: int
    evictions: int
    analyzers: list[AnalysisCacheAnalyzerStats]


class AnalysisCacheClearResponse(ResponseModel):
    removed: int


class ChangedRegionsRequest(RequestModel):
    #: The earlier version (e.g. the render's source).
    file_a: str
    #: The later version (e.g. the render).
    file_b: str
    block_ms: float = Field(50.0, ge=5.0, le=2000.0)
    threshold_db: float = Field(-90.0, ge=-160.0, le=0.0)
    merge_gap_ms: float = Field(250.0, ge=0.0, le=10000.0)
    pad_ms: float = Field(100.0, ge=0.0, le=5000.0)


class ChangedRegion(ResponseModel):
    start_sec: float
    end_sec: float


class ChangedRegionsResponse(ResponseModel):
    comparable: bool
    reason: str
    method: str
    duration_sec: float
    changed_sec: float
    changed_fraction: float
    regions: list[ChangedRegion]
    max_difference_db: float | None = None


@router.get("/analysis/cache", response_model=AnalysisCacheStatsResponse,
            response_model_exclude_unset=True)
async def analysis_cache_stats() -> dict[str, Any]:
    """Size, budget, hit/miss counters and per-analyzer usage of the
    persistent content-addressed analysis cache."""
    return await run_io_blocking(analysis_cache.stats)


@router.post("/analysis/cache/clear", response_model=AnalysisCacheClearResponse,
             response_model_exclude_unset=True)
async def analysis_cache_clear() -> dict[str, Any]:
    """Delete every cached analysis result (the next request recomputes)."""
    return {"removed": await run_io_blocking(analysis_cache.clear)}


@router.post("/analysis/changed_regions", response_model=ChangedRegionsResponse,
             response_model_exclude_unset=True)
async def changed_regions(req: ChangedRegionsRequest) -> dict[str, Any]:
    """Time ranges where ``file_b`` differs from ``file_a`` (block hashing
    plus difference energy), so a post-render re-measure can be restricted
    to them. ``comparable=False`` (sample rate / channels / length differ)
    reports the whole file as changed."""
    require_file(req.file_a)
    require_file(req.file_b)
    await require_decode_budget(req.file_a, req.file_b)
    from khensu_services.analysis.changed_regions import find_changed_regions
    try:
        return await run_blocking(
            find_changed_regions, req.file_a, req.file_b, block_ms=req.block_ms,
            threshold_db=req.threshold_db, merge_gap_ms=req.merge_gap_ms, pad_ms=req.pad_ms)
    except Exception:
        _log.exception("Changed-region detection failed")
        internal_error()
