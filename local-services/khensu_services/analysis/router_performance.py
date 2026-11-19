"""Musical timing, phase, room-tail, and harmony timeline endpoints."""
from __future__ import annotations

import os

from fastapi import APIRouter
from khensu_services.common.errors import invalid_input, not_found, too_large
from pydantic import Field

from khensu_services.common.api_models import RequestModel

from khensu_services.analysis.analyzer_responses import (
    GrooveTimingResponse, HarmonyTimelineResponse, ReverbTailResponse, StemPhaseResponse,
)
from khensu_services.common.audio_limits import AudioInputLimitError, AudioInputProbeError, validate_audio_decode_budget
from khensu_services.common.executors import run_blocking
from khensu_services.common.lru import BoundedCache, file_cache_key

router = APIRouter()

# Result cache (mtime+size keyed, like router_artifacts_scan): each of these
# re-decodes and re-tracks the whole file, and the dashboard asks repeatedly.
_perf_cache: BoundedCache[tuple, dict] = BoundedCache(max_entries=64)


async def _cached(name: str, paths, fn, *args) -> dict:
    ck = file_cache_key(paths, name)
    hit = _perf_cache.get(ck) if ck is not None else None
    if hit is not None:
        return hit
    data = await run_blocking(fn, *args)
    if ck is not None:
        _perf_cache[ck] = data
    return data


class AudioPathRequest(RequestModel):
    file_path: str


class StemPhaseRequest(RequestModel):
    stem_paths: dict[str, str] = Field(min_length=2, max_length=16)


async def _validate(*paths: str) -> None:
    for path in paths:
        if not os.path.isfile(path):
            not_found(f"File not found: {path}")
        try:
            await run_blocking(validate_audio_decode_budget, path)
        except AudioInputLimitError as exc:
            too_large(str(exc), cause=exc)
        except AudioInputProbeError as exc:
            invalid_input(f"Invalid audio file: {path}", cause=exc)


@router.post("/analyze/groove_timing", response_model=GrooveTimingResponse, response_model_exclude_unset=True)
async def groove_timing(req: AudioPathRequest) -> dict:
    from khensu_services.analysis.performance_analyzer import analyze_groove_timing
    await _validate(req.file_path)
    return await _cached("groove_timing", req.file_path, analyze_groove_timing, req.file_path)


@router.post("/analyze/stem_phase", response_model=StemPhaseResponse, response_model_exclude_unset=True)
async def stem_phase(req: StemPhaseRequest) -> dict:
    from khensu_services.analysis.performance_analyzer import analyze_stem_phase_correlation
    await _validate(*req.stem_paths.values())
    return await _cached("stem_phase", req.stem_paths, analyze_stem_phase_correlation,
                         req.stem_paths)


@router.post("/analyze/reverb_tail", response_model=ReverbTailResponse, response_model_exclude_unset=True)
async def reverb_tail(req: AudioPathRequest) -> dict:
    from khensu_services.analysis.performance_analyzer import estimate_reverb_tail
    await _validate(req.file_path)
    return await _cached("reverb_tail", req.file_path, estimate_reverb_tail, req.file_path)


@router.post("/analyze/harmony_timeline", response_model=HarmonyTimelineResponse, response_model_exclude_unset=True)
async def harmony_timeline(req: AudioPathRequest) -> dict:
    from khensu_services.analysis.performance_analyzer import analyze_harmony_timeline
    await _validate(req.file_path)
    return await _cached("harmony_timeline", req.file_path, analyze_harmony_timeline,
                         req.file_path)
