# local-services/khensu_services/copilot/router_platform_masters.py
"""Platform-masters job endpoints (audit CQ-08 split from copilot/router.py).

Renders the one-click platform-target master set (Spotify -14 / Apple -16 /
club -9 / YouTube) as a background job:
/platform_masters/start | /status/{id} | /{id}/cancel.
"""
from __future__ import annotations

import logging
import os

from fastapi import APIRouter
from pydantic import Field

from khensu_services.common.api_models import RequestModel
from khensu_services.common.job_status import JobStatus

from khensu_services.common.errors import invalid_input, require_decode_budget, require_file
from khensu_services.common.executors import long_job_executor as _executor
from khensu_services.common.jobs import (
    JobStore, cancel_reply, get_job_or_404, job_begin, job_cancel_requested, job_settle,
    request_fingerprint, start_job,
)
from khensu_services.copilot.response_models import (
    CopilotJobCancelResponse, PlatformMastersStartResponse, PlatformMastersStatusResponse,
)

_log = logging.getLogger(__name__)
router = APIRouter()


class PlatformMastersRequest(RequestModel):
    file_path: str
    output_dir: str = ""                              # default: <source dir>/platform_masters
    platforms: list[str] = Field(default_factory=list)  # empty = the full default set
    # CUT-9: by default each variant keeps the master's loudness and is only
    # made safe for the platform's true-peak ceiling; True brings each one to
    # the platform's loudness target as well.
    match_loudness: bool = False


_platform_jobs = JobStore(name="platform_masters", max_active=4)  # bounded (F2) + admission cap (ARCH-01)
_platform_futures: dict = {}


def _run_platform_job(job_id: str, file_path: str, output_dir: str,
                      platforms: list[str], match_loudness: bool = False) -> None:
    from khensu_services.copilot.platform_render import render_platform_set

    job = _platform_jobs.get(job_id)
    if job is None or not job_begin(_platform_jobs, job_id):
        return

    def progress(pid: str, done: int, total: int) -> bool:
        if job_cancel_requested(_platform_jobs, job_id):
            return False
        job["progress"]["done"] = done
        job["progress"]["current"] = None if pid == "done" else pid
        return True

    try:
        results = render_platform_set(file_path, output_dir, platforms, progress_cb=progress,
                                      match_loudness=match_loudness)
        # Partial results stay visible even after a cancel.
        job["results"] = results
        job_settle(_platform_jobs, job_id, status=JobStatus.COMPLETED.value)
    except Exception as exc:
        _log.exception("Platform masters job failed for %s", file_path)
        job_settle(_platform_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc))


@router.post("/platform_masters/start", response_model=PlatformMastersStartResponse, response_model_exclude_unset=True)
async def platform_masters_start(req: PlatformMastersRequest) -> dict:
    """Render the one-click platform set (Spotify / Apple / club / YouTube):
    each true-peak safe for the platform and through the post-render
    guardrail. By default each keeps the master's loudness (a safety variant,
    CUT-9); ``match_loudness`` also normalises to the platform's target."""
    from khensu_services.copilot.platform_render import DEFAULT_PLATFORMS, resolve_platform_id

    require_file(req.file_path)
    await require_decode_budget(req.file_path)

    platforms = req.platforms or list(DEFAULT_PLATFORMS)
    unknown = [p for p in platforms if resolve_platform_id(p) is None]
    if unknown:
        invalid_input("Unknown platform target(s): " + ", ".join(unknown))

    output_dir = req.output_dir or os.path.join(os.path.dirname(req.file_path), "platform_masters")
    from khensu_services.common.output_paths import validate_output_path
    validate_output_path(os.path.join(output_dir, "probe.wav"), req.file_path)

    # Dedup (audit ARCH-01): the same work joins the already-running render
    # instead of starting another. "Same work" includes the platform set and
    # the source's mtime+size (AUDIT-072) - a different selection, or a file
    # re-rendered in place, is a new job.
    st = os.stat(req.file_path)
    dedup_key = request_fingerprint(req.file_path, os.path.abspath(output_dir),
                                    sorted(platforms), st.st_mtime_ns, st.st_size, req.match_loudness)
    job_id, _created = start_job(
        _platform_jobs, _executor, _run_platform_job, req.file_path, output_dir, platforms, req.match_loudness,
        payload={
            "status": JobStatus.PENDING.value,
            "progress": {"total": len(platforms), "done": 0, "current": None},
            "results": [],
            "output_dir": output_dir,
            "error": None,
        }, dedup_key=dedup_key, futures=_platform_futures)
    return {"job_id": job_id, "total": len(platforms), "output_dir": output_dir}


@router.get("/platform_masters/status/{job_id}", response_model=PlatformMastersStatusResponse, response_model_exclude_unset=True)
async def platform_masters_status(job_id: str) -> dict:
    return get_job_or_404(_platform_jobs, job_id)


@router.post("/platform_masters/{job_id}/cancel", response_model=CopilotJobCancelResponse, response_model_exclude_unset=True)
async def platform_masters_cancel(job_id: str) -> dict:
    return cancel_reply(_platform_jobs, job_id, _platform_futures)   # SVC-14
