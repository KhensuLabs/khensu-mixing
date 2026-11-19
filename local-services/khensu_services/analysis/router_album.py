# local-services/khensu_services/analysis/router_album.py
"""Album endpoints: cross-track consistency, platform normalization, and the
album master (sequencing, CD metadata, WAV / cue / DDP 2.0 export).

/analyze/album                 cross-track seams (loudness / tilt / crest)
/analyze/album/normalization   track-mode vs album-mode platform playback
/analyze/album/consistency     album-mode gain suggestions
/album/master/plan             validate a sequence + lay it on the CD frame grid
/album/master/start | /status/{id} | /{id}/cancel   render + export job
"""
from __future__ import annotations
import logging
import os
from concurrent.futures import Future
from typing import Any, Literal

from fastapi import APIRouter

from pydantic import Field

from khensu_services.analysis.models import AlbumRequest, AlbumResponse
from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import invalid_input, require_decode_budget, require_file
from khensu_services.common.executors import long_job_executor, run_blocking
from khensu_services.common.job_status import JobStatus
from khensu_services.common.jobs import (
    JobStore, job_begin, job_cancel_requested, job_settle, job_update, make_job_routes,
    request_fingerprint, start_job,
)
from khensu_services.common.lru import BoundedCache, file_cache_key

_log = logging.getLogger(__name__)
router = APIRouter()

_album_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=32)

# Same CD limit album_master.plan_sequence enforces, checked by the request
# model so an oversized list is a 422 before every track is probed (SVC-17).
MAX_ALBUM_TRACKS = 99


def _compute(req: AlbumRequest) -> dict[str, Any]:
    from khensu_services.analysis.album_assembly import analyze_album
    return analyze_album(req.track_paths)


def _require_tracks(paths: list[str]) -> None:
    if not paths:
        invalid_input("No tracks provided")
    for p in paths:
        require_file(p)


@router.post("/analyze/album", response_model=AlbumResponse, response_model_exclude_unset=True)
async def analyze_album_endpoint(req: AlbumRequest) -> AlbumResponse:
    _require_tracks(req.track_paths)

    cache_key = file_cache_key(req.track_paths, "album")
    cached = _album_cache.get(cache_key) if cache_key is not None else None
    if cached is not None:
        return AlbumResponse(**cached)

    # Failures reach the global handler (generic 500, or 422 for an
    # undecodable file); no per-route wrapper (S11).
    data = await run_blocking(_compute, req)
    if cache_key is not None:
        _album_cache[cache_key] = data
    return AlbumResponse(**data)


# --- Platform normalization simulation ----------------------------------------

class AlbumNormalizationRequest(RequestModel):
    track_paths: list[str] = Field(default_factory=list)
    platform: str = "spotify"


class AlbumNormalizationTrack(ResponseModel):
    name: str
    lufs: float | None = None
    track_mode_post_lufs: float | None = None
    album_mode_post_lufs: float | None = None
    track_mode_jump_lu: float | None = None
    album_mode_jump_lu: float | None = None


class AlbumNormalizationSummary(ResponseModel):
    max_album_mode_jump_lu: float
    biggest_jump_notable: bool
    flattened_in_track_mode: list[str] = Field(default_factory=list)


class AlbumNormalizationResponse(ResponseModel):
    platform: str
    target_lufs: float
    album_lufs: float | None = None
    album_gain_db: float
    tracks: list[AlbumNormalizationTrack]
    summary: AlbumNormalizationSummary


@router.post("/analyze/album/normalization", response_model=AlbumNormalizationResponse,
             response_model_exclude_unset=True)
async def album_normalization_endpoint(req: AlbumNormalizationRequest) -> dict:
    """Simulate track-mode vs album-mode platform normalization across an
    album's track list — the loudness jumps a listener hears between songs."""
    _require_tracks(req.track_paths)
    await require_decode_budget(*req.track_paths)
    from khensu_services.analysis.album_normalization import simulate_album_normalization
    return await run_blocking(simulate_album_normalization, req.track_paths, req.platform)


# --- Album-mode loudness consistency --------------------------------------------

class AlbumConsistencyRequest(RequestModel):
    track_paths: list[str] = Field(default_factory=list)
    max_spread_lu: float = 1.5
    anchor: Literal["median", "loudest"] = "median"
    true_peak_ceiling_dbtp: float = -1.0


class AlbumConsistencyTrack(ResponseModel):
    path: str
    name: str
    lufs: float | None = None
    short_term_max_lufs: float | None = None
    true_peak_dbtp: float | None = None
    crest_db: float | None = None
    gain_db: float
    capped: bool
    reason: str


class AlbumConsistencyResponse(ResponseModel):
    anchor: str
    anchor_lufs: float | None = None
    max_spread_lu: float
    true_peak_ceiling_dbtp: float | None = None
    spread_before_lu: float | None = None
    spread_after_lu: float | None = None
    tracks: list[AlbumConsistencyTrack]


@router.post("/analyze/album/consistency", response_model=AlbumConsistencyResponse,
             response_model_exclude_unset=True)
async def album_consistency_endpoint(req: AlbumConsistencyRequest) -> dict:
    """Album-mode trims: bring every track's loudest passage inside one window
    without normalising anything to a target or touching dynamics."""
    from khensu_services.analysis.album_master import AlbumSpecError, album_consistency
    _require_tracks(req.track_paths)
    await require_decode_budget(*req.track_paths)
    try:
        return await run_blocking(album_consistency, req.track_paths, req.max_spread_lu,
                                  req.anchor, req.true_peak_ceiling_dbtp)
    except AlbumSpecError as exc:
        invalid_input(str(exc))


# --- Album master: plan + export job ----------------------------------------------

class AlbumTrackSpecModel(RequestModel):
    path: str
    title: str = ""
    performer: str = ""
    isrc: str = ""
    gain_db: float = 0.0
    gap_sec: float = Field(default=2.0, ge=0.0, le=600.0)
    crossfade_sec: float = Field(default=0.0, ge=0.0, le=60.0)
    fade_in_sec: float = Field(default=0.0, ge=0.0, le=60.0)
    fade_out_sec: float = Field(default=0.0, ge=0.0, le=60.0)


class AlbumMasterSpecModel(RequestModel):
    tracks: list[AlbumTrackSpecModel] = Field(default_factory=list, max_length=MAX_ALBUM_TRACKS)
    title: str = ""
    performer: str = ""
    catalog: str = ""


class AlbumMasterStartRequest(AlbumMasterSpecModel):
    output_dir: str = ""          # default: <first track's dir>/album_master
    name: str = "album"
    wav: bool = True
    cue: bool = True
    ddp: bool = True
    cd_text: bool = True


class AlbumPlanTrack(ResponseModel):
    number: int
    path: str
    title: str
    performer: str
    isrc: str
    index00_msf: str
    index01_msf: str
    index00_frame: int
    index01_frame: int
    duration_sec: float
    gap_sec: float
    crossfade_sec: float
    gain_db: float


class AlbumPlanResponse(ResponseModel):
    title: str
    performer: str
    catalog: str
    total_frames: int
    total_msf: str
    total_sec: float
    warnings: list[str]
    tracks: list[AlbumPlanTrack]


class AlbumMasterStartResponse(ResponseModel):
    job_id: str
    total: int
    output_dir: str


class AlbumMasterProgress(ResponseModel):
    total: int
    done: int
    current: str | None = None


class AlbumRenderStats(ResponseModel):
    samples: int
    clipped_samples: int
    peak_dbfs: float | None = None


class AlbumMasterResult(ResponseModel):
    plan: AlbumPlanResponse
    render: AlbumRenderStats
    wav_path: str | None = None
    cue_path: str | None = None
    ddp_dir: str | None = None
    ddp_files: list[str] | None = None
    ddp_md5: dict[str, str] | None = None
    ddp_verified: bool | None = None
    ddp_problems: list[str] | None = None


class AlbumMasterStatusResponse(ResponseModel):
    status: JobStatus
    progress: AlbumMasterProgress
    output_dir: str
    result: AlbumMasterResult | None = None
    error: str | None = None


class AlbumMasterCancelResponse(ResponseModel):
    ok: bool
    status: str


def _spec_from_model(req: AlbumMasterSpecModel):
    from khensu_services.analysis.album_master import spec_from_dict
    return spec_from_dict(req.model_dump())


def _plan_or_422(req: AlbumMasterSpecModel):
    from khensu_services.analysis.album_master import AlbumSpecError, plan_sequence
    try:
        return plan_sequence(_spec_from_model(req))
    except AlbumSpecError as exc:
        invalid_input(str(exc))


@router.post("/album/master/plan", response_model=AlbumPlanResponse,
             response_model_exclude_unset=True)
async def album_master_plan(req: AlbumMasterSpecModel) -> dict:
    """Validate ISRCs / catalogue / CD-Text and return every index point on the
    CD frame grid, without rendering anything."""
    _require_tracks([t.path for t in req.tracks])
    plan = await run_blocking(_plan_or_422, req)
    return plan.to_dict()


_album_jobs = JobStore(name="album_master", max_active=2)
_album_futures: dict[str, Future] = {}


def _run_album_job(job_id: str, req: AlbumMasterStartRequest, output_dir: str) -> None:
    from khensu_services.analysis.album_master import AlbumCancelled, AlbumSpecError, plan_sequence
    from khensu_services.analysis.ddp_export import export_album

    if not job_begin(_album_jobs, job_id):
        return

    def progress(done: int, total: int, stage: str) -> bool:
        if job_cancel_requested(_album_jobs, job_id):
            return False
        job_update(_album_jobs, job_id, progress={"total": total, "done": done,
                                                  "current": None if stage == "done" else stage})
        return True

    try:
        plan = plan_sequence(_spec_from_model(req))
        result = export_album(plan, output_dir, req.name, wav=req.wav, cue=req.cue, ddp=req.ddp,
                              cd_text=req.cd_text, progress=progress)
        job_settle(_album_jobs, job_id, status=JobStatus.COMPLETED.value, result=result)
    except AlbumCancelled:
        job_settle(_album_jobs, job_id, status=JobStatus.CANCELLED.value)
    except AlbumSpecError as exc:
        job_settle(_album_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc))
    except Exception as exc:
        _log.exception("Album master job failed")
        job_settle(_album_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc))


_NAME_OK = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-.()")


@router.post("/album/master/start", response_model=AlbumMasterStartResponse,
             response_model_exclude_unset=True)
async def album_master_start(req: AlbumMasterStartRequest) -> dict:
    """Render the sequenced album once (44.1 kHz / 16-bit, TPDF dither at the
    final stage) and write the WAV, cue sheet and DDP 2.0 fileset."""
    from khensu_services.common.output_paths import reject_output_alias, validate_output_path

    paths = [t.path for t in req.tracks]
    _require_tracks(paths)
    await require_decode_budget(*paths)
    if not (req.wav or req.ddp):
        invalid_input("Choose at least one of the WAV or DDP outputs.")
    if not req.name.strip() or any(c not in _NAME_OK for c in req.name) or req.name.strip(".") == "":
        invalid_input("name must be a plain file name (letters, digits, space, _ - . ( )).")
    await run_blocking(_plan_or_422, req)   # 422 on a bad ISRC/UPC before any job exists

    output_dir = req.output_dir or os.path.join(os.path.dirname(paths[0]), "album_master")
    outputs = [os.path.join(output_dir, f"{req.name}.wav"), os.path.join(output_dir, f"{req.name}.cue"),
               os.path.join(output_dir, f"{req.name}_DDP", "IMAGE.DAT")]
    for o in outputs:
        validate_output_path(o, paths[0])
        reject_output_alias(o, *paths)

    stats = [os.stat(p) for p in paths]
    dedup_key = request_fingerprint(req.model_dump(), os.path.abspath(output_dir),
                                    [(s.st_mtime_ns, s.st_size) for s in stats])
    # A full album render + DDP export holds a worker for minutes: it runs on
    # the bounded long-job pool, never the interactive DSP pool (SVC-04), and
    # its future is tracked so a cancel while queued frees the slot at once.
    job_id, _created = start_job(
        _album_jobs, long_job_executor, _run_album_job, req, output_dir,
        payload={
            "status": JobStatus.PENDING.value,
            "progress": {"total": len(paths), "done": 0, "current": None},
            "output_dir": output_dir,
            "result": None,
            "error": None,
        }, dedup_key=dedup_key, futures=_album_futures)
    return {"job_id": job_id, "total": len(paths), "output_dir": output_dir}


album_master_status, album_master_cancel = make_job_routes(
    router, _album_jobs,
    status_path="/album/master/status/{job_id}",
    cancel_path="/album/master/{job_id}/cancel",
    status_model=AlbumMasterStatusResponse,
    cancel_model=AlbumMasterCancelResponse,
    futures=_album_futures)
