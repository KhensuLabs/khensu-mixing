from __future__ import annotations
import logging
import os
from typing import Any

from fastapi import APIRouter
from khensu_services.common.errors import internal_error, invalid_input, not_found

from khensu_services.analysis.models import MaskingRequest, MaskingResponse
from khensu_services.common.lru import BoundedCache
from khensu_services.common.executors import run_blocking

_log = logging.getLogger(__name__)
router = APIRouter()

# Cheap cache keyed by each stem's (path, mtime, size) — avoids re-running a
# full librosa.load() of all 4 stems (a real CPU spike on longer tracks) when
# the caller re-requests the same, unchanged stems. Uses mtime+size instead of
# a content hash so unchanged files never need to be re-read just to key the
# cache. Bounded (audit ARCH-04) even though normal session usage keeps it
# small in practice — a bug or unusual stem-swapping workflow shouldn't be
# able to grow it without limit.
_masking_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=32)


def _cache_key(stem_paths: dict[str, str]) -> tuple:
    parts = []
    for name in sorted(stem_paths):
        path = stem_paths[name]
        st = os.stat(path)
        parts.append((name, path, st.st_mtime_ns, st.st_size))
    return tuple(parts)


@router.post("/analyze/masking", response_model=MaskingResponse, response_model_exclude_unset=True)
async def analyze_masking(req: MaskingRequest) -> MaskingResponse:
    stem_paths = {
        "vocals": req.vocals_path,
        "drums": req.drums_path,
        "bass": req.bass_path,
        "other": req.other_path,
    }
    stem_paths = {name: path for name, path in stem_paths.items() if path}
    if len(stem_paths) < 2:
        invalid_input("At least 2 stem paths are required")
    for name, path in stem_paths.items():
        if not os.path.isfile(path):
            not_found(f"{name} stem not found: {path}")

    cache_key = _cache_key(stem_paths)
    cached = _masking_cache.get(cache_key)
    if cached is not None:
        return MaskingResponse(**cached)

    from khensu_services.analysis.masking import compute_stem_masking
    try:
        data = await run_blocking(compute_stem_masking, stem_paths)
    except Exception as exc:
        _log.exception("Masking analysis failed")
        internal_error(cause=exc)

    _masking_cache[cache_key] = data
    return MaskingResponse(**data)
