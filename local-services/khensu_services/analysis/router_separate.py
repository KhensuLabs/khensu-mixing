from __future__ import annotations
import logging
import os
import pathlib
import tempfile
from concurrent.futures import Future

from fastapi import APIRouter

from khensu_services.analysis.models import SeparateRequest, SeparateResponse
from khensu_services.common.api_models import ResponseModel
from khensu_services.common.job_models import JobCancelResponse
from khensu_services.common.job_status import JobStatus
from khensu_services.common.errors import (
    check_decode_budget,
    conflict,
    forbidden_path,
    internal_error,
    not_found,
    require_file,
    timed_out,
    unavailable,
)
from khensu_services.common.executors import separation_executor as _separation_executor
from khensu_services.common.executors import io_route
from khensu_services.common.jobs import (
    JobStore, job_begin, job_cancel_requested, job_settle, job_update, make_job_routes,
    request_fingerprint, start_job, track_future,
)

_log = logging.getLogger(__name__)
router = APIRouter()

# Separation runs on a single-worker executor (only one Demucs/HPSS pass at a
# time); a small admission cap keeps a burst of /separate/start calls from
# piling up an unbounded backlog behind it (audit ARCH-01).
_jobs = JobStore(name="separate", max_active=2)
_job_futures: dict[str, Future] = {}


def _stems_result(res: dict, method: str) -> dict:
    out = {
        "vocals_path": res["vocals"],
        "drums_path": res["drums"],
        "bass_path": res["bass"],
        "other_path": res["other"],
        "method_used": method,
        "separation_quality_score": res.get("separation_quality_score", 0.0),
    }
    for key in ("model", "device"):
        if res.get(key):
            out[key] = res[key]
    return out


def _run_separation_task(job_id: str, file_path: str, output_dir: str, method: str,
                         roformer_model: str = ""):
    # Workers never index _jobs[job_id]: the job can be evicted underneath
    # them (audit M4). job_begin/job_update/job_settle read with .get under
    # the store lock and acknowledge a "cancelling" request as "cancelled".
    #
    # Backend chain (CE-AI-4): "roformer"/"auto" -> RoFormer -> Demucs -> HPSS;
    # "demucs" -> Demucs -> HPSS; "hpss" -> HPSS. Every skipped backend's
    # reason is recorded on the job and in separation.json.
    import khensu_services.analysis.router as main_router
    from khensu_services.analysis import separator as _sep
    from khensu_services.analysis.separator import SeparationCancelled
    if not job_begin(_jobs, job_id):
        return

    def _cancelled() -> bool:
        return job_cancel_requested(_jobs, job_id)

    # Why a later backend ran instead of the requested one, if it did.
    reasons: list[str] = []

    def _combined() -> str | None:
        return "; ".join(reasons) if reasons else None

    try:
        if method in ("roformer", "auto"):
            def _rof_progress(frac: float) -> None:
                job_update(_jobs, job_id, progress=0.9 * frac)

            rof_failure: list[str] = []
            res = _sep.separate_stems_roformer(file_path, output_dir, model=roformer_model or None,
                                               cancel_check=_cancelled, progress_cb=_rof_progress,
                                               on_error=rof_failure.append)
            if res is not None:
                job_settle(_jobs, job_id, status=JobStatus.COMPLETED.value, progress=1.0,
                           result=_stems_result(res, "roformer"))
                return
            rof_reason = rof_failure[-1] if rof_failure else "RoFormer separation did not run"
            reasons.append(f"RoFormer: {rof_reason}")
            job_update(_jobs, job_id, roformer_fallback_reason=rof_reason, progress=0.0)
            _log.warning("RoFormer unavailable (%s) - falling back to Demucs", rof_reason)
            if job_cancel_requested(_jobs, job_id):
                job_settle(_jobs, job_id)
                return

        if method in ("demucs", "roformer", "auto"):
            # Record WHY Demucs can't run (model/deps missing vs. crash) so
            # the UI can say more than "did not complete" (spec §0.5).
            reason = _sep.demucs_unavailable_reason()
            if reason is not None:
                reasons.append(reason if method == "demucs" else f"Demucs: {reason}")
                job_update(_jobs, job_id, demucs_fallback_reason=reason)
                _log.warning("Demucs unavailable (%s) — falling back to HPSS", reason)

            def _progress(frac: float) -> None:
                # AUDIT-047: the only hook inside Demucs' segment loop - a
                # cancel raises here so the single separation worker is
                # released now instead of after minutes of wasted inference.
                if _cancelled():
                    raise SeparationCancelled()
                job_update(_jobs, job_id, progress=frac)

            failure: list[str] = []
            res = None
            if reason is None:
                kwargs = {"progress_cb": _progress, "on_error": failure.append}
                if reasons:   # Demucs is the fallback for a RoFormer request
                    kwargs["fallback_reason"] = _combined()
                res = main_router.separate_stems_demucs(file_path, output_dir, **kwargs)
                if res is None:
                    demucs_reason = (
                        failure[-1] if failure else
                        "Demucs is installed but the separation run failed — "
                        "see the service log for the traceback")
                    reasons.append(demucs_reason if method == "demucs" else f"Demucs: {demucs_reason}")
                    job_update(_jobs, job_id, demucs_fallback_reason=demucs_reason)
            if res is not None:
                job_settle(_jobs, job_id, status=JobStatus.COMPLETED.value, progress=1.0,
                           result=_stems_result(res, "demucs"))
                return
            if job_cancel_requested(_jobs, job_id):
                job_settle(_jobs, job_id)       # acknowledge the cancel
                return

        res = main_router.separate_stems(file_path, output_dir, cancel_check=_cancelled,
                                         fallback_reason=_combined())
        job_settle(_jobs, job_id, status=JobStatus.COMPLETED.value, progress=1.0,
                   result=_stems_result(res, "hpss"))
    except SeparationCancelled:
        job_settle(_jobs, job_id)               # partial stems already removed
    except Exception as exc:
        _log.exception("Background stem separation failed for job %s", job_id)
        job_settle(_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc))
    finally:
        # Belt and braces: never leave a job stuck in "cancelling".
        if job_cancel_requested(_jobs, job_id):
            job_settle(_jobs, job_id)
        # _job_futures cleanup is owned by _track_future's done-callback.


def _track_future(job_id: str, future: Future) -> None:
    """Register a separation future (see ``common.jobs.track_future``)."""
    track_future(_job_futures, job_id, future)


class SeparationStems(ResponseModel):
    vocals_path: str
    drums_path: str
    bass_path: str
    other_path: str
    method_used: str
    separation_quality_score: float = 0.0
    model: str | None = None
    device: str | None = None


class SeparateStartResponse(ResponseModel):
    job_id: str
    status: JobStatus
    output_dir: str


class SeparateStatusResponse(ResponseModel):
    status: JobStatus
    result: SeparationStems | None = None
    error: str | None = None
    progress: float = 0.0
    output_dir: str | None = None
    demucs_fallback_reason: str | None = None
    roformer_fallback_reason: str | None = None


@router.post("/separate/start", response_model=SeparateStartResponse,
             response_model_exclude_unset=True)
@io_route
def separate_start(req: SeparateRequest):
    require_file(req.file_path)
    check_decode_budget(req.file_path)   # io_route: runs on the I/O pool, off the event loop

    # No explicit output dir → AMAS_<M>_<D>_<YYYY>_<Title> beside the source
    # (spec §1.3). Sits inside the source's parent, so it passes the
    # traversal check below by construction.
    output_dir = req.output_dir
    if not output_dir.strip():
        from khensu_services.util.naming import amas_stem_dir
        output_dir = amas_stem_dir(req.file_path)

    file_path_abs = pathlib.Path(req.file_path).resolve()
    output_dir_abs = pathlib.Path(output_dir).resolve()
    try:
        output_dir_abs.relative_to(file_path_abs.parent)
    except ValueError:
        try:
            output_dir_abs.relative_to(pathlib.Path(tempfile.gettempdir()).resolve())
        except ValueError:
            forbidden_path("Forbidden output path traversal detected")

    # AUDIT-024: separating ".../vocals.wav" into its own folder would write the
    # "vocals" stem over the source. 409 before any work starts.
    from khensu_services.analysis.separator import stem_output_paths
    from khensu_services.common.output_paths import reject_output_alias
    for stem_path in stem_output_paths(str(output_dir_abs)).values():
        reject_output_alias(stem_path, req.file_path)

    # Dedup (audit ARCH-01): a double-click on the same (file, method) joins
    # the already-running separation instead of queuing a redundant second
    # pass behind the single-worker separation executor.
    # The resolved output_dir is part of the identity: a request for a
    # different destination is different work, never a join.
    # The source's (mtime_ns, size) is part of it too (SVC-07): a file
    # re-rendered in place mid-job must start a new separation, not join
    # the one reading the old audio (the AUDIT-072 problem).
    st = os.stat(req.file_path)
    dedup_key = request_fingerprint(str(file_path_abs), req.method, req.roformer_model,
                                    str(output_dir_abs), st.st_mtime_ns, st.st_size)
    job_id, _created = start_job(
        _jobs, _separation_executor, _run_separation_task, req.file_path, output_dir, req.method,
        req.roformer_model,
        payload={
            "status": JobStatus.PENDING.value, "result": None, "error": None, "progress": 0.0,
            "output_dir": str(output_dir_abs),
        }, dedup_key=dedup_key, futures=_job_futures)

    return {"job_id": job_id, "status": JobStatus.PENDING.value, "output_dir": str(output_dir_abs)}


# Status + cancel via the shared factory (S11). Cancel: queued work is
# stopped immediately; a running Demucs/HPSS call reports "cancelling" until
# it has released its worker. An unknown id answers ok=false / "unknown"
# (SVC-14) instead of a blanket ok=true.
separate_status, separate_cancel = make_job_routes(
    router, _jobs,
    status_path="/separate/status/{job_id}",
    cancel_path="/separate/{job_id}/cancel",
    status_model=SeparateStatusResponse,
    cancel_model=JobCancelResponse,
    futures=_job_futures)


@router.post("/separate", response_model=SeparateResponse, response_model_exclude_unset=True)
async def separate(req: SeparateRequest) -> SeparateResponse:
    """Legacy separate wrapper that starts the job and polls internally for compatibility."""
    import asyncio
    # separate_start is an io_route: it does its blocking header probing and
    # path resolution on the I/O pool, off the event loop.
    res_start = await separate_start(req)
    job_id = res_start["job_id"]

    # Every terminal state maps to its errors.py status (SVC-08): the old loop
    # leaked the raw exception text in a 500, and spun the full 300 s before
    # a 504 when the job was interrupted or evicted underneath it.
    for _ in range(3000):
        await asyncio.sleep(0.1)
        job = _jobs.get(job_id)
        if job is None:
            not_found("Separation job no longer exists")
        status = job.get("status")
        if status == JobStatus.COMPLETED.value:
            return SeparateResponse(**job["result"])
        if status == JobStatus.FAILED.value:
            # The worker already logged the traceback; the reply stays generic.
            internal_error()
        if status == JobStatus.CANCELLED.value:
            conflict("Separation was cancelled")
        if status == JobStatus.INTERRUPTED.value:
            unavailable("Separation was interrupted by service shutdown")
    # Timed out: don't leave an orphaned separation running for nobody.
    await separate_cancel(job_id)
    timed_out("Separation timed out")
