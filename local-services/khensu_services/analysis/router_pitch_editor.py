# local-services/khensu_services/analysis/router_pitch_editor.py
from __future__ import annotations
import logging
import os
from typing import Any

from fastapi import APIRouter
from khensu_services.common.errors import internal_error, invalid_input, not_found

from khensu_services.analysis.models import (
    PitchCorrectRequest, PitchCorrectResponse, PitchCurveRequest, PitchCurveResponse,
)
from khensu_services.common.lru import BoundedCache
from khensu_services.common.output_paths import resolve_render_output
from khensu_services.common.executors import run_blocking

_log = logging.getLogger(__name__)
router = APIRouter()

# pYIN is slow; cache the curve on (path, mtime, size, fmin, fmax).
_curve_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=16)


@router.post("/analyze/pitch_curve", response_model=PitchCurveResponse, response_model_exclude_unset=True)
async def analyze_pitch_curve_endpoint(req: PitchCurveRequest) -> dict[str, Any]:
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    if not (0.0 < req.fmin < req.fmax):
        invalid_input("Require 0 < fmin < fmax")

    st = os.stat(req.file_path)
    key = (req.file_path, st.st_mtime_ns, st.st_size, req.fmin, req.fmax, req.reference_hz)
    cached = _curve_cache.get(key)
    if cached is not None:
        return cached

    from khensu_services.analysis.pitch_editor import analyze_pitch_curve
    try:
        data = await run_blocking(analyze_pitch_curve, req.file_path, req.fmin, req.fmax,
                                  req.reference_hz)
    except Exception as exc:
        _log.exception("Pitch curve analysis failed")
        internal_error(cause=exc)

    _curve_cache[key] = data
    return data


@router.post("/render/pitch_correct", response_model=PitchCorrectResponse, response_model_exclude_unset=True)
async def render_pitch_correct_endpoint(req: PitchCorrectRequest) -> dict[str, Any]:
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    out = resolve_render_output(req.output_path, req.file_path, "pitchcorrected")

    from khensu_services.analysis.pitch_editor import render_pitch_correct
    corrections = [c.model_dump() for c in req.corrections]
    try:
        return await run_blocking(render_pitch_correct, req.file_path, corrections,
                                  req.crossfade_ms, out)
    except Exception as exc:
        _log.exception("Pitch correction render failed")
        internal_error("Internal render error — see the service log for details.", cause=exc)
