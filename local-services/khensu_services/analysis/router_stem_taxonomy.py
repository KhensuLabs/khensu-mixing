# local-services/khensu_services/analysis/router_stem_taxonomy.py
from __future__ import annotations
import logging
import os
from typing import Any

from fastapi import APIRouter
from khensu_services.common.errors import internal_error, not_found

from khensu_services.analysis.analyzer_responses import StemTaxonomyResponse
from khensu_services.analysis.models import StemTaxonomyRequest
from khensu_services.common.lru import BoundedCache
from khensu_services.common.executors import run_blocking
from khensu_services.util.genre_schema import load_genre_profile

_log = logging.getLogger(__name__)
router = APIRouter()

# Same mtime+size cheap-cache pattern as the file-analysis table (router_file_analyses.py), keyed
# on all four stem files.
_taxonomy_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=32)


def _cache_key(paths: list[str], genre_id: str) -> tuple:
    parts: list = [genre_id]
    for p in paths:
        st = os.stat(p)
        parts.extend((p, st.st_mtime_ns, st.st_size))
    # Same paths re-separated by Demucs instead of HPSS get a new label (AID-07).
    from khensu_services.analysis.separator import stems_separation_method
    parts.append(stems_separation_method({str(i): p for i, p in enumerate(paths)}))
    return tuple(parts)


def _load_genre_profile(genre_id: str) -> dict | None:
    """Return a single merged genre profile (base + user custom) by id."""
    if not genre_id:
        return None
    try:
        return load_genre_profile(genre_id)
    except Exception:
        _log.warning("Could not load genre profile for %s", genre_id, exc_info=True)
    return None


def _compute(vocals: str, drums: str, bass: str, other: str,
             genre: dict | None) -> dict[str, Any]:
    from khensu_services.analysis.stem_taxonomy import analyze_stem_taxonomy
    return analyze_stem_taxonomy(vocals, drums, bass, other, genre=genre)


@router.post("/analyze/stem_taxonomy", response_model=StemTaxonomyResponse, response_model_exclude_unset=True)
async def analyze_stem_taxonomy_endpoint(req: StemTaxonomyRequest) -> dict[str, Any]:
    paths = [req.vocals_path, req.drums_path, req.bass_path, req.other_path]
    for p in paths:
        if not os.path.isfile(p):
            not_found(f"Stem not found: {p}")

    cache_key = _cache_key(paths, req.genre_id)
    cached = _taxonomy_cache.get(cache_key)
    if cached is not None:
        return cached

    genre = _load_genre_profile(req.genre_id)
    try:
        data = await run_blocking(_compute, *paths, genre)
    except Exception as exc:
        _log.exception("Stem taxonomy analysis failed")
        internal_error(cause=exc)

    _taxonomy_cache[cache_key] = data
    return data
