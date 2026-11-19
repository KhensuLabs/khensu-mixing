# local-services/khensu_services/analysis/router_pitch_accuracy.py
from __future__ import annotations
from khensu_services.analysis.context import load_audio
import logging
import os
from typing import Any

from fastapi import APIRouter
from khensu_services.common.errors import internal_error, invalid_input, not_found

from khensu_services.analysis.analyzer_responses import PitchAccuracyResponse
from khensu_services.analysis.models import PitchAccuracyRequest
from khensu_services.common.lru import BoundedCache
from khensu_services.common.executors import run_blocking

_log = logging.getLogger(__name__)
router = APIRouter()

# Same mtime+size cheap-cache pattern as the file-analysis table (router_file_analyses.py).
_pitch_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=32)


def _cache_key(req: PitchAccuracyRequest) -> tuple:
    st = os.stat(req.file_path)
    return (req.file_path, st.st_mtime_ns, st.st_size, req.fmin, req.fmax, req.reference_hz)


def _compute(req: PitchAccuracyRequest) -> dict[str, Any]:
    from khensu_services.analysis.pitch_accuracy import analyze_pitch_accuracy
    y, sr = load_audio(req.file_path, sr=None, mono=False)
    return analyze_pitch_accuracy(y, int(sr), fmin=req.fmin, fmax=req.fmax,
                                  reference_hz=req.reference_hz)


@router.post("/analyze/pitch_accuracy", response_model=PitchAccuracyResponse, response_model_exclude_unset=True)
async def analyze_pitch_accuracy_endpoint(req: PitchAccuracyRequest) -> dict[str, Any]:
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    if not (req.fmin < req.fmax):
        invalid_input("Require fmin < fmax")

    cache_key = _cache_key(req)
    cached = _pitch_cache.get(cache_key)
    if cached is not None:
        return cached

    try:
        data = await run_blocking(_compute, req)
    except Exception as exc:
        _log.exception("Pitch accuracy analysis failed")
        internal_error(cause=exc)

    _pitch_cache[cache_key] = data
    return data
