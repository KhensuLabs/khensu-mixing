# local-services/khensu_services/analysis/router_gain_rider.py
from __future__ import annotations
import logging
import os
from typing import Any

from fastapi import APIRouter
from khensu_services.common.errors import internal_error, not_found

from khensu_services.analysis.models import GainRiderRequest, GainRiderResponse
from khensu_services.common.lru import BoundedCache
from khensu_services.common.executors import run_blocking
from khensu_services.common.output_paths import resolve_render_output

_log = logging.getLogger(__name__)
router = APIRouter()

# Analysis-only calls are cached (mtime+size); render calls always run since
# they produce a file on disk.
_gain_rider_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=32)


def _cache_key(req: GainRiderRequest) -> tuple:
    st = os.stat(req.file_path)
    return (req.file_path, st.st_mtime_ns, st.st_size,
            round(req.target_rms_db, 2), round(req.max_gain_db, 2))


def _compute(req: GainRiderRequest, output_path: str | None) -> dict[str, Any]:
    from khensu_services.analysis.gain_rider import compute_gain_ride
    return compute_gain_ride(req.file_path, req.target_rms_db,
                             req.max_gain_db, req.render, output_path)


@router.post("/analyze/gain_rider", response_model=GainRiderResponse, response_model_exclude_unset=True)
async def analyze_gain_rider(req: GainRiderRequest) -> GainRiderResponse:
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")

    cache_key = _cache_key(req)
    if not req.render:
        cached = _gain_rider_cache.get(cache_key)
        if cached is not None:
            return GainRiderResponse(**cached)

    # UIX-11: a render goes to the caller's (unique) output_path when given -
    # validated against the allowed roots and never aliasing the source - so
    # a second ride does not overwrite a file the render history still
    # references; without one, the old <file>_gainride.wav default.
    output_path = (resolve_render_output(req.output_path, req.file_path, "gainride",
                                         allowed_exts=(".wav",))
                   if req.render else None)

    try:
        data = await run_blocking(_compute, req, output_path)
    except Exception as exc:
        _log.exception("Gain rider analysis failed")
        internal_error(cause=exc)

    if not req.render:
        _gain_rider_cache[cache_key] = data
    return GainRiderResponse(**data)
