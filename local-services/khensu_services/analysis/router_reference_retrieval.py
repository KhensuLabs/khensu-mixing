# local-services/khensu_services/analysis/router_reference_retrieval.py
from __future__ import annotations
from typing import Any

from fastapi import APIRouter

from khensu_services.analysis.analyzer_responses import ReferenceMatchResponse
from khensu_services.analysis.models import ReferenceMatchRequest
from khensu_services.common.errors import require_file
from khensu_services.common.lru import BoundedCache, file_cache_key
from khensu_services.common.executors import run_blocking

router = APIRouter()

_match_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=32)


def _compute(file_path: str, genre_id: str | None, k: int,
             include_sections: bool) -> dict[str, Any]:
    from khensu_services.analysis.reference_retrieval import analyze_reference_match
    return analyze_reference_match(file_path, genre_id=genre_id, k=k,
                                   include_sections=include_sections)


@router.post("/analyze/reference_match", response_model=ReferenceMatchResponse, response_model_exclude_unset=True)
async def analyze_reference_match_endpoint(req: ReferenceMatchRequest) -> dict[str, Any]:
    require_file(req.file_path)

    genre_id = req.genre_id or None
    cache_key = file_cache_key(req.file_path, genre_id or "", req.k, req.include_sections)
    cached = _match_cache.get(cache_key) if cache_key is not None else None
    if cached is not None:
        return cached

    # Failures reach the global handler (S11): no per-route except/500.
    data = await run_blocking(_compute, req.file_path, genre_id, req.k,
                              req.include_sections)
    if cache_key is not None:
        _match_cache[cache_key] = data
    return data
