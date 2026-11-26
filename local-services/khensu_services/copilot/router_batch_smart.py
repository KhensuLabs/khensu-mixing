# local-services/khensu_services/copilot/router_batch_smart.py
"""Per-file smart batch endpoints (ENH-4):

    POST /copilot/batch/smart/start          analyse, fix and master every file on its own terms
    GET  /copilot/batch/smart/status/{id}    per-file progress and the results table
    POST /copilot/batch/smart/{id}/cancel

The work per file is ``copilot/batch_smart.process_file`` (the verified agent
loop, then the mastering loop in the chosen style); every render goes through
the shared safety pipeline. Runs on the long-job pool; cancellation is checked
between steps, so the file in flight stops at its next step boundary and no
further file starts.
"""
from __future__ import annotations

import logging
import os
from typing import Literal

from fastapi import APIRouter
from pydantic import Field, field_validator

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import conflict, invalid_input, not_found
from khensu_services.common.executors import long_job_executor as _long_executor
from khensu_services.common.job_status import JobStatus
from khensu_services.common.jobs import (
    JobStore, cancel_reply, get_job_or_404, job_begin, job_cancel_requested, job_settle,
    job_update, request_fingerprint, start_job,
)
from khensu_services.copilot import batch_smart
from khensu_services.copilot.response_models import CopilotJobCancelResponse, JobStartResponse

_log = logging.getLogger(__name__)
router = APIRouter()

_MAX_FILES = 200

_smart_jobs = JobStore(name="batch_smart", max_active=2)
_smart_futures: dict = {}


class SmartBatchRequest(RequestModel):
    input_dir: str = ""                                                   # folder (non-recursive)
    file_paths: list[str] = Field(default_factory=list, max_length=500)   # and/or explicit files
    output_dir: str = ""        # default: <input_dir or first file's dir>/khensu_smart_batch
    suffix: str = "_smart"
    fix: bool = True            # run the verified fix loop on each file's own findings
    master: bool = False        # then the mastering loop toward the genre in ``style``
    genre_id: str = ""
    genre_action_allowed: bool = False
    abstained_reason: str = ""
    style: Literal["warm", "balanced", "open", "loud"] = "balanced"
    strength: float = Field(default=0.5, ge=0.0, le=1.0)
    # A saved Master Assistant set's Loudness and Ceiling macros: shift the
    # mastering loudness target, override the true-peak ceiling (None = the
    # genre's own).
    loudness_offset_db: float = Field(default=0.0, ge=-6.0, le=6.0)
    ceiling_dbtp: float | None = Field(default=None, ge=-3.0, le=-0.1)
    target_lufs: float = Field(default=-14.0, ge=-40.0, le=-3.0)
    include_loudness: bool = True
    targets: dict[str, list[float]] = Field(default_factory=dict)
    max_steps: int = Field(default=6, ge=1, le=12)
    max_iterations: int = Field(default=5, ge=1, le=20)

    @field_validator("suffix")
    @classmethod
    def _plain_suffix(cls, value: str) -> str:
        if len(value) > 64:
            raise ValueError("suffix too long (max 64 characters)")
        if any(ch in value for ch in ("/", "\\", ":")) or ".." in value:
            raise ValueError("suffix must not contain path separators, '..' or ':'")
        if any(ord(ch) < 32 or ord(ch) == 127 for ch in value):
            raise ValueError("suffix must not contain control characters")
        return value


class SmartBatchStartResponse(JobStartResponse):
    total_files: int
    output_dir: str


class SmartBatchProgress(ResponseModel):
    total: int
    done: int
    failed: int
    current: str | None = None
    stage: str | None = None


class SmartBatchFileResult(ResponseModel):
    file_path: str
    output_path: str = ""
    # completed | unchanged (nothing kept, no file written) | failed
    status: str
    before_lufs: float | None = None
    before_true_peak_dbtp: float | None = None
    after_lufs: float | None = None
    after_true_peak_dbtp: float | None = None
    findings_before: int | None = None
    findings_after: int | None = None
    findings_cleared: int | None = None
    cleared: list[str] = []
    remaining: list[str] = []
    kept_steps: int = 0
    fix_summary: str | None = None
    mastered: bool = False
    master_summary: str | None = None
    error: str | None = None


class SmartBatchStatusResponse(ResponseModel):
    status: JobStatus
    error: str | None = None
    output_dir: str | None = None
    progress: SmartBatchProgress
    results: list[SmartBatchFileResult] = []


def _lookup_genre(genre_id: str) -> dict:
    from khensu_services.copilot.router_mastering_agent import _lookup_genre as lookup
    return lookup(genre_id)


def _render_step_fn(job_id: str):
    """render_step for the agent loop: the shared pipeline, cancellable."""
    from khensu_services.copilot.dsp import apply_pedalboard_chain
    from khensu_services.copilot.router_render import render_through_pipeline

    def render_step(source: str, output: str, effects: list[dict]) -> dict:
        _result, guardrail, _validation = render_through_pipeline(
            source, output,
            lambda quarantine: apply_pedalboard_chain(source, quarantine, effects, normalize=False),
            effects=effects, graph=None,
            should_cancel=lambda: job_cancel_requested(_smart_jobs, job_id))
        return guardrail
    return render_step


def _run_smart_job(job_id: str, req: SmartBatchRequest, files: list[str], output_dir: str) -> None:
    if not job_begin(_smart_jobs, job_id):
        return
    try:
        _run_files(job_id, req, files, output_dir)
    except batch_smart.SmartBatchCancelled:
        job_settle(_smart_jobs, job_id, status=JobStatus.CANCELLED.value)
        return
    except Exception as exc:
        _log.exception("Smart batch job %s failed", job_id)
        job_settle(_smart_jobs, job_id, status=JobStatus.FAILED.value, error=f"Batch failed: {exc}")
        return
    job_settle(_smart_jobs, job_id, status=JobStatus.COMPLETED.value)


def _run_files(job_id: str, req: SmartBatchRequest, files: list[str], output_dir: str) -> None:
    os.makedirs(output_dir, exist_ok=True)
    genre = dict(_lookup_genre(req.genre_id)) if req.master else {}
    if req.master and req.loudness_offset_db:
        genre["lufs_target"] = float(genre.get("lufs_target", -12.0)) + req.loudness_offset_db
    if req.master and req.ceiling_dbtp is not None:
        genre["true_peak_max"] = req.ceiling_dbtp
    render_step = _render_step_fn(job_id)
    should_cancel = lambda: job_cancel_requested(_smart_jobs, job_id)   # noqa: E731
    used: set[str] = set()
    results: list[dict] = []
    done = failed = 0

    def progress(current: str | None, stage: str | None) -> dict:
        return {"total": len(files), "done": done, "failed": failed,
                "current": current, "stage": stage}

    for src in files:
        if should_cancel():
            raise batch_smart.SmartBatchCancelled()
        name = os.path.basename(src)
        stem = os.path.splitext(name)[0]
        out_path = os.path.join(output_dir, batch_smart.unique_output_name(stem, req.suffix, used))
        work_dir = os.path.join(output_dir, f".work_{job_id[:8]}_{len(results) + 1}")
        job_update(_smart_jobs, job_id, progress=progress(name, "starting"))
        try:
            entry = batch_smart.process_file(
                src, out_path, work_dir, fix=req.fix, master=req.master, genre=genre,
                style=req.style, strength=req.strength, target_lufs=req.target_lufs,
                include_loudness=req.include_loudness, targets=req.targets or None,
                max_steps=req.max_steps, max_iterations=req.max_iterations,
                render_step=render_step, should_cancel=should_cancel,
                on_stage=lambda st, n=name: job_update(_smart_jobs, job_id, progress=progress(n, st)))
        except batch_smart.SmartBatchCancelled:
            raise
        except Exception as exc:
            _log.exception("Smart batch failed for %s (job %s)", src, job_id)
            detail = getattr(exc, "detail", None)
            entry = {"file_path": src, "output_path": "", "status": JobStatus.FAILED.value,
                     "error": str(detail) if detail else str(exc)}
        finally:
            batch_smart.remove_tree_quiet(work_dir)
        if entry.get("status") == JobStatus.FAILED.value:
            failed += 1
        done += 1
        results.append(entry)
        job_update(_smart_jobs, job_id, results=list(results), progress=progress(None, None))


def _collect_files(req: SmartBatchRequest) -> list[str]:
    from khensu_services.common.local_paths import require_local_path
    # 403 for a UNC / network path before anything stats it (AUDIT-063).
    require_local_path(req.input_dir, "Folder")
    for p in req.file_paths:
        require_local_path(p, "File")
    require_local_path(req.output_dir, "Output folder")
    files: list[str] = []
    if req.input_dir:
        if not os.path.isdir(req.input_dir):
            not_found(f"Folder not found: {req.input_dir}")
        for name in sorted(os.listdir(req.input_dir)):
            p = os.path.join(req.input_dir, name)
            if os.path.isfile(p) and os.path.splitext(name)[1].lower() in batch_smart.AUDIO_EXTS:
                files.append(p)
    for p in req.file_paths:
        if not os.path.isfile(p):
            not_found(f"File not found: {p}")
        if p not in files:
            files.append(p)
    return files


@router.post("/batch/smart/start", response_model=SmartBatchStartResponse,
             response_model_exclude_unset=True)
async def batch_smart_start(req: SmartBatchRequest) -> dict:
    if not req.fix and not req.master:
        invalid_input("Nothing to do: turn on fixing, mastering or both.")
    if req.master and req.genre_id and not req.genre_action_allowed:
        conflict(req.abstained_reason or
                 "Mastering abstained: confirm the genre targets before mastering to them.")
    files = _collect_files(req)
    if not files:
        invalid_input("No audio files to process (empty folder / file list)")
    if len(files) > _MAX_FILES:
        invalid_input(f"Batch too large: {len(files)} files (max {_MAX_FILES})")
    from khensu_services.copilot.router_render import _file_identity, _require_audio_decode_budget
    await _require_audio_decode_budget(*files, allow_probe_errors=True)

    base_dir = req.input_dir or os.path.dirname(files[0])
    output_dir = req.output_dir or os.path.join(base_dir, "khensu_smart_batch")
    from khensu_services.common.local_paths import require_local_path
    from khensu_services.common.output_paths import validate_output_path
    require_local_path(output_dir, "Output folder")
    validate_output_path(os.path.join(output_dir, "probe.wav"), files[0])

    # Dedup: the same files (unchanged on disk) with every same parameter
    # join the running batch instead of starting a second one.
    dedup_key = (output_dir + "|" + ",".join(sorted(files)) + "|"
                 + request_fingerprint(req, _file_identity(*sorted(files))))
    job_id, _created = start_job(
        _smart_jobs, _long_executor, _run_smart_job, req, files, output_dir,
        futures=_smart_futures, dedup_key=dedup_key, payload={
            "status": JobStatus.PENDING.value, "error": None, "output_dir": output_dir,
            "progress": {"total": len(files), "done": 0, "failed": 0},
            "results": [],
        })
    return {"job_id": job_id, "total_files": len(files), "output_dir": output_dir}


@router.get("/batch/smart/status/{job_id}", response_model=SmartBatchStatusResponse,
            response_model_exclude_unset=True)
async def batch_smart_status(job_id: str) -> dict:
    return get_job_or_404(_smart_jobs, job_id)


@router.post("/batch/smart/{job_id}/cancel", response_model=CopilotJobCancelResponse,
             response_model_exclude_unset=True)
async def batch_smart_cancel(job_id: str) -> dict:
    return cancel_reply(_smart_jobs, job_id, _smart_futures)
