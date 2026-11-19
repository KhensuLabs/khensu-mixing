# local-services/khensu_services/analysis/router_metric_timeline.py
from __future__ import annotations
import logging
import os
from typing import Any

from fastapi import APIRouter

from khensu_services.analysis.analyzer_responses import MetricTimelineResponse
from khensu_services.analysis.models import MetricTimelineRequest
from khensu_services.common import analysis_cache
from khensu_services.common.errors import internal_error, require_decode_budget, require_file
from khensu_services.common.lru import BoundedCache
from khensu_services.common.executors import run_blocking

_log = logging.getLogger(__name__)
router = APIRouter()

_metric_timeline_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=32)

# Persistent cache (CE-PLT-2): the analyzer is "metric_timeline"; bump its
# version in common/analysis_versions when analyze_metric_timeline's output
# changes (FIX-15).


def _cache_key(file_path: str, block_sec: float, hop_sec: float) -> tuple:
    st = os.stat(file_path)
    return (file_path, st.st_mtime_ns, st.st_size, round(block_sec, 3), round(hop_sec, 3))


def _compute(file_path: str, block_sec: float, hop_sec: float) -> dict[str, Any]:
    from khensu_services.analysis.metric_timeline import analyze_metric_timeline
    return analysis_cache.persistent_call(
        file_path, "metric_timeline",
        {"block_sec": round(block_sec, 6), "hop_sec": round(hop_sec, 6)},
        lambda: analyze_metric_timeline(file_path, block_sec=block_sec, hop_sec=hop_sec))


def _compute_incremental(baseline_path: str, file_path: str,
                         block_sec: float, hop_sec: float) -> dict[str, Any]:
    """Post-render re-measure: reuse the source's windows where the render
    did not change, recompute the rest. Falls back to a whole-file pass when
    the two files are not sample-comparable."""
    from khensu_services.analysis.changed_regions import (
        find_changed_regions, incremental_metric_timeline,
    )
    regions = find_changed_regions(baseline_path, file_path)
    if regions["comparable"]:
        baseline = _compute(baseline_path, block_sec, hop_sec)
        result = incremental_metric_timeline(baseline, file_path, regions["regions"])
        if result is not None:
            return result
    full = _compute(file_path, block_sec, hop_sec)
    return {**full, "recomputed_windows": int(full.get("n_windows") or 0), "reused_windows": 0}


@router.post("/analyze/metric_timeline", response_model=MetricTimelineResponse, response_model_exclude_unset=True)
async def analyze_metric_timeline_endpoint(req: MetricTimelineRequest) -> dict[str, Any]:
    require_file(req.file_path)
    if req.baseline_path:
        require_file(req.baseline_path, "Baseline file")
    await require_decode_budget(req.file_path)
    if req.baseline_path:
        await require_decode_budget(req.baseline_path)

    if req.baseline_path:
        try:
            return await run_blocking(_compute_incremental, req.baseline_path, req.file_path,
                                      req.block_sec, req.hop_sec)
        except Exception as exc:
            _log.exception("Incremental metric timeline failed")
            internal_error(cause=exc)

    cache_key = _cache_key(req.file_path, req.block_sec, req.hop_sec)
    cached = _metric_timeline_cache.get(cache_key)
    if cached is not None:
        return cached

    try:
        data = await run_blocking(_compute, req.file_path, req.block_sec, req.hop_sec)
    except Exception as exc:
        _log.exception("Metric timeline analysis failed")
        internal_error(cause=exc)

    _metric_timeline_cache[cache_key] = data
    return data
