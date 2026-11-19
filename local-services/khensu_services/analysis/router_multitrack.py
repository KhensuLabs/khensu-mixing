# local-services/khensu_services/analysis/router_multitrack.py
"""Multitrack (stem-folder) session endpoints.

``POST /analyze/multitrack/scan``           list + name + validate a folder (fast, no decode)
``POST /analyze/multitrack/start``          analyze the stems as a background job
``GET  /analyze/multitrack/status/{id}``    progress / result
``POST /analyze/multitrack/{id}/cancel``    cooperative cancel
"""
from __future__ import annotations

import hashlib
import logging
import os
from typing import Any, Optional

from fastapi import APIRouter
from pydantic import Field

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.config import config_dir
from khensu_services.common.errors import check_decode_budget, invalid_input, not_found, require_file
from concurrent.futures import Future

from khensu_services.common.executors import long_job_executor as _executor
from khensu_services.common.executors import run_blocking
from khensu_services.common.job_status import JobStatus
from khensu_services.common.jobs import (
    JobStore, cancel_job, get_job_or_404, job_begin, job_cancel_requested, job_settle,
    request_fingerprint, start_job,
)
from khensu_services.common.local_paths import require_local_path

_log = logging.getLogger(__name__)
router = APIRouter()

_jobs = JobStore(name="multitrack", max_active=2)
# Multi-minute decode/measure work runs on the bounded long-job pool (SVC-04),
# and its futures are tracked so a cancel before start is immediate.
_job_futures: dict[str, Future] = {}


# --- Requests ----------------------------------------------------------------

class MultitrackScanRequest(RequestModel):
    folder: str


class MultitrackStartRequest(RequestModel):
    folder: str = ""                                          # either a folder ...
    file_paths: list[str] = Field(default_factory=list)       # ... or explicit stems
    track_names: list[str] = Field(default_factory=list)      # optional, same order as the stems
    write_mixdown: bool = True


# --- Responses ---------------------------------------------------------------

class StemFileInfo(ResponseModel):
    path: str
    file_name: str
    track_name: str
    role: str
    role_label: str
    role_source: str
    sample_rate: int
    channels: int
    duration_sec: float


class StemMismatch(ResponseModel):
    track: str
    path: str
    kind: str          # "sample_rate" | "length" | "channels"
    detail: str


class MultitrackScanResponse(ResponseModel):
    folder: str
    track_count: int
    session_sample_rate: int
    consistent: bool
    files: list[StemFileInfo]
    mismatches: list[StemMismatch]
    max_tracks: int


class MultitrackStartResponse(ResponseModel):
    job_id: str
    total: int
    created: bool


class TrackAdvice(ResponseModel):
    issue_code: str
    severity: str
    what: str
    suggested_action: str
    why_helps: str
    measured_value: Optional[float] = None
    unit: Optional[str] = None
    related_track: Optional[str] = None
    myth_clarification: Optional[str] = None


class MultitrackTrack(ResponseModel):
    name: str
    path: str
    file_name: str
    role: str
    role_label: str
    role_source: str
    role_confidence: float
    sample_rate: int
    channels: int
    duration_sec: float
    integrated_lufs: Optional[float] = None
    true_peak_dbtp: Optional[float] = None
    sample_peak_dbfs: Optional[float] = None
    crest_db: Optional[float] = None
    stereo_width: float
    correlation: Optional[float] = None
    low_end_width: float
    spectral_tilt_db_per_oct: Optional[float] = None
    band_profile: list[float]
    silent: bool
    advice: list[TrackAdvice]


class MultitrackMaskingPair(ResponseModel):
    track_a: str
    track_b: str
    collision_score: float
    band_index: int
    band_overlap: float
    level_diff_db: float
    risk: float
    band_lo_hz: float
    band_hi_hz: float
    band_label: str
    risk_level: str    # "high" | "moderate" | "low"
    summary: str


class MultitrackMix(ResponseModel):
    sample_rate: int
    integrated_lufs: Optional[float] = None
    true_peak_dbtp: Optional[float] = None
    sample_peak_dbfs: Optional[float] = None
    crest_db: Optional[float] = None
    stereo_width: Optional[float] = None
    correlation: Optional[float] = None
    duration_sec: Optional[float] = None
    headroom_ok: Optional[bool] = None
    mixdown_path: Optional[str] = None


class MultitrackResult(ResponseModel):
    track_count: int
    session_sample_rate: int
    consistent: bool
    mismatches: list[StemMismatch]
    tracks: list[MultitrackTrack]
    pairs: list[MultitrackMaskingPair]
    matrix_tracks: list[str]
    matrix: list[list[float]]
    bands_hz: list[float]
    mix: MultitrackMix
    # "demucs" | "hpss" | "unknown" (no separation.json - e.g. a real
    # multitrack export); `stem_note` explains an HPSS approximation (AID-07).
    stem_separation: Optional[str] = None
    stem_note: Optional[str] = None


class MultitrackProgress(ResponseModel):
    total: int
    done: int
    current: Optional[str] = None


class MultitrackStatusResponse(ResponseModel):
    status: JobStatus
    progress: MultitrackProgress
    result: Optional[MultitrackResult] = None
    error: Optional[str] = None


class MultitrackCancelResponse(ResponseModel):
    status: str


# --- Helpers -----------------------------------------------------------------

def _resolve_stems(folder: str, file_paths: list[str]) -> list[str]:
    from khensu_services.analysis.multitrack import MAX_TRACKS, list_stem_files

    if file_paths:
        for p in file_paths:
            require_file(p, "Stem")
        paths = list(file_paths)
    else:
        if not folder:
            invalid_input("Give a stem folder or a list of stem files.")
        require_local_path(folder, "Folder")
        if not os.path.isdir(folder):
            not_found(f"Folder not found: {folder}")
        paths = list_stem_files(folder)
    if not paths:
        invalid_input("No audio files found in the folder (WAV, FLAC, AIFF, OGG or MP3).")
    if len(paths) > MAX_TRACKS:
        invalid_input(f"{len(paths)} stems; a multitrack session takes at most {MAX_TRACKS}.")
    if len(set(os.path.normcase(os.path.abspath(p)) for p in paths)) != len(paths):
        invalid_input("The same stem is listed more than once.")
    return paths


def _scan(folder: str) -> dict[str, Any]:
    from khensu_services.analysis.multitrack import (
        MAX_TRACKS, derive_track_names, find_mismatches, probe_stems, role_from_name,
    )
    from khensu_services.analysis.stem_taxonomy import BUCKET_LABELS

    paths = _resolve_stems(folder, [])
    for p in paths:
        check_decode_budget(p)
    names = derive_track_names(paths)
    probes = probe_stems(paths)
    session_sr, mismatches = find_mismatches(probes, names)
    files = []
    for path, name, probe in zip(paths, names, probes):
        role = role_from_name(name)
        files.append({
            "path": path,
            "file_name": os.path.basename(path),
            "track_name": name,
            "role": role or "other",
            "role_label": BUCKET_LABELS.get(role or "other", "Other"),
            "role_source": "name" if role else "unknown",
            "sample_rate": probe["sample_rate"],
            "channels": probe["channels"],
            "duration_sec": round(probe["duration_sec"], 3),
        })
    return {"folder": folder, "track_count": len(paths), "session_sample_rate": session_sr,
            "consistent": not mismatches, "files": files, "mismatches": mismatches,
            "max_tracks": MAX_TRACKS}


def _mixdown_path(paths: list[str]) -> str:
    """Where the summed mix goes: the config dir, keyed by the stem set, so a
    re-run overwrites its own previous sum and never writes into the user's
    stem folder."""
    key = hashlib.sha256("\n".join(sorted(os.path.abspath(p) for p in paths)).encode("utf-8")).hexdigest()[:16]
    return str(config_dir() / "multitrack" / key / "khensu_mixdown.wav")


def _run_job(job_id: str, paths: list[str], names: list[str], mixdown: Optional[str]) -> None:
    from khensu_services.analysis.multitrack import analyze_multitrack

    job = _jobs.get(job_id)
    if job is None or not job_begin(_jobs, job_id):
        return

    def progress(stage: str, done: int, total: int) -> bool:
        if job_cancel_requested(_jobs, job_id):
            return False
        job["progress"]["done"] = done
        job["progress"]["current"] = stage
        return True

    try:
        for p in paths:
            check_decode_budget(p)
        result = analyze_multitrack(paths, names=names or None, mixdown_path=mixdown, progress_cb=progress)
        if result is None:
            job_settle(_jobs, job_id, status=JobStatus.CANCELLED.value)
            return
        job["progress"]["done"] = len(paths)
        job["progress"]["current"] = None
        job_settle(_jobs, job_id, status=JobStatus.COMPLETED.value, result=result)
    except Exception as exc:
        detail = getattr(exc, "detail", None) or str(exc)
        _log.exception("Multitrack analysis failed")
        job_settle(_jobs, job_id, status=JobStatus.FAILED.value, error=str(detail))


# --- Routes ------------------------------------------------------------------

@router.post("/analyze/multitrack/scan", response_model=MultitrackScanResponse,
             response_model_exclude_unset=True)
async def multitrack_scan(req: MultitrackScanRequest) -> dict[str, Any]:
    """List, name and validate the stems in a folder without decoding them."""
    return await run_blocking(_scan, req.folder)


@router.post("/analyze/multitrack/start", response_model=MultitrackStartResponse,
             response_model_exclude_unset=True)
async def multitrack_start(req: MultitrackStartRequest) -> dict[str, Any]:
    paths = await run_blocking(_resolve_stems, req.folder, req.file_paths)
    if req.track_names and len(req.track_names) != len(paths):
        invalid_input("track_names must give one name per stem.")
    mixdown = _mixdown_path(paths) if req.write_mixdown else None
    stats = []
    for p in paths:
        st = os.stat(p)
        stats.append((p, st.st_mtime_ns, st.st_size))
    dedup_key = request_fingerprint(stats, list(req.track_names), mixdown)
    job_id, created = start_job(
        _jobs, _executor, _run_job, paths, list(req.track_names), mixdown,
        payload={
            "status": JobStatus.PENDING.value,
            "progress": {"total": len(paths), "done": 0, "current": None},
            "result": None,
            "error": None,
        }, dedup_key=dedup_key, futures=_job_futures)
    return {"job_id": job_id, "total": len(paths), "created": created}


@router.get("/analyze/multitrack/status/{job_id}", response_model=MultitrackStatusResponse,
            response_model_exclude_unset=True)
async def multitrack_status(job_id: str) -> dict[str, Any]:
    return dict(get_job_or_404(_jobs, job_id))


@router.post("/analyze/multitrack/{job_id}/cancel", response_model=MultitrackCancelResponse,
             response_model_exclude_unset=True)
async def multitrack_cancel(job_id: str) -> dict[str, Any]:
    return {"status": cancel_job(_jobs, job_id, _job_futures)}
