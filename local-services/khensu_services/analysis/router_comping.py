# local-services/khensu_services/analysis/router_comping.py
from __future__ import annotations
import logging
import math
import os
from typing import Any

from fastapi import APIRouter
from khensu_services.common.errors import internal_error, invalid_input, not_found

from khensu_services.analysis.models import CompingRequest, CompingResponse
from khensu_services.common.lru import BoundedCache
from khensu_services.common.executors import run_blocking
from khensu_services.common.output_paths import resolve_render_output

_log = logging.getLogger(__name__)
router = APIRouter()

# Analysis-only calls cached; render calls always run (they write a file).
_comping_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=32)


def _cache_key(req: CompingRequest) -> tuple:
    parts: list[Any] = []
    for p in req.take_paths:
        st = os.stat(p)
        parts.extend((p, st.st_mtime_ns, st.st_size))
    parts.append(req.reference_hz)
    return tuple(parts)


def _compute(req: CompingRequest, output_path: str | None) -> dict[str, Any]:
    from khensu_services.analysis.comping import analyze_comping
    return analyze_comping(req.take_paths, req.render, output_path, req.reference_hz)


@router.post("/analyze/comping", response_model=CompingResponse, response_model_exclude_unset=True)
async def analyze_comping_endpoint(req: CompingRequest) -> CompingResponse:
    if len(req.take_paths) < 2:
        invalid_input("Comping needs at least 2 takes")
    for p in req.take_paths:
        if not os.path.isfile(p):
            not_found(f"File not found: {p}")

    if req.reference_hz is not None and not (math.isfinite(req.reference_hz)
                                             and 380.0 <= req.reference_hz <= 500.0):
        invalid_input("reference_hz must be within 380..500")
    # UIX-11: the caller's render path (e.g. a uniqueRenderFile) goes through
    # the same allowed-roots / no-source-alias gate as every render route;
    # absent, the default <take1>_comp.wav beside the first take.
    output_path: str | None = None
    if req.render:
        output_path = resolve_render_output(req.output_path, req.take_paths[0], "comp",
                                            allowed_exts=(".wav",),
                                            extra_sources=tuple(req.take_paths[1:]))

    cache_key = _cache_key(req)
    if not req.render:
        cached = _comping_cache.get(cache_key)
        if cached is not None:
            return CompingResponse(**cached)

    try:
        data = await run_blocking(_compute, req, output_path)
    except Exception as exc:
        _log.exception("Comping analysis failed")
        internal_error(cause=exc)

    if not req.render:
        _comping_cache[cache_key] = data
    return CompingResponse(**data)
