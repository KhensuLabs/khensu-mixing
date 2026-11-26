# local-services/khensu_services/analysis/router_reference_scan.py
"""REFS-1: the reference folder scan (``analysis/reference_scan.py``).

/references/scan/roots              GET the scan folders, POST add / set one
/references/scan/roots/remove       POST forget a folder (and its items)
/references/scan/start              POST scan the folders (a long job)
/references/scan/status/{job_id}    GET progress: files done / total
/references/scan/{job_id}/cancel    POST stop it
/references/scan/current            GET the running (or last) scan job
/references/scan/items              GET songs and samples, filtered
/references/scan/packs              GET the sample packs, filtered
/references/scan/summary            GET counts: songs, samples, packs, per genre
/references/scan/items/genre        POST the user's genre for an item (or its pack)
/references/scan/thumbnail/{id}     GET an item's artwork thumbnail (JPEG, base64)
"""
from __future__ import annotations

import logging
import os
import threading
from concurrent.futures import Future
from typing import Any, Literal, Optional

from fastapi import APIRouter, Query
from pydantic import Field

from khensu_services.common.api_models import OpenResponseModel, RequestModel, ResponseModel
from khensu_services.common.errors import (
    forbidden_path,
    invalid_input,
    not_found,
)
from khensu_services.common.executors import long_job_executor, run_io_blocking
from khensu_services.common.job_status import JobStatus
from khensu_services.common.jobs import (
    JobStore,
    job_begin,
    job_cancel_requested,
    job_settle,
    job_update,
    make_job_routes,
    request_fingerprint,
    start_job,
)

_log = logging.getLogger(__name__)
router = APIRouter()

_scan_jobs = JobStore(name="reference_scan", max_active=1)
_scan_futures: dict[str, Future] = {}
_last_job: dict[str, str] = {}


# --- models ---------------------------------------------------------------------

class ReferenceScanRoot(ResponseModel):
    path: str
    added_at: str
    last_scan_at: str
    rescan_on_launch: bool
    exists: bool
    items: int


class ReferenceScanRootsResponse(ResponseModel):
    roots: list[ReferenceScanRoot]


class ReferenceScanRootRequest(RequestModel):
    path: str = Field(..., min_length=1, max_length=1024)
    rescan_on_launch: Optional[bool] = None
    #: Make this the only scan folder (the screen's "Reference folder").
    replace: bool = False


class ReferenceScanRootRemoveRequest(RequestModel):
    path: str = Field(..., min_length=1, max_length=1024)
    forget_items: bool = True


class ReferenceScanRootRemoveResponse(ResponseModel):
    removed: bool
    forgotten_items: int


class ReferenceScanStartRequest(RequestModel):
    #: Folders to scan; empty = every saved scan folder.
    roots: list[str] = Field(default_factory=list, max_length=32)
    #: Add new / changed songs to the reference library (embeddings index).
    index_songs: bool = True
    max_files: int = Field(50000, ge=1, le=500000)


class ReferenceScanStartResponse(ResponseModel):
    job_id: str
    created: bool
    roots: list[str]


class ReferenceScanProgress(ResponseModel):
    phase: str = "pending"
    done: int = 0
    total: int = 0
    current: str = ""
    songs: int = 0
    samples: int = 0
    failed: int = 0
    unchanged: int = 0
    indexed: int = 0


class ReferenceScanResult(ResponseModel):
    files_found: int
    processed: int
    unchanged: int
    songs: int
    samples: int
    failed: int
    removed: int
    indexed: int
    cancelled: bool
    truncated: bool


class ReferenceScanStatusResponse(ResponseModel):
    status: str
    roots: list[str] = []
    progress: Optional[ReferenceScanProgress] = None
    result: Optional[ReferenceScanResult] = None
    error: Optional[str] = None


class ReferenceScanCurrentResponse(ResponseModel):
    job_id: Optional[str] = None
    status: Optional[str] = None


class ReferenceScanItem(ResponseModel):
    id: str
    root: str
    path: str
    kind: str
    sample_type: str
    title: str
    author: str
    album: str
    pack: str
    pack_path: str
    artwork_path: str
    has_artwork: bool
    genre: str
    family: str
    genre_source: str
    genre_confidence: float
    tag_genre: str
    bpm: float
    key: str
    duration_s: float
    indexed: bool
    exists: bool
    error: str


class ReferenceScanItemsResponse(ResponseModel):
    items: list[ReferenceScanItem]
    total: int


class ReferenceScanPack(ResponseModel):
    pack: str
    pack_path: str
    author: str
    samples: int
    loops: int
    artwork_path: str
    has_artwork: bool
    genre: str
    family: str


class ReferenceScanPacksResponse(ResponseModel):
    packs: list[ReferenceScanPack]


class ReferenceScanCounts(OpenResponseModel):
    """Items per catalog genre id / family id."""


class ReferenceScanSummaryResponse(ResponseModel):
    songs: int
    samples: int
    packs: int
    unsorted: int
    genres: ReferenceScanCounts
    families: ReferenceScanCounts


class ReferenceScanGenreRequest(RequestModel):
    id: str = Field(..., min_length=1, max_length=64)
    #: A catalog genre id or family id; "" clears the genre.
    genre: str = Field("", max_length=80)
    #: For a sample: give its whole pack this genre.
    apply_to_pack: bool = False
    #: For a song: store it as a genre example the probe learns from (GEN-1e).
    learn: bool = True


class ReferenceScanGenreResponse(ResponseModel):
    item: ReferenceScanItem
    updated: int
    learned: bool


class ReferenceScanThumbnailResponse(ResponseModel):
    id: str
    path: str
    width: int
    height: int
    jpeg_base64: str


# --- folders --------------------------------------------------------------------

def _check_root(path: str) -> str:
    """A folder the user chose: local, existing, not the service's own config dir."""
    from khensu_services.common.config import config_dir
    from khensu_services.common.local_paths import require_local_path
    require_local_path(path, "Reference folder")          # 403 before anything stats it
    real = os.path.realpath(os.path.abspath(path))
    if not os.path.exists(real):
        not_found(f"Reference folder not found: {path}")
    if not os.path.isdir(real):
        invalid_input(f"Not a folder: {path}")
    cfg = os.path.normcase(os.path.realpath(str(config_dir())))
    norm = os.path.normcase(real)
    if norm == cfg or norm.startswith(cfg + os.sep):
        forbidden_path(f"Reference folder inside the service configuration directory: {path}")
    return os.path.normpath(os.path.abspath(path))


@router.get("/references/scan/roots", response_model=ReferenceScanRootsResponse,
            response_model_exclude_unset=True)
async def reference_scan_roots() -> dict[str, Any]:
    from khensu_services.analysis import reference_scan as rs
    return {"roots": await run_io_blocking(rs.list_roots)}


@router.post("/references/scan/roots", response_model=ReferenceScanRoot, response_model_exclude_unset=True)
async def reference_scan_set_root(req: ReferenceScanRootRequest) -> dict[str, Any]:
    """Add a scan folder (``replace``: make it the only one)."""
    from khensu_services.analysis import reference_scan as rs
    path = _check_root(req.path)
    return await run_io_blocking(rs.set_root, path, req.rescan_on_launch, replace=req.replace)


@router.post("/references/scan/roots/remove", response_model=ReferenceScanRootRemoveResponse,
             response_model_exclude_unset=True)
async def reference_scan_remove_root(req: ReferenceScanRootRemoveRequest) -> dict[str, Any]:
    from khensu_services.analysis import reference_scan as rs
    roots = {r["path"] for r in await run_io_blocking(rs.list_roots)}
    path = rs.canonical_root(req.path)
    if path not in roots:
        return {"removed": False, "forgotten_items": 0}
    n = await run_io_blocking(rs.remove_root, path, req.forget_items)
    return {"removed": True, "forgotten_items": n}


# --- the scan job -----------------------------------------------------------------

def _run_scan(job_id: str, roots: list[str], index_songs: bool, max_files: int) -> None:
    from khensu_services.analysis import reference_scan as rs
    if not job_begin(_scan_jobs, job_id):
        return
    try:
        result = rs.scan(roots, index_songs=index_songs, max_files=max_files,
                         progress=lambda p: job_update(_scan_jobs, job_id, progress=p),
                         cancelled=lambda: job_cancel_requested(_scan_jobs, job_id))
        status = JobStatus.CANCELLED.value if result["cancelled"] else JobStatus.COMPLETED.value
        job_settle(_scan_jobs, job_id, status=status, result=result)
    except Exception as exc:  # noqa: BLE001 - the job reports it
        _log.exception("Reference scan failed")
        job_settle(_scan_jobs, job_id, status=JobStatus.FAILED.value, error=f"{type(exc).__name__}: {exc}"[:300])


def start_scan(roots: list[str], index_songs: bool = True, max_files: int = 50000) -> tuple[str, bool]:
    """Admit and submit a scan of ``roots`` (429 while another runs)."""
    dedup = request_fingerprint("reference_scan", sorted(roots), index_songs, max_files)
    job_id, created = start_job(
        _scan_jobs, long_job_executor, _run_scan, roots, index_songs, max_files,
        payload={"status": JobStatus.PENDING.value, "roots": roots,
                 "progress": {"phase": "pending", "done": 0, "total": 0, "current": ""},
                 "result": None, "error": None},
        dedup_key=dedup, futures=_scan_futures)
    _last_job["id"] = job_id
    return job_id, created


@router.post("/references/scan/start", response_model=ReferenceScanStartResponse,
             response_model_exclude_unset=True)
async def reference_scan_start(req: ReferenceScanStartRequest) -> dict[str, Any]:
    """Walk the scan folders: sort songs and samples, read tags and artwork,
    give each a genre. Files unchanged since the last scan are skipped."""
    from khensu_services.analysis import reference_scan as rs
    saved = [r["path"] for r in await run_io_blocking(rs.list_roots)]
    if req.roots:
        roots = []
        for p in req.roots:
            path = _check_root(p)
            if path not in saved:
                await run_io_blocking(rs.set_root, path, None)
            roots.append(path)
    else:
        roots = [p for p in saved if os.path.isdir(p)]
        for p in roots:
            _check_root(p)
    if not roots:
        invalid_input("No reference folder to scan: choose a folder first.")
    job_id, created = start_scan(roots, req.index_songs, req.max_files)
    return {"job_id": job_id, "created": created, "roots": roots}


make_job_routes(router, _scan_jobs,
                status_path="/references/scan/status/{job_id}",
                cancel_path="/references/scan/{job_id}/cancel",
                status_model=ReferenceScanStatusResponse,
                futures=_scan_futures,
                not_found_detail="Reference scan not found")


@router.get("/references/scan/current", response_model=ReferenceScanCurrentResponse,
            response_model_exclude_unset=True)
async def reference_scan_current() -> dict[str, Any]:
    """The running scan, else the last one still remembered (a screen opened
    mid-scan, or after a launch rescan, follows it)."""
    with _scan_jobs._lock:
        for jid, job in _scan_jobs.items():
            if job.get("status") in (JobStatus.PENDING.value, JobStatus.RUNNING.value):
                return {"job_id": jid, "status": job.get("status")}
        jid = _last_job.get("id")
        if jid and jid in _scan_jobs:
            return {"job_id": jid, "status": _scan_jobs[jid].get("status")}
    return {}


# --- results ------------------------------------------------------------------------

Kind = Literal["song", "sample"]


@router.get("/references/scan/items", response_model=ReferenceScanItemsResponse,
            response_model_exclude_unset=True)
async def reference_scan_items(genre: Optional[str] = Query(None, max_length=80),
                               family: Optional[str] = Query(None, max_length=80),
                               kind: Optional[Kind] = None,
                               pack: Optional[str] = Query(None, max_length=1024),
                               author: Optional[str] = Query(None, max_length=200),
                               text: Optional[str] = Query(None, max_length=200),
                               unsorted: bool = False,
                               limit: int = Query(200, ge=1, le=2000),
                               offset: int = Query(0, ge=0)) -> dict[str, Any]:
    from khensu_services.analysis import reference_scan as rs
    return await run_io_blocking(lambda: rs.list_items(genre=genre, family=family, kind=kind, pack=pack,
                                                       author=author, text=text, unsorted=unsorted,
                                                       limit=limit, offset=offset))


@router.get("/references/scan/packs", response_model=ReferenceScanPacksResponse,
            response_model_exclude_unset=True)
async def reference_scan_packs(genre: Optional[str] = Query(None, max_length=80),
                               family: Optional[str] = Query(None, max_length=80),
                               author: Optional[str] = Query(None, max_length=200),
                               text: Optional[str] = Query(None, max_length=200),
                               unsorted: bool = False,
                               limit: int = Query(200, ge=1, le=2000)) -> dict[str, Any]:
    from khensu_services.analysis import reference_scan as rs
    packs = await run_io_blocking(lambda: rs.list_packs(genre=genre, family=family, author=author, text=text,
                                                        unsorted=unsorted, limit=limit))
    return {"packs": packs}


@router.get("/references/scan/summary", response_model=ReferenceScanSummaryResponse,
            response_model_exclude_unset=True)
async def reference_scan_summary() -> dict[str, Any]:
    from khensu_services.analysis import reference_scan as rs
    return await run_io_blocking(rs.summary)


@router.post("/references/scan/items/genre", response_model=ReferenceScanGenreResponse,
             response_model_exclude_unset=True)
async def reference_scan_set_genre(req: ReferenceScanGenreRequest) -> dict[str, Any]:
    """Correct an item's genre (a song's correction is also a genre example)."""
    from khensu_services.analysis import reference_scan as rs
    from khensu_services.common.executors import run_blocking
    try:
        # run_blocking: storing a genre example embeds the track (DSP).
        return await run_blocking(rs.set_item_genre, req.id, req.genre.strip(),
                                  apply_to_pack=req.apply_to_pack, learn=req.learn)
    except KeyError:
        not_found(f"Scanned item not found: {req.id}")
    except ValueError as exc:
        invalid_input(str(exc), cause=exc)


@router.get("/references/scan/thumbnail/{item_id}", response_model=ReferenceScanThumbnailResponse,
            response_model_exclude_unset=True)
async def reference_scan_thumbnail(item_id: str) -> dict[str, Any]:
    from khensu_services.analysis import reference_scan as rs
    item = await run_io_blocking(rs.get_item, item_id)
    if item is None:
        not_found(f"Scanned item not found: {item_id}")
    if not item["artwork_path"]:
        not_found(f"No artwork for {item_id}")
    payload = await run_io_blocking(rs.thumbnail_payload, item["artwork_path"])
    if payload is None:
        not_found(f"Artwork thumbnail missing for {item_id}")
    return {"id": item_id, "path": item["artwork_path"], **payload}


# --- launch rescan --------------------------------------------------------------------

def schedule_launch_rescan(delay_s: float = 45.0) -> threading.Timer | None:
    """Rescan the folders marked "rescan on launch", ``delay_s`` after the
    service starts (so it never competes with the launch's own analysis).
    Called from the service lifespan, which cancels the returned timer on
    shutdown; never raises."""
    if os.environ.get("KHENSU_DISABLE_LAUNCH_RESCAN", "").strip() == "1":
        return None

    def go() -> None:
        try:
            from khensu_services.analysis import reference_scan as rs
            roots = [r["path"] for r in rs.list_roots() if r["rescan_on_launch"] and r["exists"]]
            if roots:
                start_scan(roots)
        except Exception:  # noqa: BLE001 - a launch convenience, never fatal
            _log.warning("Launch rescan of the reference folders failed", exc_info=True)

    t = threading.Timer(delay_s, go)
    t.daemon = True
    t.start()
    return t
